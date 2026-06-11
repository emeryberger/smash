// bench_rss_standalone.cpp - RSS reduction over time using plain malloc/free.
// No smash link dependency — compare allocators via LD_PRELOAD / DYLD_INSERT_LIBRARIES.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#endif

static size_t getCurrentRSSBytes() {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return info.resident_size;
    }
    return 0;
#elif defined(__linux__)
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0) return 0;
    char buf[4096];
    ssize_t n = syscall(SYS_read, fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    const char* p = strstr(buf, "VmRSS:");
    if (!p) return 0;
    size_t kb = 0;
    sscanf(p, "VmRSS: %zu kB", &kb);
    return kb * 1024;
#else
    return 0;
#endif
}

int main(int argc, char* argv[]) {
    size_t alloc_size = 1024 * 1024;  // 1 MB: clears kLargeAllocVmThreshold in both full and large-only mode
    size_t total_mb = 64;
    int wait_sec = 10;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--alloc-size") == 0 && i + 1 < argc)
            alloc_size = atol(argv[++i]);
        else if (strcmp(argv[i], "--total-mb") == 0 && i + 1 < argc)
            total_mb = atol(argv[++i]);
        else if (strcmp(argv[i], "--wait") == 0 && i + 1 < argc)
            wait_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "Usage: %s [--alloc-size N] [--total-mb N] [--wait SEC]\n", argv[0]);
            return 0;
        }
    }

    size_t total_bytes = total_mb * 1024 * 1024;
    size_t num_allocs = total_bytes / alloc_size;

    fprintf(stdout, "=== RSS Reduction Benchmark (standalone) ===\n");
    fprintf(stdout, "Alloc size: %zu, Total: %zu MB, Wait: %d s\n\n",
            alloc_size, total_mb, wait_sec);

    size_t initial_rss = getCurrentRSSBytes();
    fprintf(stdout, "Initial RSS: %.1f MB\n", initial_rss / (1024.0 * 1024.0));

    void** ptrs = static_cast<void**>(malloc(num_allocs * sizeof(void*)));
    for (size_t i = 0; i < num_allocs; ++i) {
        ptrs[i] = malloc(alloc_size);
        if (ptrs[i]) {
            memset(ptrs[i], 0x42, alloc_size);
        }
    }

    size_t peak_rss = getCurrentRSSBytes();
    fprintf(stdout, "After allocation: %.1f MB (+%.1f MB)\n",
            peak_rss / (1024.0 * 1024.0),
            (peak_rss - initial_rss) / (1024.0 * 1024.0));

    fprintf(stdout, "\nWaiting for compression (sampling RSS every second)...\n");
    for (int sec = 1; sec <= wait_sec; ++sec) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        size_t rss = getCurrentRSSBytes();
        double reduction = 1.0 - (static_cast<double>(rss) / peak_rss);
        fprintf(stdout, "  t=%2ds: RSS=%.1f MB (%.0f%% reduction from peak)\n",
                sec, rss / (1024.0 * 1024.0), reduction * 100.0);
    }

    fprintf(stdout, "\nTouching 10%% of pages...\n");
    for (size_t i = 0; i < num_allocs; i += 10) {
        if (ptrs[i]) {
            volatile uint8_t* p = static_cast<volatile uint8_t*>(ptrs[i]);
            p[0] = 0xFF;
        }
    }

    size_t after_touch = getCurrentRSSBytes();
    fprintf(stdout, "After touching: %.1f MB\n", after_touch / (1024.0 * 1024.0));

    for (size_t i = 0; i < num_allocs; ++i) {
        free(ptrs[i]);
    }
    free(ptrs);

    size_t final_rss = getCurrentRSSBytes();
    fprintf(stdout, "After free: %.1f MB\n\n", final_rss / (1024.0 * 1024.0));

    return 0;
}
