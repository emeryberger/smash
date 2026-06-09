// Verify compression works in LARGE-ONLY mode (SMASH_LARGE_ONLY=1) — the
// production-supported configuration per CLAUDE.md ("the production-supported
// config for concurrent workloads"). malloc_compression_test covers FULL mode;
// this is its large-only sibling, and the only test that exercises
// SMASH_LARGE_ONLY at all.
//
// In large-only mode:
//   - malloc(size <= 16 KiB) -> system malloc passthrough (NOT compressed)
//   - malloc(size  > 16 KiB) -> smash LargeAlloc, which routes to the
//     compressible VmRegion only when alloc_size >= kLargeAllocVmThreshold
//     (1 MiB, large_alloc.h:52); smaller large-allocs go to direct mmap and
//     are NOT compressed. So the "large" chunks here are >= 1 MiB on purpose.
//   - free(ptr) routes by page_map_ membership.
//
// Run under DYLD_INSERT_LIBRARIES / LD_PRELOAD with:
//   SMASH_LARGE_ONLY=1
//   SMASH_COLD_TIMEOUT_SEC=1
//   SMASH_DEFER_PHASES_MS=0
//
// Test sequence:
//   1. Allocate many LARGE compressible chunks (256 KiB each) — these go
//      through smash and must compress.
//   2. Also allocate small chunks (4 KiB) interleaved — these pass through
//      to the system allocator and must remain valid (the point is that
//      large-only mode doesn't corrupt the passthrough path).
//   3. Poll SIGUSR2 stats; assert compressed > 0 (large allocations did
//      get compressed despite small allocs bypassing smash entirely).
//   4. Read every byte of BOTH large and small chunks back — integrity.
//   5. Free everything (exercises the page_map_ routing in free()).

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
constexpr size_t kSmallBytes = 4 * 1024;         // <= 16 KiB => system passthrough
constexpr size_t kNumSmall   = 256;

void fillChunk(unsigned char* p, size_t bytes, size_t seed) {
    // Highly compressible per-chunk constant fill, same scheme as
    // malloc_compression_test so the ratio is comparable.
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

// Capture the next SIGUSR2 stats line smash emits to stderr. (Identical
// mechanism to malloc_compression_test: redirect fd 2 across the synchronous
// kill(), then restore and slurp.)
std::string captureSigusr2Stats() {
    char path[] = "/tmp/smash-largeonly-stats-XXXXXX";
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
    // 1 + 2. Allocate large (smash) and small (passthrough) chunks.
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

    // 3. Poll for compression of the large allocations.
    constexpr int kMaxWaitSec = 30;
    long compressed = -1, committed = -1;
    std::string stats;
    for (int waited = 0; waited < kMaxWaitSec; ++waited) {
        sleep(1);
        stats = captureSigusr2Stats();
        compressed = parseField(stats, "compressed=");
        committed = parseField(stats, "committed=");
        if (compressed > 0) {
            fprintf(stderr,
                    "large_only_compression_test: compressed after %ds\n",
                    waited + 1);
            break;
        }
    }

    fprintf(stderr, "large_only_compression_test: captured stats:\n  %s",
            stats.empty() ? "<empty>\n" : stats.c_str());
    if (compressed < 0 || committed < 0) {
        fprintf(stderr,
                "FAIL: SIGUSR2 stats missing/unparsable. compressed=%ld "
                "committed=%ld. Is the compressor running in large-only mode?\n",
                compressed, committed);
        return 1;
    }
    if (compressed == 0) {
        fprintf(stderr,
                "FAIL: no large allocation compressed within %ds "
                "(committed=%ld). Large-only mode should still compress "
                "allocations > 16 KiB.\n",
                kMaxWaitSec, committed);
        return 1;
    }
    fprintf(stderr,
            "large_only_compression_test: compressed=%ld of committed=%ld OK\n",
            compressed, committed);

    // 4. Integrity: large chunks decompress correctly on fault…
    for (size_t i = 0; i < kNumLarge; ++i) {
        if (!checkChunk(static_cast<unsigned char*>(large[i]),
                        kLargeBytes, i, "large", i)) return 1;
    }
    // …and small (system-malloc) chunks were never disturbed.
    for (size_t i = 0; i < kNumSmall; ++i) {
        if (!checkChunk(static_cast<unsigned char*>(small[i]),
                        kSmallBytes, i ^ 0x5a, "small", i)) return 1;
    }
    fprintf(stderr, "large_only_compression_test: integrity PASSED\n");

    // 5. Free both classes (exercises page_map_ routing in free()).
    for (size_t i = 0; i < kNumLarge; ++i) std::free(large[i]);
    for (size_t i = 0; i < kNumSmall; ++i) std::free(small[i]);
    std::free(large);
    std::free(small);

    fprintf(stderr, "large_only_compression_test: ALL PASSED\n");
    return 0;
}
