// Parameterized compression test: ONE body, run in both FULL mode
// (SMASH_LARGE_ONLY=0) and LARGE-ONLY mode (SMASH_LARGE_ONLY=1). The two
// ctest registrations (test_malloc_compression, test_large_only_compression)
// point at this same executable and differ ONLY in the SMASH_LARGE_ONLY env
// var, so the two production modes are provably exercised by identical
// assertions — no chance of the modes drifting apart in coverage.
//
// The mode is read from the environment (not a compile flag) precisely so the
// assertions cannot diverge per mode.
//
// Run under DYLD_INSERT_LIBRARIES (macOS) / LD_PRELOAD (Linux) with:
//   SMASH_LARGE_ONLY={0,1}
//   SMASH_COLD_TIMEOUT_SEC=1
//   SMASH_DEFER_PHASES_MS=0
//
// Allocation mix (identical in both modes):
//   - LARGE chunks (2 MiB each, >= kLargeAllocVmThreshold = 1 MiB) — these go
//     through smash's LargeAlloc -> compressible VmRegion in BOTH modes, so
//     compressed>0 holds regardless of mode. (Sub-1 MiB large-allocs would
//     fall to direct mmap and not compress, so the chunk size is deliberate.)
//   - SMALL chunks (4 KiB each, <= kMaxSmallSize = 16 KiB) — in full mode these
//     are smash-managed (slab) and compressible; in large-only mode they pass
//     through to the system allocator. Either way the test only asserts they
//     read back byte-exact, which simultaneously proves (a) full-mode slab
//     integrity and (b) large-only passthrough is never corrupted.
//
// Sequence (identical in both modes):
//   1. malloc large + small compressible chunks, fill each with a per-chunk
//      pattern.
//   2. poll SIGUSR2 stats until compressed>0 (the large chunks compress).
//   3. read every byte of BOTH classes back — integrity through fault
//      decompress (large) and untouched passthrough/slab (small).
//   4. free everything (exercises page_map_ routing in free()).

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <string>
#include <unistd.h>

namespace {

constexpr size_t kLargeBytes = 2 * 1024 * 1024;  // >= 1 MiB => VmRegion (compressible)
constexpr size_t kNumLarge   = 24;               // 48 MiB total compressible
constexpr size_t kSmallBytes = 4 * 1024;         // <= 16 KiB
constexpr size_t kNumSmall   = 256;

void fillChunk(unsigned char* p, size_t bytes, size_t seed) {
    // Highly compressible: a single byte repeated, distinct per chunk.
    std::memset(p, static_cast<int>(seed & 0xff), bytes);
}

bool checkChunk(const unsigned char* p, size_t bytes, size_t seed,
                const char* kind, size_t idx) {
    unsigned char want = static_cast<unsigned char>(seed & 0xff);
    for (size_t j = 0; j < bytes; ++j) {
        if (p[j] != want) {
            fprintf(stderr,
                    "FAIL: %s chunk %zu byte %zu got 0x%02x want 0x%02x\n",
                    kind, idx, j, p[j], want);
            return false;
        }
    }
    return true;
}

// Capture the next SIGUSR2 stats line smash emits to stderr. Redirect fd 2
// across the synchronous kill(), then restore and slurp the temp file.
std::string captureSigusr2Stats() {
    char path[] = "/tmp/smash-mode-stats-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        fprintf(stderr, "FAIL: mkstemp errno=%d\n", errno);
        return {};
    }
    unlink(path);
    int saved = dup(2);
    if (saved < 0) { close(fd); return {}; }
    fflush(stderr);
    if (dup2(fd, 2) < 0) { close(saved); close(fd); return {}; }

    // Signals delivered via kill() to the calling process run synchronously
    // inside kill(); the handler completes (and writes to fd 2) before kill()
    // returns.
    kill(getpid(), SIGUSR2);
    fsync(fd);

    fflush(stderr);
    dup2(saved, 2);
    close(saved);

    lseek(fd, 0, SEEK_SET);
    std::string out;
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    return out;
}

long parseField(const std::string& s, const char* needle) {
    auto pos = s.find(needle);
    if (pos == std::string::npos) return -1;
    pos += std::strlen(needle);
    char* end = nullptr;
    long v = std::strtol(s.c_str() + pos, &end, 10);
    if (end == s.c_str() + pos) return -1;
    return v;
}

}  // namespace

