// Observe mode with idle-page READ tracking (Linux): the kernel idle-page
// bitmap is the only way observe mode sees reads (it never applies PROT_NONE).
//
//   observe_read_track_test <libsmash.so> root  [large_only] [thp]
//   observe_read_track_test <libsmash.so> idled <smash-idled> [large_only] [thp]
//
// root:  re-runs itself as root (`sudo -n` if needed) with libsmash preloaded,
//        so smash opens /sys/kernel/mm/page_idle/bitmap itself.
// idled: starts `sudo -n smash-idled -p <this pid>` on a private socket and
//        runs itself UNPRIVILEGED under the preload, as the daemon's
//        launcher's child: the smash-idled client path.
// thp: needs THP "always" (skips otherwise, or if no chunk got a huge page):
// pages then sit in 2 MiB folios, which the bitmap tracks by head PFN only.
// (madvise(MADV_HUGEPAGE) on a malloc'd chunk is not enough: smash hands out
// already-faulted pages, which stay 4 KiB.)
// Exits 77 (skip) without page_idle, without root / passwordless sudo, or if
// smash did not enable read tracking.
//
// The preloaded child (all >= 1 MiB chunks, so both modes manage them):
//   cold     16 MiB written once                      -> projected
//   reread   16 MiB written once, READ (never written) after it is projected
//                                                     -> refaults_read
//   readhot  16 MiB read (never written) every 100 ms -> NOT projected
// Without read tracking readhot would be projected and the rereads missed.
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr size_t kMiB = 1024 * 1024;
constexpr size_t kChunk = 2 * kMiB;
constexpr size_t kNumCold = 8, kNumReread = 8, kNumHot = 8;
const size_t kPage = static_cast<size_t>(sysconf(_SC_PAGESIZE));

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
    size_t e = js.find('"', pos + k.size());
    return js.substr(pos + k.size(), e - pos - k.size());
}

void fill(unsigned char* p, size_t n, unsigned seed) {
    for (size_t i = 0; i < n; ++i)
        p[i] = static_cast<unsigned char>((i % 64) < 8 ? seed + (i / 4096) : (i % 64));
}

volatile unsigned g_sink;
void readAll(unsigned char** r, size_t n) {
    unsigned s = 0;
    for (size_t c = 0; c < n; ++c)
        for (size_t off = 0; off < kChunk; off += kPage) s += r[c][off + 8];
    g_sink = g_sink + s;
}

