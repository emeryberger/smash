// Workload for the SMASH_REPORT / SMASH_METRICS_FILE tests
// (tests/check_report.sh). Run under LD_PRELOAD / DYLD_INSERT_LIBRARIES with
// SMASH_COLD_TIMEOUT_SEC=1 in both SMASH_LARGE_ONLY modes.
//
// Allocates compressible large chunks (2 MiB, >= kLargeAllocVmThreshold, so
// smash-managed and compressible in BOTH modes) plus small ones, waits until
// the metrics file reports compressed (memory-released) pages — polling, not
// a fixed sleep, because a loaded macOS CI runner stretches the compressor's
// tick sleeps (60 s cap) — then reads everything back (decompression
// faults, so the latency histogram is non-empty), waits for one more metrics
// interval and exits normally so the atexit report runs. The checker script
// asserts on the report and the metrics file; this program only checks data.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unistd.h>

// smash_compressed_bytes from the SMASH_METRICS_FILE (%p = our pid), or -1.
// Bytes, not pages: smash pages are 16 KiB on macOS arm64, 4 KiB elsewhere.
static long compressedBytesFromMetrics() {
    const char* tmpl = getenv("SMASH_METRICS_FILE");
    if (!tmpl) return -1;
    std::string path(tmpl);
    size_t at = path.find("%p");
    if (at != std::string::npos) path.replace(at, 2, std::to_string(getpid()));
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return -1;
    char line[256];
    long v = -1;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "smash_compressed_bytes %ld", &v) == 1) break;
    fclose(f);
    return v;
}

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
    // Wait for at least half the large chunks' bytes to be compressed and
    // released, so the read-back below takes decompression faults.
    const long want = static_cast<long>(kNumLarge * kLarge / 2);
    const time_t deadline = time(nullptr) + 60;
    long got = -1;
    // Observe mode keeps no blobs, so there is nothing to wait for.
    const char* mode = getenv("SMASH_MODE");
    const bool observe = mode && strcmp(mode, "observe") == 0;
    while (!observe && time(nullptr) < deadline && (got = compressedBytesFromMetrics()) < want)
        usleep(200 * 1000);
    if (!observe && got < want) fprintf(stderr, "report_test: only %ld compressed bytes after 60 s\n", got);
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
