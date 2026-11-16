// fork() safety stress (issue #61), Linux, under LD_PRELOAD, both modes.
//
// Worker threads churn malloc/free/realloc across size classes and read a
// compressed cold set (decompress faults: per-page locks + CompressStore shard
// locks held) while the main thread forks repeatedly. Each child allocates and
// frees across size classes, reads and verifies the whole cold set (every
// reclaimed page faults in the child, which has no uffd and takes the classic
// path), then _exit(0)s. A child that does not finish within 20 s inherited a
// lock held by a thread that no longer exists: the test fails (and kills it).
//
//   fork_stress_test [forks] [threads]      (defaults 120, 6)
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

constexpr size_t kColdBytes = 16u << 20;
unsigned char* g_cold;
std::atomic<bool> g_stop{false};
std::atomic<int> g_fail{0};

unsigned char patByte(size_t off) { return static_cast<unsigned char>(1 + ((off / 80) % 241)); }

uint64_t xorshift(uint64_t& s) {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return s;
}
size_t pickSize(uint64_t& r) {
    const uint64_t k = xorshift(r) % 100;
    if (k < 75) return 8 + xorshift(r) % 2048;
    if (k < 95) return 2048 + xorshift(r) % (60 * 1024);
    return (1u << 20) + xorshift(r) % (1u << 20);
}

void* worker(void* arg) {
    uint64_t r = reinterpret_cast<uint64_t>(arg) | 1;
    void* slot[256] = {};
    while (!g_stop.load(std::memory_order_relaxed)) {
        const int i = static_cast<int>(xorshift(r) % 256);
        if (slot[i]) {
            if (xorshift(r) % 4 == 0) {
                void* q = realloc(slot[i], pickSize(r));
                if (q) slot[i] = q;
            } else {
                free(slot[i]);
                slot[i] = nullptr;
            }
        } else {
            const size_t n = pickSize(r);
            slot[i] = malloc(n);
            if (slot[i]) memset(slot[i], static_cast<int>(n & 0x7f) + 1, n < 4096 ? n : 4096);
        }
        if (xorshift(r) % 8 == 0) {
            const size_t off = xorshift(r) % kColdBytes;
            if (g_cold[off] != patByte(off)) g_fail.fetch_add(1);
        }
        if (xorshift(r) % 2000 == 0) usleep(static_cast<useconds_t>(xorshift(r) % 20000));
    }
    for (void* p : slot) free(p);
    return nullptr;
}

int childBody(uint64_t seed) {
    uint64_t r = seed | 1;
    void* p[512];
    for (int round = 0; round < 4; ++round) {
        for (int i = 0; i < 512; ++i) {
            const size_t n = pickSize(r);
            p[i] = malloc(n);
            if (!p[i]) return 3;
            memset(p[i], 0x5a, n < 64 ? n : 64);
        }
        for (int i = 0; i < 512; ++i) free(p[i]);
    }
    for (size_t off = 0; off < kColdBytes; off += 1024)
        if (g_cold[off] != patByte(off)) return 4;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const int forks = argc > 1 ? atoi(argv[1]) : 120;
    const int nthreads = argc > 2 ? atoi(argv[2]) : 6;
    const char* lo = getenv("SMASH_LARGE_ONLY");
    fprintf(stderr, "fork_stress_test: %d forks, %d threads, mode=%s\n", forks, nthreads,
            lo && lo[0] == '1' ? "large-only" : "full");
    g_cold = static_cast<unsigned char*>(malloc(kColdBytes));
    for (size_t j = 0; j < kColdBytes; ++j) g_cold[j] = patByte(j);
    sleep(3);  // let the compressor reclaim the cold set first

    pthread_t th[32];
    const int n = nthreads > 32 ? 32 : nthreads;
    const uint64_t seed = static_cast<uint64_t>(time(nullptr)) ^ static_cast<uint64_t>(getpid());
    for (int i = 0; i < n; ++i)
        pthread_create(&th[i], nullptr, worker, reinterpret_cast<void*>(seed + 7919ull * i));

    int hung = 0, bad = 0;
    uint64_t r = seed ^ 0x1234567ull;
    for (int f = 0; f < forks && !g_fail.load(); ++f) {
        usleep(static_cast<useconds_t>(xorshift(r) % 60000));
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); ++bad; break; }
        if (pid == 0) _exit(childBody(seed + static_cast<uint64_t>(f)));
        int st = 0;
        bool done = false;
        for (int ms = 0; ms < 20000; ms += 5) {
            pid_t w = waitpid(pid, &st, WNOHANG);
            if (w == pid) { done = true; break; }
            usleep(5000);
        }
        if (!done) {
            fprintf(stderr, "FAIL: fork %d: child %d hung (inherited a held lock?)\n", f, pid);
            if (const char* g = getenv("FORK_STRESS_GDB"); g && g[0] == '1') {
                char cmd[160];  // debugging aid: where is the child stuck?
                snprintf(cmd, sizeof(cmd), "sudo -n gdb -p %d -batch -ex 'thread apply all bt 30' 1>&2", pid);
                if (system(cmd) != 0) fprintf(stderr, "(gdb failed)\n");
            }
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            ++hung;
            continue;
        }
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            fprintf(stderr, "FAIL: fork %d: child status 0x%x\n", f, st);
            ++bad;
        }
    }
    g_stop.store(true);
    for (int i = 0; i < n; ++i) pthread_join(th[i], nullptr);
    for (size_t j = 0; j < kColdBytes; j += 4096)
        if (g_cold[j] != patByte(j)) { fprintf(stderr, "FAIL: parent cold set\n"); ++bad; break; }
    if (g_fail.load()) fprintf(stderr, "FAIL: %d worker mismatches\n", g_fail.load());
    fprintf(stderr, "fork_stress_test: hung=%d bad=%d -> %s\n", hung, bad,
            hung || bad || g_fail.load() ? "FAIL" : "PASS");
    return hung || bad || g_fail.load() ? 1 : 0;
}