// The preloaded process.
int child() {
    const char* report = getenv("SMASH_OBSERVE_REPORT");
    if (!report) { fprintf(stderr, "FAIL: no SMASH_OBSERVE_REPORT\n"); return 1; }
    unlink(report);
    unsigned char* cold[kNumCold];
    unsigned char* reread[kNumReread];
    unsigned char* hot[kNumHot];
    const bool thp = getenv("SMASH_RT_THP") != nullptr;
    auto chunk = [&]() {
        if (!thp) return static_cast<unsigned char*>(malloc(kChunk));
        // A huge page needs a 2 MiB-aligned 2 MiB extent: over-allocate and
        // use the aligned middle (never freed; the child just exits).
        auto a = reinterpret_cast<uintptr_t>(malloc(2 * kChunk));
        return reinterpret_cast<unsigned char*>((a + kChunk - 1) & ~static_cast<uintptr_t>(kChunk - 1));
    };
    for (size_t c = 0; c < kNumCold; ++c) { cold[c] = chunk(); fill(cold[c], kChunk, 1 + c); }
    for (size_t c = 0; c < kNumReread; ++c) { reread[c] = chunk(); fill(reread[c], kChunk, 101 + c); }
    for (size_t c = 0; c < kNumHot; ++c) { hot[c] = chunk(); fill(hot[c], kChunk, 201 + c); }
    if (thp) {
        const std::string sm = slurp("/proc/self/smaps_rollup");
        const size_t at = sm.find("AnonHugePages:");
        const long long kb = at == std::string::npos ? 0 : std::atoll(sm.c_str() + at + 14);
        fprintf(stderr, "thp: AnonHugePages %lld kB\n", kb);
        if (kb < 4096) {
            printf("SKIP: chunks not backed by transparent huge pages (%lld kB)\n", kb);
            return 77;
        }
    }
    const long long cold_b = static_cast<long long>((kNumCold + kNumReread) * kChunk);
    const long long hot_b = static_cast<long long>(kNumHot * kChunk);

    std::string js;
    long long would = 0;
    const double t0 = now();
    while (now() - t0 < 45.0) {
        readAll(hot, kNumHot);
        usleep(100 * 1000);
        js = slurp(report);
        if (!js.empty() && jsonStr(js, "read_tracking") == "none" && now() - t0 > 5.0) break;
        would = jsonInt(js, "would_compress_bytes");
        if (would >= cold_b * 8 / 10) break;
    }
    const std::string rt = jsonStr(js, "read_tracking");
    fprintf(stderr, "phase1 (read_tracking=%s): %s", rt.c_str(), js.c_str());
    if (rt.empty() || rt == "none") {
        printf("SKIP: smash did not enable idle-page read tracking (read_tracking=%s)\n", rt.c_str());
        return 77;
    }
    if (would < cold_b * 8 / 10) {
        fprintf(stderr, "FAIL: would_compress_bytes=%lld < 80%% of cold %lld\n", would, cold_b);
        return 1;
    }
    for (int k = 0; k < 30; ++k) { readAll(hot, kNumHot); usleep(100 * 1000); }
    js = slurp(report);
    would = jsonInt(js, "would_compress_bytes");
    if (would > cold_b + hot_b / 4) {
        fprintf(stderr, "FAIL: would_compress_bytes=%lld includes the read-hot region (cold %lld, hot %lld)\n",
                would, cold_b, hot_b);
        return 1;
    }
    // Phase 2: read (never write) the projected reread region.
    const long long rf0 = jsonInt(js, "refaults_read"), wf0 = jsonInt(js, "refaults_write");
    readAll(reread, kNumReread);
    const long long want = static_cast<long long>(kNumReread * kChunk / kPage) * 8 / 10;
    long long rf = rf0;
    const double t1 = now();
    while (now() - t1 < 20.0) {
        readAll(hot, kNumHot);
        usleep(100 * 1000);
        js = slurp(report);
        rf = jsonInt(js, "refaults_read");
        if (rf - rf0 >= want) break;
    }
    fprintf(stderr, "phase2: %s", js.c_str());
    if (rf - rf0 < want) {
        fprintf(stderr, "FAIL: refaults_read grew by %lld, want >= %lld\n", rf - rf0, want);
        return 1;
    }
    if (jsonInt(js, "refaults_write") - wf0 > want / 8) {
        fprintf(stderr, "FAIL: reads counted as write refaults\n");
        return 1;
    }
    printf("PASS: observe read tracking via %s (refaults_read +%lld)\n", rt.c_str(), rf - rf0);
    return 0;
}

