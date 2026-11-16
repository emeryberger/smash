// Workload for the SMASH_REPORT / SMASH_METRICS_FILE tests
// (tests/check_report.sh). Run under LD_PRELOAD / DYLD_INSERT_LIBRARIES with
// SMASH_COLD_TIMEOUT_SEC=1 in both SMASH_LARGE_ONLY modes.
//
// Allocates compressible large chunks (2 MiB, >= kLargeAllocVmThreshold, so
// smash-managed and compressible in BOTH modes) plus small ones, waits past
// the cold timeout so they compress, reads everything back (decompression
// faults, so the latency histogram is non-empty), waits for one more metrics
// interval and exits normally so the atexit report runs. The checker script
// asserts on the report and the metrics file; this program only checks data.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

int main() {
    constexpr size_t kLarge = 2u << 20, kNumLarge = 16;
    constexpr size_t kSmall = 4096, kNumSmall = 256;
    unsigned char* large[kNumLarge];
    unsigned char* small[kNumSmall];
    for (size_t i = 0; i < kNumLarge; ++i) {
        large[i] = static_cast<unsigned char*>(malloc(kLarge));
        if (!large[i]) return 2;
        memset(large[i], static_cast<int>(i + 1), kLarge);
    }
    for (size_t i = 0; i < kNumSmall; ++i) {
        small[i] = static_cast<unsigned char*>(malloc(kSmall));
        if (!small[i]) return 2;
        memset(small[i], static_cast<int>(i & 0xff), kSmall);
    }
    sleep(6);  // > cold timeout + deferred reclaim + a few ticks
    for (size_t i = 0; i < kNumLarge; ++i)
        for (size_t j = 0; j < kLarge; j += 4096)
            if (large[i][j] != static_cast<unsigned char>(i + 1)) {
                fprintf(stderr, "FAIL: large %zu byte %zu\n", i, j);
                return 1;
            }
    for (size_t i = 0; i < kNumSmall; ++i)
        for (size_t j = 0; j < kSmall; ++j)
            if (small[i][j] != static_cast<unsigned char>(i & 0xff)) {
                fprintf(stderr, "FAIL: small %zu byte %zu\n", i, j);
                return 1;
            }
    sleep(2);  // let a metrics interval (1 s in the test) pass
    printf("report_test: data ok\n");
    return 0;
}
