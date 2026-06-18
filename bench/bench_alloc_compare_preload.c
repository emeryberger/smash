#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/syscall.h>

#define NUM_SMALL  200000
#define NUM_MEDIUM  50000
#define NUM_LARGE   10000

static size_t get_rss_kb() {
    char buf[4096];
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = syscall(SYS_read, fd, buf, sizeof(buf)-1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    char* p = strstr(buf, "VmRSS:");
    if (!p) return 0;
    size_t kb = 0;
    sscanf(p, "VmRSS: %zu", &kb);
    return kb;
}

int main() {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    void** small_ptrs = (void**)calloc(NUM_SMALL, sizeof(void*));
    void** med_ptrs = (void**)calloc(NUM_MEDIUM, sizeof(void*));
    void** large_ptrs = (void**)calloc(NUM_LARGE, sizeof(void*));

    for (int i = 0; i < NUM_SMALL; i++) {
        small_ptrs[i] = malloc(256);
        if (i % 3 == 0) memset(small_ptrs[i], 0, 256);
        else if (i % 3 == 1) memset(small_ptrs[i], 0x42, 256);
        else {
            char* p = (char*)small_ptrs[i];
            for (int j = 0; j < 256; j++) p[j] = (char)((i*7+j*13) & 0xFF);
        }
    }
    for (int i = 0; i < NUM_MEDIUM; i++) {
        med_ptrs[i] = malloc(1024);
        memset(med_ptrs[i], i & 0xFF, 1024);
    }
    for (int i = 0; i < NUM_LARGE; i++) {
        large_ptrs[i] = malloc(8192);
        memset(large_ptrs[i], 0, 8192);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double alloc_ms = (t1.tv_sec - t0.tv_sec)*1000.0 + (t1.tv_nsec - t0.tv_nsec)/1e6;
    size_t peak_rss = get_rss_kb();
    printf("Alloc: %.1f ms, Peak RSS: %.1f MB\n", alloc_ms, peak_rss/1024.0);

    printf("Cooling 12s...\n");
    for (int t = 1; t <= 12; t++) {
        usleep(1000000);
        if (t % 4 == 0) {
            size_t rss = get_rss_kb();
            printf("  t=%ds: RSS=%.1f MB\n", t, rss/1024.0);
        }
    }
    size_t cool_rss = get_rss_kb();
    double reduction = (1.0 - (double)cool_rss / peak_rss) * 100;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    volatile long sum = 0;
    for (int i = 0; i < NUM_SMALL; i += 10)
        sum += ((char*)small_ptrs[i])[0];
    for (int i = 0; i < NUM_MEDIUM; i += 10)
        sum += ((char*)med_ptrs[i])[0];
    for (int i = 0; i < NUM_LARGE; i += 10)
        sum += ((char*)large_ptrs[i])[0];
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double access_ms = (t1.tv_sec - t0.tv_sec)*1000.0 + (t1.tv_nsec - t0.tv_nsec)/1e6;
    (void)sum;

    size_t final_rss = get_rss_kb();

    printf("\n=== Results ===\n");
    printf("Alloc time:     %.1f ms\n", alloc_ms);
    printf("Peak RSS:       %.1f MB\n", peak_rss/1024.0);
    printf("Cool RSS:       %.1f MB (%.1f%% reduction)\n", cool_rss/1024.0, reduction);
    printf("Cold access:    %.2f ms (%d objects sampled)\n", access_ms,
           NUM_SMALL/10 + NUM_MEDIUM/10 + NUM_LARGE/10);
    printf("Final RSS:      %.1f MB\n", final_rss/1024.0);

    // SIGUSR2 triggers smash stats dump (no-op without smash)
    signal(SIGUSR2, SIG_IGN);
    kill(getpid(), SIGUSR2);
    usleep(200000);

    for (int i = 0; i < NUM_SMALL; i++) free(small_ptrs[i]);
    for (int i = 0; i < NUM_MEDIUM; i++) free(med_ptrs[i]);
    for (int i = 0; i < NUM_LARGE; i++) free(large_ptrs[i]);
    free(small_ptrs); free(med_ptrs); free(large_ptrs);
    return 0;
}