int main() {
    const char* lo = getenv("SMASH_LARGE_ONLY");
    const bool large_only = lo && lo[0] == '1';
    const char* mode = large_only ? "large-only" : "full";
    fprintf(stderr, "mode_compression_test: mode=%s\n", mode);

    // 1. Allocate large (smash-managed in both modes) + small (slab in full,
    //    passthrough in large-only) compressible chunks.
    void** large = static_cast<void**>(std::calloc(kNumLarge, sizeof(void*)));
    void** small = static_cast<void**>(std::calloc(kNumSmall, sizeof(void*)));
    if (!large || !small) {
        fprintf(stderr, "FAIL: calloc chunk arrays\n");
        return 1;
    }
    for (size_t i = 0; i < kNumLarge; ++i) {
        large[i] = std::malloc(kLargeBytes);
        if (!large[i]) { fprintf(stderr, "FAIL: malloc large %zu\n", i); return 1; }
        fillChunk(static_cast<unsigned char*>(large[i]), kLargeBytes, i);
    }
    for (size_t i = 0; i < kNumSmall; ++i) {
        small[i] = std::malloc(kSmallBytes);
        if (!small[i]) { fprintf(stderr, "FAIL: malloc small %zu\n", i); return 1; }
        fillChunk(static_cast<unsigned char*>(small[i]), kSmallBytes, i ^ 0x5a);
    }

    // Extra warmup: small malloc/free churn to push smash past its
    // compression-start threshold even on builds with a high warmup gate.
    for (int i = 0; i < 5000; ++i) {
        void* p = std::malloc(64);
        if (p) std::free(p);
    }

    // 2. Poll for compression. A one-shot sleep+check races the compressor on
    //    slow/contended runners; kick SIGUSR2 once per second and stop as soon
    //    as compression is observed (happy path ~2-3 s, full bound only on a
    //    genuine failure — well inside the 60 s test timeout).
    constexpr int kMaxWaitSec = 30;
    long compressed = -1, committed = -1;
    std::string stats;
    for (int waited = 0; waited < kMaxWaitSec; ++waited) {
        sleep(1);
        stats = captureSigusr2Stats();
        compressed = parseField(stats, "compressed=");
        committed = parseField(stats, "committed=");
        if (compressed > 0) {
            fprintf(stderr, "mode_compression_test: compressed after %ds\n",
                    waited + 1);
            break;
        }
    }

    fprintf(stderr, "mode_compression_test: captured stats:\n  %s",
            stats.empty() ? "<empty>\n" : stats.c_str());
    if (compressed < 0 || committed < 0) {
        fprintf(stderr,
                "FAIL: SIGUSR2 stats missing/unparsable in %s mode. "
                "compressed=%ld committed=%ld. Is the compressor running?\n",
                mode, compressed, committed);
        return 1;
    }
    if (compressed == 0) {
        fprintf(stderr,
                "FAIL: no page compressed within %ds in %s mode "
                "(committed=%ld). Both modes must compress allocations "
                ">= 1 MiB.\n",
                kMaxWaitSec, mode, committed);
        return 1;
    }
    fprintf(stderr,
            "mode_compression_test: compressed=%ld of committed=%ld OK (%s)\n",
            compressed, committed, mode);

    // 3. Integrity: large chunks decompress correctly on fault…
    for (size_t i = 0; i < kNumLarge; ++i) {
        if (!checkChunk(static_cast<unsigned char*>(large[i]),
                        kLargeBytes, i, "large", i)) return 1;
    }
    // …and small chunks (slab in full, system passthrough in large-only) are
    // byte-exact — proving full-mode slab integrity / large-only passthrough
    // is never corrupted.
    for (size_t i = 0; i < kNumSmall; ++i) {
        if (!checkChunk(static_cast<unsigned char*>(small[i]),
                        kSmallBytes, i ^ 0x5a, "small", i)) return 1;
    }
    fprintf(stderr, "mode_compression_test: integrity PASSED (%s)\n", mode);

    // 4. Free both classes (exercises page_map_ routing in free()).
    for (size_t i = 0; i < kNumLarge; ++i) std::free(large[i]);
    for (size_t i = 0; i < kNumSmall; ++i) std::free(small[i]);
    std::free(large);
    std::free(small);

    fprintf(stderr, "mode_compression_test: ALL PASSED (%s)\n", mode);
    return 0;
}
