// Observe (dry-run) mode end-to-end test: SMASH_MODE=observe under
// DYLD_INSERT_LIBRARIES / LD_PRELOAD, registered twice (SMASH_LARGE_ONLY=0/1)
// so full and large-only mode run byte-identical assertions.
//
// Regions (all >= 1 MiB chunks, so they live in the compressible VmRegion in
// BOTH modes):
//   cold     48 MiB, compressible, written once  -> must be projected
//   retouch  16 MiB, compressible, written once, rewritten after it has been
//            projected as reclaimed               -> must count refaults
//   hot      16 MiB, rewritten every 100 ms       -> must NOT be projected
// Small 4 KiB chunks (slab in full mode, system malloc in large-only) are
// written once and read back.
//
// Asserts, from the SMASH_OBSERVE_REPORT JSON (rewritten every second):
//   - would_compress_bytes covers >= 80% of cold+retouch and stays below
//     cold+retouch+hot/2 (the hot region is not projected);
//   - rewriting the retouch region yields refaults_write >= 80% of its pages;
//   - nothing was actually compressed (SIGUSR2 stats: compressed=0 shadow=0)
//     and RSS still holds every region (pages were never dropped);
//   - every byte of every region reads back exactly as written.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <time.h>
#include <unistd.h>

namespace {

constexpr size_t kMiB = 1024 * 1024;
constexpr size_t kChunk = 2 * kMiB;
constexpr size_t kNumCold = 24, kNumRetouch = 8, kNumHot = 8;
constexpr size_t kSmallBytes = 4096, kNumSmall = 256;
static const size_t kPage = static_cast<size_t>(sysconf(_SC_PAGESIZE));

// Compressible but not trivially so: a repeating 64-byte record keyed by seed.
void fill(unsigned char* p, size_t n, unsigned seed) {
    for (size_t i = 0; i < n; ++i)
        p[i] = static_cast<unsigned char>((i % 64) < 8 ? seed + (i / 4096) : (i % 64));
}
bool check(const unsigned char* p, size_t n, unsigned seed, const char* what, size_t idx) {
    for (size_t i = 0; i < n; ++i) {
        unsigned char want = static_cast<unsigned char>((i % 64) < 8 ? seed + (i / 4096) : (i % 64));
        if (p[i] != want) {
            fprintf(stderr, "FAIL: %s chunk %zu byte %zu got 0x%02x want 0x%02x\n",
                    what, idx, i, p[i], want);
            return false;
        }
    }
    return true;
}

double now() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

std::string slurp(const char* path) {
    std::string out;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return out;
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
    close(fd);
    return out;
}

long long jsonInt(const std::string& js, const char* key) {
    std::string k = std::string("\"") + key + "\":";
    size_t pos = js.find(k);
    if (pos == std::string::npos) return -1;
    return std::strtoll(js.c_str() + pos + k.size(), nullptr, 10);
}

void touchHot(unsigned char** hot, unsigned gen) {
    for (size_t c = 0; c < kNumHot; ++c)
        for (size_t off = 0; off < kChunk; off += kPage)
            hot[c][off + 8] = static_cast<unsigned char>(gen + c);  // byte 8: outside fill's key bytes
}

// Capture the SIGUSR2 stats line (SMASH_STATS_SIGNAL=1) via a temp file in cwd.
std::string sigusr2Stats() {
    char path[] = "observe-stats-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return {};
    unlink(path);
    int saved = dup(2);
    dup2(fd, 2);
    kill(getpid(), SIGUSR2);
    usleep(100 * 1000);
    dup2(saved, 2);
    close(saved);
    lseek(fd, 0, SEEK_SET);
    std::string out;
    char buf[1024];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
    close(fd);
    return out;
}

long long statField(const std::string& line, const char* key) {
    std::string k = std::string(" ") + key + "=";
    size_t pos = line.find(k);
    if (pos == std::string::npos) return -1;
    return std::strtoll(line.c_str() + pos + k.size(), nullptr, 10);
}

}  // namespace

