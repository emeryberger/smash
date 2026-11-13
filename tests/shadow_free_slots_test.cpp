// Regression test: slab pages with freed slots must complete shadow reclaim.
//
// compressPage() compresses a copy of the page with every free slot zeroed
// (zeroFreeSlots). Shadow reclaim later verifies the page by comparing it with
// the decompressed blob. It used to compare against the raw page, whose freed
// slots still hold the old object bytes, so every page with a freed slot
// failed verification, was discarded back to ACTIVE, recompressed, and failed
// again forever. Pages stayed resident (Redis with 50% of keys deleted:
// 281 MB instead of 98 MB).
//
// Run under LD_PRELOAD / DYLD_INSERT_LIBRARIES (full mode) with:
//   SMASH_COLD_TIMEOUT_SEC=1  SMASH_DEFER_PHASES_MS=0  SMASH_STATS_SIGNAL=1
//
// Sequence:
//   1. malloc kObjs objects of kObjBytes (a slab size class that holds a few
//      objects per page), each filled with a compressible nonzero pattern.
//   2. free every other object, so each page keeps live objects plus freed
//      slots full of stale bytes.
//   3. poll SIGUSR2 stats until most of those pages are COMPRESSED (shadow
//      reclaim finished), not merely COMPRESSED_SHADOW.
//   4. read the survivors back byte-exact and free them.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <string>
#include <unistd.h>

namespace {

constexpr size_t kObjBytes = 2000;  // 2048-byte class: 2 per 4 KiB page
constexpr size_t kObjs = 16384;     // 32 MiB of objects, ~16 MiB live after frees

unsigned char patternByte(size_t i) { return static_cast<unsigned char>(0x40 + (i % 61)); }

std::string captureSigusr2Stats() {
    char path[] = "/tmp/smash-shadow-stats-XXXXXX";
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
    kill(getpid(), SIGUSR2);  // handler runs synchronously inside kill()
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
    void** objs = static_cast<void**>(std::calloc(kObjs, sizeof(void*)));
    if (!objs) { fprintf(stderr, "FAIL: calloc\n"); return 1; }
    for (size_t i = 0; i < kObjs; ++i) {
        objs[i] = std::malloc(kObjBytes);
        if (!objs[i]) { fprintf(stderr, "FAIL: malloc %zu\n", i); return 1; }
        std::memset(objs[i], patternByte(i), kObjBytes);
    }
    // Free every other object: the freed slots keep their old (nonzero) bytes.
    for (size_t i = 0; i < kObjs; i += 2) { std::free(objs[i]); objs[i] = nullptr; }

    // Pages holding the surviving objects. Each survivor's page also holds a
    // freed slot, so all of them depend on the masked comparison.
    const long page = sysconf(_SC_PAGESIZE);
    const long want = static_cast<long>(kObjs * kObjBytes / static_cast<size_t>(page)) / 2;

    constexpr int kMaxWaitSec = 30;
    long compressed = -1, shadow = -1;
    std::string stats;
    for (int waited = 0; waited < kMaxWaitSec; ++waited) {
        sleep(1);
        stats = captureSigusr2Stats();
        compressed = parseField(stats, "compressed=");
        shadow = parseField(stats, "shadow=");
        if (compressed >= want) {
            fprintf(stderr, "shadow_free_slots_test: %ld pages compressed after %ds\n",
                    compressed, waited + 1);
            break;
        }
    }
    fprintf(stderr, "shadow_free_slots_test: stats: %s",
            stats.empty() ? "<empty>\n" : stats.c_str());
    if (compressed < 0) {
        fprintf(stderr, "FAIL: SIGUSR2 stats missing (is SMASH_STATS_SIGNAL=1 set?)\n");
        return 1;
    }
    if (compressed < want) {
        fprintf(stderr,
                "FAIL: only %ld of >= %ld pages with freed slots finished shadow "
                "reclaim within %ds (shadow=%ld). Pages with freed slots fail "
                "the shadow content check.\n",
                compressed, want, kMaxWaitSec, shadow);
        return 1;
    }

    for (size_t i = 1; i < kObjs; i += 2) {
        auto* p = static_cast<const unsigned char*>(objs[i]);
        for (size_t j = 0; j < kObjBytes; ++j) {
            if (p[j] != patternByte(i)) {
                fprintf(stderr, "FAIL: object %zu byte %zu got 0x%02x want 0x%02x\n",
                        i, j, p[j], patternByte(i));
                return 1;
            }
        }
        std::free(objs[i]);
    }
    std::free(objs);
    fprintf(stderr, "shadow_free_slots_test: PASSED\n");
    return 0;
}
