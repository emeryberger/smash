// Verify the malloc-routed allocation path actually compresses pages
// under a live compressor — the headline path the rest of smash exists
// to optimize. external_mapping_test covers the mmap / Mach VM
// surfaces; this one covers the slab / large-alloc surface.
//
// Run under DYLD_INSERT_LIBRARIES=...libsmash.dylib (macOS) or
// LD_PRELOAD=...libsmash.so (Linux). Required env:
//
//   SMASH_LARGE_ONLY=0
//   SMASH_COLD_TIMEOUT_SEC=1
//   SMASH_DEFER_PHASES_MS=0
//
// Test sequence:
//   1. malloc enough total bytes that some pages can compress
//      (~32 MiB across ~512 chunks — each chunk multi-page so it lands
//       in slab or large-alloc paths)
//   2. fill each chunk with a deterministic per-chunk pattern
//   3. sleep ≥ 2 × COLD_TIMEOUT_SEC so the compressor processes them
//   4. send SIGUSR2 to self; smash's stats handler dumps a line like
//      "[smash stats] pid=N committed=K active=A ... compressed=C ..."
//      Parse it and assert compressed > 0.
//   5. read every byte back — verify integrity (decompresses on fault)
//   6. free everything

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <unistd.h>

namespace {

constexpr size_t kChunkBytes = 64 * 1024;  // 64 KiB → multi-page on any OS
constexpr size_t kNumChunks  = 512;        // 32 MiB total
constexpr size_t kSleepSec   = 5;          // ≥ 2 × COLD_TIMEOUT_SEC=1

void fillChunk(unsigned char* p, size_t bytes, size_t chunk_index) {
    // Highly compressible: a single byte repeated. The whole region
    // therefore looks like 512 distinct constant pages — every
    // compressor algorithm collapses it to ~tens of bytes.
    std::memset(p, static_cast<int>(chunk_index & 0xff), bytes);
}

bool checkChunk(const unsigned char* p, size_t bytes, size_t chunk_index) {
    unsigned char want = static_cast<unsigned char>(chunk_index & 0xff);
    for (size_t j = 0; j < bytes; ++j) {
        if (p[j] != want) {
            fprintf(stderr,
                    "FAIL: chunk %zu byte %zu got 0x%02x want 0x%02x\n",
                    chunk_index, j, p[j], want);
            return false;
        }
    }
    return true;
}

// Capture the next SIGUSR2 stats line emitted by smash to stderr.
// Redirects fd 2 to a temp file across the kill() call, then restores.
// Returns the captured text, or an empty string on failure.
std::string captureSigusr2Stats() {
    char path[] = "/tmp/smash-malloc-stats-XXXXXX";
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

    // Signals delivered via kill() to the calling process run
    // synchronously inside kill(); the handler completes before kill()
    // returns. write(2, ...) inside the handler lands in our temp file.
    kill(getpid(), SIGUSR2);
    fsync(fd);

    fflush(stderr);
    dup2(saved, 2);
    close(saved);

    // Slurp captured contents.
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

// Parse "compressed=N" from a smash stats line.
// Returns -1 if the field is missing.
long parseCompressed(const std::string& s) {
    const char* needle = "compressed=";
    auto pos = s.find(needle);
    if (pos == std::string::npos) return -1;
    pos += std::strlen(needle);
    char* end = nullptr;
    long v = std::strtol(s.c_str() + pos, &end, 10);
    if (end == s.c_str() + pos) return -1;
    return v;
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
    // 1. Allocate.
    void** chunks = static_cast<void**>(std::calloc(kNumChunks, sizeof(void*)));
    if (!chunks) {
        fprintf(stderr, "FAIL: calloc chunks\n");
        return 1;
    }
    for (size_t i = 0; i < kNumChunks; ++i) {
        chunks[i] = std::malloc(kChunkBytes);
        if (!chunks[i]) {
            fprintf(stderr, "FAIL: malloc chunk %zu\n", i);
            return 1;
        }
        fillChunk(static_cast<unsigned char*>(chunks[i]), kChunkBytes, i);
    }

    // Extra warmup: 5000 small mallocs to push smash past its
    // compression-start threshold even on builds with a high warmup gate.
    for (int i = 0; i < 5000; ++i) {
        void* p = std::malloc(64);
        if (p) std::free(p);
    }

    // 2. Wait for the compressor.
    fprintf(stderr, "malloc_compression_test: sleeping %zus for compressor\n",
            kSleepSec);
    sleep(kSleepSec);

    // 3. Stats: assert compressed > 0.
    std::string stats = captureSigusr2Stats();
    fprintf(stderr, "malloc_compression_test: captured stats:\n  %s",
            stats.empty() ? "<empty>\n" : stats.c_str());
    long compressed = parseCompressed(stats);
    long active = parseField(stats, "active=");
    long committed = parseField(stats, "committed=");
    if (compressed < 0 || active < 0 || committed < 0) {
        fprintf(stderr,
                "FAIL: SIGUSR2 stats missing or unparsable. "
                "compressed=%ld active=%ld committed=%ld\n",
                compressed, active, committed);
        return 1;
    }
    if (compressed == 0) {
        fprintf(stderr,
                "FAIL: compressor never compressed any page "
                "(committed=%ld active=%ld). Either smash didn't start "
                "the compressor, or COLD_TIMEOUT_SEC was too high for "
                "the test sleep duration.\n",
                committed, active);
        return 1;
    }
    fprintf(stderr,
            "malloc_compression_test: compressed=%ld of committed=%ld OK\n",
            compressed, committed);

    // 4. Read-back / decompress integrity check.
    for (size_t i = 0; i < kNumChunks; ++i) {
        if (!checkChunk(static_cast<unsigned char*>(chunks[i]),
                        kChunkBytes, i)) {
            return 1;
        }
    }
    fprintf(stderr, "malloc_compression_test: integrity PASSED\n");

    // 5. Free.
    for (size_t i = 0; i < kNumChunks; ++i) std::free(chunks[i]);
    std::free(chunks);

    fprintf(stderr, "malloc_compression_test: ALL PASSED\n");
    return 0;
}
