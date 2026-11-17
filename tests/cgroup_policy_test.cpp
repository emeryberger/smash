// End-to-end SMASH_CGROUP policy test, run under interposition in BOTH modes
// (test_cgroup_policy_full: SMASH_LARGE_ONLY=0, test_cgroup_policy_large_only:
// SMASH_LARGE_ONLY=1 — same executable, mode from the env).
//
// ctest points SMASH_CGROUP_ROOT at a fake cgroup directory (written by the
// cgroup_fake_setup fixture, tests/make_fake_cgroup.cmake) whose memory.current
// sits 1 MiB under a 100 MiB memory.max: the compressor must detect the cap at
// startup, sample it every tick and run at the CRITICAL pressure level. The
// test asserts, via the SIGUSR2 stats line:
//   * cg=2/...   policy active and at the critical level;
//   * compressed>0 — the pressure policy (skip non-resident pages, keep the
//     blob store warm) still compresses resident idle pages;
// then verifies a byte-exact read-back of large (compressible in both modes)
// and small (slab in full / passthrough in large-only) chunks.
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <string>
#include <unistd.h>

namespace {

constexpr size_t kLargeBytes = 2 * 1024 * 1024;
constexpr size_t kNumLarge = 16;
constexpr size_t kSmallBytes = 4 * 1024;
constexpr size_t kNumSmall = 256;

void fill(unsigned char* p, size_t n, size_t seed) {
    // Compressible but not same-filled: a short repeating ramp per chunk.
    for (size_t j = 0; j < n; ++j) p[j] = static_cast<unsigned char>((seed + (j % 61)) & 0xff);
}

bool check(const unsigned char* p, size_t n, size_t seed, const char* kind, size_t idx) {
    for (size_t j = 0; j < n; ++j) {
        unsigned char want = static_cast<unsigned char>((seed + (j % 61)) & 0xff);
        if (p[j] != want) {
            fprintf(stderr, "FAIL: %s chunk %zu byte %zu got 0x%02x want 0x%02x\n",
                    kind, idx, j, p[j], want);
            return false;
        }
    }
    return true;
}

std::string captureStats() {
    char path[] = "/tmp/smash-cgpolicy-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return {};
    unlink(path);
    int saved = dup(2);
    fflush(stderr);
    dup2(fd, 2);
    kill(getpid(), SIGUSR2);   // handler runs synchronously inside kill()
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    lseek(fd, 0, SEEK_SET);
    std::string out;
    char buf[4096];
    for (ssize_t n; (n = read(fd, buf, sizeof(buf))) > 0;) out.append(buf, static_cast<size_t>(n));
    close(fd);
    return out;
}

long field(const std::string& s, const char* needle) {
    auto pos = s.find(needle);
    if (pos == std::string::npos) return -1;
    return std::strtol(s.c_str() + pos + std::strlen(needle), nullptr, 10);
}

}  // namespace

int main() {
    const char* lo = getenv("SMASH_LARGE_ONLY");
    const char* mode = (lo && lo[0] == '1') ? "large-only" : "full";
    const char* root = getenv("SMASH_CGROUP_ROOT");
    fprintf(stderr, "cgroup_policy_test: mode=%s SMASH_CGROUP_ROOT=%s\n", mode, root ? root : "(unset)");
    if (!root) { fprintf(stderr, "FAIL: SMASH_CGROUP_ROOT not set by ctest\n"); return 1; }

    void* large[kNumLarge];
    void* small[kNumSmall];
    for (size_t i = 0; i < kNumLarge; ++i) {
        large[i] = malloc(kLargeBytes);
        if (!large[i]) return 1;
        fill(static_cast<unsigned char*>(large[i]), kLargeBytes, i);
    }
    for (size_t i = 0; i < kNumSmall; ++i) {
        small[i] = malloc(kSmallBytes);
        if (!small[i]) return 1;
        fill(static_cast<unsigned char*>(small[i]), kSmallBytes, i ^ 0x5a);
    }
    for (int i = 0; i < 5000; ++i) free(malloc(64));

    std::string stats;
    long compressed = -1;
    for (int waited = 0; waited < 30; ++waited) {
        sleep(1);
        stats = captureStats();
        compressed = field(stats, "compressed=");
        if (compressed > 0 && field(stats, "cg=") == 2) break;
    }
    fprintf(stderr, "cgroup_policy_test: stats: %s", stats.empty() ? "<empty>\n" : stats.c_str());
    long level = field(stats, "cg=");
    if (level != 2) {
        fprintf(stderr, "FAIL: cgroup policy level %ld, want 2 (critical; -1 = inactive)\n", level);
        return 1;
    }
    if (compressed <= 0) {
        fprintf(stderr, "FAIL: nothing compressed under the pressure policy (%s)\n", mode);
        return 1;
    }
    for (size_t i = 0; i < kNumLarge; ++i)
        if (!check(static_cast<unsigned char*>(large[i]), kLargeBytes, i, "large", i)) return 1;
    for (size_t i = 0; i < kNumSmall; ++i)
        if (!check(static_cast<unsigned char*>(small[i]), kSmallBytes, i ^ 0x5a, "small", i)) return 1;
    for (size_t i = 0; i < kNumLarge; ++i) free(large[i]);
    for (size_t i = 0; i < kNumSmall; ++i) free(small[i]);
    fprintf(stderr, "cgroup_policy_test: ALL PASSED (%s, compressed=%ld)\n", mode, compressed);
    return 0;
}