int main() {
    const char* report = getenv("SMASH_OBSERVE_REPORT");
    const char* mode = getenv("SMASH_MODE");
    if (!report || !mode || strcmp(mode, "observe") != 0) {
        fprintf(stderr, "FAIL: needs SMASH_MODE=observe and SMASH_OBSERVE_REPORT\n");
        return 1;
    }
    unlink(report);
    const bool large_only = getenv("SMASH_LARGE_ONLY") && getenv("SMASH_LARGE_ONLY")[0] == '1';
    fprintf(stderr, "observe_mode_test: mode=%s\n", large_only ? "large-only" : "full");

    unsigned char* cold[kNumCold];
    unsigned char* retouch[kNumRetouch];
    unsigned char* hot[kNumHot];
    unsigned char* small[kNumSmall];
    for (size_t c = 0; c < kNumCold; ++c) { cold[c] = static_cast<unsigned char*>(malloc(kChunk)); fill(cold[c], kChunk, 1 + c); }
    for (size_t c = 0; c < kNumRetouch; ++c) { retouch[c] = static_cast<unsigned char*>(malloc(kChunk)); fill(retouch[c], kChunk, 101 + c); }
    for (size_t c = 0; c < kNumHot; ++c) { hot[c] = static_cast<unsigned char*>(malloc(kChunk)); fill(hot[c], kChunk, 201 + c); }
    for (size_t c = 0; c < kNumSmall; ++c) { small[c] = static_cast<unsigned char*>(malloc(kSmallBytes)); fill(small[c], kSmallBytes, 7 + c); }

    const long long cold_b = static_cast<long long>((kNumCold + kNumRetouch) * kChunk);
    const long long hot_b = static_cast<long long>(kNumHot * kChunk);

    // Phase 1: wait until cold+retouch are projected as reclaimed.
    unsigned gen = 0;
    std::string js;
    long long would = 0;
    const double t0 = now();
    while (now() - t0 < 45.0) {
        touchHot(hot, ++gen);
        usleep(100 * 1000);
        js = slurp(report);
        would = jsonInt(js, "would_compress_bytes");
        if (would >= cold_b * 8 / 10) break;
    }
    fprintf(stderr, "phase1 report: %s", js.c_str());
    if (would < cold_b * 8 / 10) {
        fprintf(stderr, "FAIL: would_compress_bytes=%lld < 80%% of cold %lld\n", would, cold_b);
        return 1;
    }
    // Let a few more ticks pass with the hot region hot; it must stay out.
    for (int k = 0; k < 30; ++k) { touchHot(hot, ++gen); usleep(100 * 1000); }
    js = slurp(report);
    would = jsonInt(js, "would_compress_bytes");
    if (would > cold_b + hot_b / 2) {
        fprintf(stderr, "FAIL: would_compress_bytes=%lld includes the hot region (cold %lld, hot %lld)\n",
                would, cold_b, hot_b);
        return 1;
    }
    if (jsonInt(js, "projected_saving_bytes") <= 0) {
        fprintf(stderr, "FAIL: projected_saving_bytes <= 0\n");
        return 1;
    }
    // Nothing really compressed or dropped.
    std::string st = sigusr2Stats();
    fprintf(stderr, "stats: %s", st.c_str());
    if (statField(st, "compressed") != 0 || statField(st, "shadow") != 0 ||
        statField(st, "compressing") > 1) {
        fprintf(stderr, "FAIL: observe mode compressed pages for real\n");
        return 1;
    }
    const long long rss = jsonInt(js, "rss_bytes");
    if (rss < (cold_b + hot_b) * 9 / 10) {
        fprintf(stderr, "FAIL: rss_bytes=%lld < 90%% of the allocated footprint %lld (pages dropped?)\n",
                rss, cold_b + hot_b);
        return 1;
    }

    // Phase 2: rewrite the retouch region -> projected refaults.
    const long long rf0 = jsonInt(js, "refaults_write");
    for (size_t c = 0; c < kNumRetouch; ++c) fill(retouch[c], kChunk, 101 + c);  // same bytes, still a write
    const long long want_rf = static_cast<long long>(kNumRetouch * kChunk / kPage) * 8 / 10;
    long long rf = rf0;
    const double t1 = now();
    while (now() - t1 < 20.0) {
        touchHot(hot, ++gen);
        usleep(100 * 1000);
        js = slurp(report);
        rf = jsonInt(js, "refaults_write");
        if (rf - rf0 >= want_rf) break;
    }
    fprintf(stderr, "phase2 report: %s", js.c_str());
    if (rf - rf0 < want_rf) {
        fprintf(stderr, "FAIL: refaults_write grew by %lld, want >= %lld\n", rf - rf0, want_rf);
        return 1;
    }

    // Integrity: observe mode never changes data.
    for (size_t c = 0; c < kNumCold; ++c) if (!check(cold[c], kChunk, 1 + c, "cold", c)) return 1;
    for (size_t c = 0; c < kNumRetouch; ++c) if (!check(retouch[c], kChunk, 101 + c, "retouch", c)) return 1;
    for (size_t c = 0; c < kNumSmall; ++c) if (!check(small[c], kSmallBytes, 7 + c, "small", c)) return 1;
    for (size_t c = 0; c < kNumHot; ++c)
        for (size_t off = 0; off < kChunk; off += kPage)
            if (hot[c][off + 8] != static_cast<unsigned char>(gen + c)) {
                fprintf(stderr, "FAIL: hot chunk %zu page %zu lost a write\n", c, off / kPage);
                return 1;
            }
    for (auto* p : cold) free(p);
    for (auto* p : retouch) free(p);
    for (auto* p : hot) free(p);
    for (auto* p : small) free(p);
    fprintf(stderr, "PASS: observe mode (%s)\n", large_only ? "large-only" : "full");
    return 0;
}
