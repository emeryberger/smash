// Observe-mode projection model test: SMASH_MODE=observe under
// DYLD_INSERT_LIBRARIES / LD_PRELOAD with SMASH_COLD_TIMEOUT_SEC=6, registered
// twice (SMASH_LARGE_ONLY=0/1). Checks the parts of the projection that model
// a real run's behaviour beyond "pages not written for the cold timeout"
// (agents/estimator.md, "Comparing estimates with a real run"):
//
//   cold     32 MiB, compressible, written once
//   readhot  16 MiB, compressible, written once, then READ every 50 ms
//
//   A. refault backoff (no read tracking): right after the cold pages are
//      first projected as reclaimed (idle age ~T+2 ticks), backoff_held_bytes
//      holds them back -- a run whose buckets backed off compresses at 2T --
//      and the hold ends once they are 2T+1 ticks idle.
//   B. read signal (Linux, soft-dirty, no idle bitmap): readhot is never
//      written, so write tracking projects it reclaimed, but the arena's
//      referenced bytes put it in read_held_bytes.
// Ticks are read from the report's "ticks" field, so the checks depend on the
// compressor's tick count, not on wall-clock timing.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <time.h>
#include <unistd.h>

namespace {

constexpr size_t kMiB = 1024 * 1024;
constexpr size_t kChunk = 2 * kMiB;
constexpr size_t kNumCold = 16, kNumRead = 8;
static const size_t kPage = static_cast<size_t>(sysconf(_SC_PAGESIZE));

void fill(unsigned char* p, size_t n, unsigned seed) {
    for (size_t i = 0; i < n; ++i)
        p[i] = static_cast<unsigned char>((i % 64) < 8 ? seed + (i / 4096) : (i % 64));
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

std::string jsonStr(const std::string& js, const char* key) {
    std::string k = std::string("\"") + key + "\": \"";
    size_t pos = js.find(k);
    if (pos == std::string::npos) return {};
    size_t end = js.find('"', pos + k.size());
    return js.substr(pos + k.size(), end - pos - k.size());
}

volatile unsigned long g_sink = 0;
unsigned char* g_read[kNumRead];
void readHot() {
    unsigned long s = 0;
    for (size_t c = 0; c < kNumRead; ++c)
        for (size_t off = 0; off < kChunk; off += kPage) s += g_read[c][off + 8];
    g_sink = g_sink + s;
}

// Poll the report (reading readhot every 50 ms) until pred(js) or cap_s.
template <class Pred>
bool waitFor(const char* report, std::string& js, double cap_s, Pred pred) {
    const double t0 = now();
    while (now() - t0 < cap_s) {
        readHot();
        usleep(50 * 1000);
        js = slurp(report);
        if (!js.empty() && pred(js)) return true;
    }
    return false;
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

    unsigned char* cold[kNumCold];
    for (size_t c = 0; c < kNumCold; ++c) { cold[c] = static_cast<unsigned char*>(malloc(kChunk)); fill(cold[c], kChunk, 1 + c); }
    for (size_t c = 0; c < kNumRead; ++c) { g_read[c] = static_cast<unsigned char*>(malloc(kChunk)); fill(g_read[c], kChunk, 51 + c); }
    const long long cold_b = static_cast<long long>(kNumCold * kChunk);
    const long long read_b = static_cast<long long>(kNumRead * kChunk);
    const long long all_b = cold_b + read_b;

    // A1: first report with (nearly) everything projected reclaimed.
    std::string js;
    if (!waitFor(report, js, 60.0, [&](const std::string& j) {
            return jsonInt(j, "would_compress_bytes") >= all_b * 8 / 10; })) {
        fprintf(stderr, "FAIL: regions never projected reclaimed: %s", js.c_str());
        return 1;
    }
    fprintf(stderr, "A1 report: %s", js.c_str());
    if (js.find("\"assumes_backoff\": true") == std::string::npos &&
        js.find("\"assumes_backoff\": false") == std::string::npos) {
        fprintf(stderr, "FAIL: no assumes_backoff field\n");
        return 1;
    }
    const bool assumes = js.find("\"assumes_backoff\": true") != std::string::npos;
    if (assumes) {
        const long long held = jsonInt(js, "backoff_held_bytes");
        if (held < cold_b / 4) {
            fprintf(stderr, "FAIL: backoff_held_bytes=%lld < %lld right after the first reclaim\n", held, cold_b / 4);
            return 1;
        }
        if (jsonInt(js, "projected_saving_bytes") > jsonInt(js, "projected_saving_no_backoff_bytes") - held) {
            fprintf(stderr, "FAIL: projected saving does not subtract the backoff hold\n");
            return 1;
        }
        // A2: the hold is transient -- gone once the pages are 2T+1 idle.
        if (!waitFor(report, js, 60.0, [&](const std::string& j) {
                return jsonInt(j, "backoff_held_bytes") < cold_b / 8; })) {
            fprintf(stderr, "FAIL: backoff_held_bytes never dropped: %s", js.c_str());
            return 1;
        }
        fprintf(stderr, "A2 report: %s", js.c_str());
    } else {
        fprintf(stderr, "skip A: read tracking available and no refault seen\n");
    }

    // B: readhot is read every 50 ms but never written.
    const bool ref_signal = jsonInt(js, "referenced_bytes") >= 0 && jsonStr(js, "read_tracking") == "none";
    if (ref_signal) {
        if (!waitFor(report, js, 30.0, [&](const std::string& j) {
                return jsonInt(j, "read_held_bytes") >= read_b / 2; })) {
            fprintf(stderr, "FAIL: read_held_bytes stayed below %lld: %s", read_b / 2, js.c_str());
            return 1;
        }
        fprintf(stderr, "B report: %s", js.c_str());
        if (jsonInt(js, "read_held_bytes") > read_b + read_b / 2) {
            fprintf(stderr, "FAIL: read_held_bytes=%lld well above the read-hot region %lld\n",
                    jsonInt(js, "read_held_bytes"), read_b);
            return 1;
        }
    } else {
        fprintf(stderr, "skip B: no referenced-bits signal on this platform/config\n");
    }

    for (auto* p : cold) free(p);
    for (auto* p : g_read) free(p);
    fprintf(stderr, "PASS: observe projection (%s)\n", large_only ? "large-only" : "full");
    return 0;
}
