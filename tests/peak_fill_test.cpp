// peak_fill_test.cpp - eager fill compression bounds the peak of a bulk load.
//
// Run under interposition (LD_PRELOAD / DYLD_INSERT_LIBRARIES) with
// SMASH_EAGER_FILL=1 and the DEFAULT cold timeout (10 s). The program writes
// kTotalMb of compressible data in 1 MiB chunks, paced over kFillSec seconds,
// each chunk written once and then left alone: a fill. With the default
// timeout nothing can reach the cold floor before the fill ends, so without
// eager compression the peak grows by ~1.0x the bytes written. Eager fill
// compresses chunks that stayed quiet for a tick while the heap was growing,
// so the RSS growth over the fill must stay below kMaxPeakFraction of the
// bytes written. 1 MiB chunks clear kLargeAllocVmThreshold, so the same
// assertions hold in full and large-only mode (SMASH_LARGE_ONLY=0/1).
// Finally every byte is read back and compared (fault-decompress of eagerly
// compressed pages must be exact).

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

static constexpr size_t kChunk = 1u << 20;
static constexpr size_t kTotalMb = 192;
static constexpr double kFillSec = 8.0;
// Measured growth/written with eager fill: 0.41 (cloudnew, x86-64 Linux
// 6.12, both modes), 0.31-0.32 (M-series macOS); 1.00-1.01 on both with
// SMASH_EAGER_FILL=0. 0.65 leaves room for CI runners whose ticks slip.
static constexpr double kMaxPeakFraction = 0.65;

static size_t rssBytes() {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return info.resident_size;
    return 0;
#elif defined(__linux__)
    int fd = open("/proc/self/statm", O_RDONLY);
    if (fd < 0) return 0;
    char buf[128];
    ssize_t n = syscall(SYS_read, fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    long sz = 0, res = 0;
    if (sscanf(buf, "%ld %ld", &sz, &res) < 2) return 0;
    return static_cast<size_t>(res) * static_cast<size_t>(sysconf(_SC_PAGESIZE));
#else
    return 0;
#endif
}

// Compressible, chunk-specific content so the read-back is meaningful.
static void fillChunk(uint8_t* p, size_t idx) {
    char line[64];
    int n = snprintf(line, sizeof(line), "chunk %06zu record value data store ", idx);
    for (size_t off = 0; off < kChunk; off += static_cast<size_t>(n))
        memcpy(p + off, line, std::min(static_cast<size_t>(n), kChunk - off));
}

static bool checkChunk(const uint8_t* p, size_t idx) {
    char line[64];
    int n = snprintf(line, sizeof(line), "chunk %06zu record value data store ", idx);
    for (size_t off = 0; off < kChunk; off += static_cast<size_t>(n))
        if (memcmp(p + off, line, std::min(static_cast<size_t>(n), kChunk - off)) != 0)
            return false;
    return true;
}

int main() {
    const char* eager = getenv("SMASH_EAGER_FILL");
    if (!eager || atoi(eager) == 0)  // e.g. to confirm the bound discriminates
        fprintf(stderr, "peak_fill_test: SMASH_EAGER_FILL is off; expect FAIL\n");
    // Start the compressor the way any threaded program would.
    for (int i = 0; i < 2; ++i) std::thread([] { free(malloc(64)); }).join();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    const size_t n = kTotalMb * (1u << 20) / kChunk;
    uint8_t** chunks = static_cast<uint8_t**>(malloc(n * sizeof(uint8_t*)));
    const size_t base = rssBytes();
    size_t peak = base;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < n; ++i) {
        chunks[i] = static_cast<uint8_t*>(malloc(kChunk));
        if (!chunks[i]) { fprintf(stderr, "malloc failed\n"); return 1; }
        fillChunk(chunks[i], i);
        std::this_thread::sleep_until(t0 + std::chrono::duration<double>(kFillSec * (i + 1) / n));
        const size_t r = rssBytes();
        if (r > peak) peak = r;
    }
    const double written = static_cast<double>(n * kChunk);
    const double growth = peak > base ? static_cast<double>(peak - base) : 0.0;
    const double frac = growth / written;
    printf("peak_fill_test: base=%.1f MB peak=%.1f MB written=%.0f MB growth/written=%.2f (max %.2f)\n",
           base / 1048576.0, peak / 1048576.0, written / 1048576.0, frac, kMaxPeakFraction);

    size_t bad = 0;
    for (size_t i = 0; i < n; ++i)
        if (!checkChunk(chunks[i], i)) ++bad;
    for (size_t i = 0; i < n; ++i) free(chunks[i]);
    free(chunks);
    if (bad) {
        printf("FAIL: %zu chunks corrupted after eager compression\n", bad);
        return 1;
    }
    if (frac >= kMaxPeakFraction) {
        printf("FAIL: fill peak not bounded by eager compression\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