int runChild(char** argv, const char* sudo_env_prefix) {
    pid_t p = fork();
    if (p == 0) {
        if (sudo_env_prefix) {
            // sudo resets the environment: pass the preload through env(1).
            std::string lib = std::string("LD_PRELOAD=") + getenv("SMASH_RT_LIB");
            std::string large = std::string("SMASH_LARGE_ONLY=") + getenv("SMASH_LARGE_ONLY");
            std::string rep = std::string("SMASH_OBSERVE_REPORT=") + getenv("SMASH_OBSERVE_REPORT");
            const char* thp = getenv("SMASH_RT_THP") ? "SMASH_RT_THP=1" : "SMASH_RT_NOTHP=1";
            execlp("sudo", "sudo", "-n", "env", lib.c_str(), large.c_str(), rep.c_str(), "SMASH_MODE=observe",
                   "SMASH_DEFER_PHASES_MS=0", "SMASH_COLD_TIMEOUT_SEC=1", "SMASH_OBSERVE_INTERVAL=1",
                   "SMASH_RT_CHILD=1", thp, argv[0], static_cast<char*>(nullptr));
        } else {
            setenv("LD_PRELOAD", getenv("SMASH_RT_LIB"), 1);
            setenv("SMASH_MODE", "observe", 1);
            setenv("SMASH_DEFER_PHASES_MS", "0", 1);
            setenv("SMASH_COLD_TIMEOUT_SEC", "1", 1);
            setenv("SMASH_OBSERVE_INTERVAL", "1", 1);
            setenv("SMASH_RT_CHILD", "1", 1);
            execl(argv[0], argv[0], static_cast<char*>(nullptr));
        }
        _exit(127);
    }
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (getenv("SMASH_RT_CHILD")) return child();
    if (argc < 3) {
        fprintf(stderr, "usage: %s <libsmash> root|idled [smash-idled] [large_only] [thp]\n", argv[0]);
        return 2;
    }
    const std::string how = argv[2];
    const bool idled = how == "idled";
    if (idled && argc < 4) { fprintf(stderr, "idled needs the smash-idled path\n"); return 2; }
    bool large_only = false, thp = false;
    for (int a = idled ? 4 : 3; a < argc; ++a) {
        if (!strcmp(argv[a], "large_only")) large_only = true;
        else if (!strcmp(argv[a], "thp")) thp = true;
    }
    if (thp) {
        const std::string e = slurp("/sys/kernel/mm/transparent_hugepage/enabled");
        if (e.find("[always]") == std::string::npos) {
            printf("SKIP: transparent huge pages not in 'always' mode\n");
            return 77;
        }
        setenv("SMASH_RT_THP", "1", 1);
    }
    if (access("/sys/kernel/mm/page_idle/bitmap", F_OK) != 0) {
        printf("SKIP: kernel lacks CONFIG_IDLE_PAGE_TRACKING\n");
        return 77;
    }
    const bool root = geteuid() == 0;
    if (!root && system("sudo -n true >/dev/null 2>&1") != 0) {
        printf("SKIP: needs root or passwordless sudo\n");
        return 77;
    }
    char cwd[512];
    if (!getcwd(cwd, sizeof(cwd))) return 1;
    const std::string tag = std::string(idled ? "idled" : "root") + (large_only ? "_lo" : "_full") + (thp ? "_thp" : "");
    const std::string report = std::string(cwd) + "/observe_read_" + tag + ".json";
    setenv("SMASH_RT_LIB", argv[1], 1);
    setenv("SMASH_LARGE_ONLY", large_only ? "1" : "0", 1);
    setenv("SMASH_OBSERVE_REPORT", report.c_str(), 1);
    if (!idled) {
        setenv("SMASH_IDLE_READ_TRACK", "1", 1);
        return runChild(argv, root ? nullptr : "sudo");
    }
    if (root) {
        printf("SKIP: the smash-idled case must run unprivileged (it would use the bitmap directly)\n");
        return 77;
    }
    // Daemon scoped to this process: the preloaded child is its descendant.
    // The socket lives in a private 0700 directory (smash-idled chowns the
    // socket to our uid); /tmp because sun_path is short and a build dir on
    // a network mount may not take the root chown.
    char dir[] = "/tmp/smash-idled-test-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    const std::string sock = std::string(dir) + "/s";
    const std::string self = std::to_string(getpid());
    pid_t d = fork();
    if (d == 0) {
        execlp("sudo", "sudo", "-n", argv[3], "-s", sock.c_str(), "-p", self.c_str(), "-v",
               static_cast<char*>(nullptr));
        _exit(127);
    }
    struct stat sb{};
    for (int k = 0; k < 100 && stat(sock.c_str(), &sb) != 0; ++k) usleep(50 * 1000);
    if (stat(sock.c_str(), &sb) != 0) {
        fprintf(stderr, "FAIL: smash-idled did not create %s\n", sock.c_str());
        return 1;
    }
    setenv("SMASH_IDLED_SOCK", sock.c_str(), 1);
    const int rc = runChild(argv, nullptr);
    // The daemon exits once its launcher (us) is gone; nothing to kill. Its
    // socket is removed then; the directory is ours.
    unlink(sock.c_str());
    rmdir(dir);
    return rc;
}
