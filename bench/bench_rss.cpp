// bench_rss.cpp - RSS reduction over time using the standard malloc/free API.
//
// Links NO smash internals: run it under an interposing allocator via
//   DYLD_INSERT_LIBRARIES=libsmash.dylib ./bench_rss    (macOS)
//   LD_PRELOAD=libsmash.so ./bench_rss                  (Linux)
// or with no preload for a system-malloc baseline. This mirrors how a real
// application uses smash, and avoids compiling a second static copy of the
// allocator into the benchmark.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
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
    // Direct syscall to avoid Smash's read() interposition.
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

// Peak resident set (VmHWM) since exec; 0 where unavailable.
static size_t getHwmBytes() {
#if defined(__linux__)
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0) return 0;
    char buf[4096];
    ssize_t n = syscall(SYS_read, fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    const char* p = strstr(buf, "VmHWM:");
    size_t kb = 0;
    if (p) sscanf(p, "VmHWM: %zu kB", &kb);
    return kb * 1024;
#else
    return 0;
#endif
}

int main(int argc, char* argv[]) {
    // 1 MiB allocations clear kLargeAllocVmThreshold, so they enter smash's
    // compressible VmRegion in both full and large-only mode.
    size_t alloc_size = 1024 * 1024;
    size_t total_mb = 64;
    // Smash only compresses a page after it stays untouched for the cold
    // timeout (SMASH_COLD_TIMEOUT_SEC, default 10 s). Sample past that so the
    // compression actually shows up; otherwise a 10 s window ends right as the
    // first pages become eligible and the run misleadingly reports ~0%.
    const char* cold_env = getenv("SMASH_COLD_TIMEOUT_SEC");
    int cold_sec = cold_env ? atoi(cold_env) : 10;
    if (cold_sec <= 0) cold_sec = 10;
    int wait_sec = cold_sec + 5;
    // --fill-sec S paces the fill over S seconds (one chunk written once,
    // then left alone), modelling a load phase that is slower than the
    // compressor's tick. 0 (default) writes everything at once.
    double fill_sec = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--alloc-size") == 0 && i + 1 < argc)
            alloc_size = atol(argv[++i]);
        else if (strcmp(argv[i], "--total-mb") == 0 && i + 1 < argc)
            total_mb = atol(argv[++i]);
        else if (strcmp(argv[i], "--wait") == 0 && i + 1 < argc)
            wait_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "--fill-sec") == 0 && i + 1 < argc)
            fill_sec = atof(argv[++i]);
        else if (strcmp(argv[i], "--help") == 0) {
            fprintf(stderr,
                    "Usage: %s [--alloc-size N] [--total-mb N] [--wait SEC] [--fill-sec SEC]\n"
                    "Run under DYLD_INSERT_LIBRARIES / LD_PRELOAD to exercise smash.\n",
                    argv[0]);
            return 0;
        }
    }

    size_t total_bytes = total_mb * 1024 * 1024;
    size_t num_allocs = total_bytes / alloc_size;

    fprintf(stdout, "=== RSS Reduction Benchmark ===\n");
    fprintf(stdout, "Alloc size: %zu, Total: %zu MB, Cold timeout: %d s\n\n",
            alloc_size, total_mb, cold_sec);

    size_t initial_rss = getCurrentRSSBytes();
    fprintf(stdout, "Initial RSS: %.1f MB\n", initial_rss / (1024.0 * 1024.0));

    void** ptrs = static_cast<void**>(malloc(num_allocs * sizeof(void*)));
    const auto t_fill0 = std::chrono::steady_clock::now();
    size_t fill_max_rss = 0;
    for (size_t i = 0; i < num_allocs; ++i) {
        ptrs[i] = malloc(alloc_size);
        if (ptrs[i]) {
            // Fill with a compressible pattern.
            memset(ptrs[i], 0x42, alloc_size);
        }
        if (fill_sec > 0) {
            const auto due = t_fill0 + std::chrono::duration<double>(fill_sec * (i + 1) / num_allocs);
            std::this_thread::sleep_until(due);
            size_t r = getCurrentRSSBytes();
            if (r > fill_max_rss) fill_max_rss = r;
        }
    }

    size_t peak_rss = getCurrentRSSBytes();
    if (fill_max_rss > peak_rss) peak_rss = fill_max_rss;
    fprintf(stdout, "After allocation: %.1f MB (+%.1f MB)\n",
            peak_rss / (1024.0 * 1024.0),
            (peak_rss - initial_rss) / (1024.0 * 1024.0));

    fprintf(stdout, "METRIC fill_peak_rss_mb %.1f\n", peak_rss / (1024.0 * 1024.0));
    fflush(stdout);
    size_t min_rss = peak_rss;
    fprintf(stdout,
            "\nWaiting for compression (cold threshold %d s; sampling %d s)...\n",
            cold_sec, wait_sec);
    if (cold_sec >= 10) {
        fprintf(stdout,
                "  (note: pages first become eligible at t=%ds; set "
                "SMASH_COLD_TIMEOUT_SEC lower to compress sooner)\n",
                cold_sec);
    }
    for (int sec = 1; sec <= wait_sec; ++sec) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        size_t rss = getCurrentRSSBytes();
        double reduction = 1.0 - (static_cast<double>(rss) / peak_rss);
        if (rss < min_rss) min_rss = rss;
        fprintf(stdout, "  t=%2ds: RSS=%.1f MB (%.0f%% reduction from peak)\n",
                sec, rss / (1024.0 * 1024.0), reduction * 100.0);
    }

    // Touch some pages to trigger decompression.
    fprintf(stdout, "\nTouching 10%% of pages...\n");
    for (size_t i = 0; i < num_allocs; i += 10) {
        if (ptrs[i]) {
            volatile uint8_t* p = static_cast<volatile uint8_t*>(ptrs[i]);
            p[0] = 0xFF;  // Trigger fault if compressed
        }
    }

    size_t after_touch = getCurrentRSSBytes();
    fprintf(stdout, "After touching: %.1f MB\n", after_touch / (1024.0 * 1024.0));
    fprintf(stdout, "METRIC min_rss_mb %.1f\n", min_rss / (1024.0 * 1024.0));
    fprintf(stdout, "METRIC hwm_rss_mb %.1f\n", getHwmBytes() / (1024.0 * 1024.0));
    fprintf(stdout, "METRIC workload_done 1\n");
    fflush(stdout);

    for (size_t i = 0; i < num_allocs; ++i) {
        free(ptrs[i]);
    }
    free(ptrs);

    size_t final_rss = getCurrentRSSBytes();
    fprintf(stdout, "After free: %.1f MB\n\n", final_rss / (1024.0 * 1024.0));

    return 0;
}
