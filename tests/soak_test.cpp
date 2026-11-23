// Multi-threaded alloc / free / fault soak under LD_PRELOAD / DYLD_INSERT_LIBRARIES,
// with randomized timing and (optionally) the kill switch toggled underneath.
//
//   soak_test [seconds] [threads]      (defaults: $SMASH_SOAK_SEC or 15, 8)
//   SMASH_SOAK_TOGGLE=1                a thread flips smash_disable()/smash_enable()
//                                      (enabled 3.5-7 s, disabled 0.3-1.5 s)
//   SMASH_SOAK_SEED=N                  reproduce a run (printed at start)
//
// Every live object carries a pattern derived from (seed, size); it is checked
// on free / realloc and at the end. A shared cold set (long-lived, compressible,
// so the compressor reclaims it) is read at random by every worker, driving
// concurrent decompress faults against compression, frees and kill-switch drains.
// Any mismatch, a decompression failure, or (toggle mode) a page still
// compressed after smash_disable() returned 0 fails the run.
//
// External memory (exercised when run with SMASH_TRACK_EXTERNAL=1): a second
// cold set mapped directly with mmap, read the same way, and per-worker
// mmap'd chunks managed like a bundled allocator's extents (map + fill,
// check, madvise(MADV_DONTNEED) then expect zeros, refill, munmap).
#include <dlfcn.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

struct Status {  // mirrors SmashSafetyStatus (include/smash/smash.h)
    int disabled, inert;
    unsigned long long compressed_pages, reclaimed_pages, faults, decompress_faults,
        decompress_failures, watchdog_trips;
    unsigned cold_floor_ticks, slo_multiplier_x256;
    unsigned long long fault_rate, fault_p99_us;
    char reason[128];
};
using status_fn = int (*)(Status*);
using toggle_fn = int (*)();
status_fn g_status;
toggle_fn g_disable, g_enable;

std::atomic<bool> g_stop{false};
std::atomic<int> g_fail{0};
std::atomic<uint64_t> g_ops{0}, g_toggles{0}, g_drained_from{0};

constexpr size_t kColdBytes = 32u << 20;
unsigned char* g_cold;
constexpr size_t kExtColdBytes = 16u << 20;
unsigned char* g_ext_cold;  // mmap'd directly: external memory

uint64_t xorshift(uint64_t& s) {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return s;
}
unsigned char patByte(uint64_t seed, size_t off) {
    return static_cast<unsigned char>(1 + ((seed + off / 96) % 239));
}
void fillObj(unsigned char* p, size_t n, uint64_t seed) {
    for (size_t j = 0; j < n; ++j) p[j] = patByte(seed, j);
}
bool checkObj(const unsigned char* p, size_t n, uint64_t seed) {
    // Sparse check for big objects (every 509th byte + both ends).
    const size_t step = n > 65536 ? 509 : 1;
    for (size_t j = 0; j < n; j += step)
        if (p[j] != patByte(seed, j)) return false;
    return n == 0 || p[n - 1] == patByte(seed, n - 1);
}

struct Slot { unsigned char* p; size_t n; uint64_t seed; };

// A worker's mmap'd extent: state 0 = unmapped, 1 = filled, 2 = purged (zero).
struct Extent { unsigned char* p; uint64_t seed; int state; };
constexpr size_t kExtentBytes = 256u << 10;
void extentOp(Extent& e, uint64_t& r) {
    if (e.state == 0) {
        void* m = mmap(nullptr, kExtentBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) { g_fail.fetch_add(1); return; }
        e.p = static_cast<unsigned char*>(m);
        e.seed = xorshift(r);
        fillObj(e.p, kExtentBytes, e.seed);
        e.state = 1;
        return;
    }
    if (e.state == 2) {
        // Linux: DONTNEED'd anonymous memory reads zero. macOS keeps the old
        // bytes or zeros, page by page.
        for (size_t j = 0; j < kExtentBytes; j += 509) {
#ifdef __linux__
            const bool ok = e.p[j] == 0;
#else
            const bool ok = e.p[j] == 0 || e.p[j] == patByte(e.seed, j);
#endif
            if (!ok) { g_fail.fetch_add(1); fprintf(stderr, "FAIL: purged extent byte %zu is neither zero nor old data\n", j); break; }
        }
        fillObj(e.p, kExtentBytes, e.seed);
        e.state = 1;
        return;
    }
    if (!checkObj(e.p, kExtentBytes, e.seed)) {
        g_fail.fetch_add(1);
        fprintf(stderr, "FAIL: extent mismatch\n");
    }
    if (xorshift(r) % 2) {
        madvise(e.p, kExtentBytes, MADV_DONTNEED);
        e.state = 2;
    } else {
        munmap(e.p, kExtentBytes);
        e.state = 0;
    }
}

size_t pickSize(uint64_t& r) {
    const uint64_t k = xorshift(r) % 100;
    if (k < 70) return 16 + xorshift(r) % 1009;
    if (k < 92) return 1024 + xorshift(r) % (64 * 1024);
    return (1u << 20) + xorshift(r) % (2u << 20);  // >= 1 MiB: compressible in both modes
}

void* worker(void* arg) {
    uint64_t r = reinterpret_cast<uint64_t>(arg) | 1;
    constexpr int kSlots = 384;
    Slot* slots = static_cast<Slot*>(calloc(kSlots, sizeof(Slot)));
    constexpr int kExtents = 16;
    Extent ext[kExtents] = {};
    while (!g_stop.load(std::memory_order_relaxed)) {
        const int i = static_cast<int>(xorshift(r) % kSlots);
        Slot& s = slots[i];
        const uint64_t op = xorshift(r) % 100;
        if (s.p && op < 10) {  // realloc keeps the prefix
            if (!checkObj(s.p, s.n, s.seed)) { g_fail.fetch_add(1); fprintf(stderr, "FAIL: pre-realloc mismatch\n"); }
            const size_t nn = pickSize(r);
            auto* q = static_cast<unsigned char*>(realloc(s.p, nn));
            if (!q) { g_fail.fetch_add(1); continue; }
            const size_t keep = nn < s.n ? nn : s.n;
            if (!checkObj(q, keep, s.seed)) { g_fail.fetch_add(1); fprintf(stderr, "FAIL: realloc lost data\n"); }
            s.p = q; s.n = nn;
            fillObj(s.p, s.n, s.seed);
        } else if (s.p) {
            if (!checkObj(s.p, s.n, s.seed)) {
                g_fail.fetch_add(1);
                fprintf(stderr, "FAIL: object mismatch (size %zu)\n", s.n);
            }
            free(s.p);
            s.p = nullptr;
        } else {
            s.n = pickSize(r);
            s.seed = xorshift(r);
            s.p = static_cast<unsigned char*>(malloc(s.n));
            if (!s.p) { g_fail.fetch_add(1); continue; }
            fillObj(s.p, s.n, s.seed);
        }
        // Read a random page of the cold set: a decompress fault whenever the
        // compressor reclaimed it.
        if (op % 4 == 0) {
            const size_t off = (xorshift(r) % (kColdBytes / 4096)) * 4096 + xorshift(r) % 4096;
            if (g_cold[off] != patByte(7, off)) {
                g_fail.fetch_add(1);
                fprintf(stderr, "FAIL: cold set byte %zu\n", off);
            }
            const size_t eo = (xorshift(r) % (kExtColdBytes / 4096)) * 4096 + xorshift(r) % 4096;
            if (g_ext_cold[eo] != patByte(9, eo)) {
                g_fail.fetch_add(1);
                fprintf(stderr, "FAIL: external cold set byte %zu\n", eo);
            }
        }
        if (op % 16 == 1) extentOp(ext[xorshift(r) % kExtents], r);
        g_ops.fetch_add(1, std::memory_order_relaxed);
        // Randomized timing: mostly busy, sometimes idle long enough for the
        // compressor to reclaim this thread's objects.
        const uint64_t t = xorshift(r) % 1000;
        if (t < 3) usleep(static_cast<useconds_t>(200000 + xorshift(r) % 1300000));
        else if (t < 40) usleep(static_cast<useconds_t>(xorshift(r) % 5000));
    }
    for (int i = 0; i < kSlots; ++i)
        if (slots[i].p) {
            if (!checkObj(slots[i].p, slots[i].n, slots[i].seed)) {
                g_fail.fetch_add(1);
                fprintf(stderr, "FAIL: final object mismatch\n");
            }
            free(slots[i].p);
        }
    free(slots);
    for (Extent& e : ext) {
        if (e.state == 1 && !checkObj(e.p, kExtentBytes, e.seed)) {
            g_fail.fetch_add(1);
            fprintf(stderr, "FAIL: final extent mismatch\n");
        }
        if (e.state) munmap(e.p, kExtentBytes);
    }
    return nullptr;
}

void* toggler(void* arg) {
    uint64_t r = reinterpret_cast<uint64_t>(arg) | 1;
    while (!g_stop.load(std::memory_order_relaxed)) {
        // Enabled long enough (3.5-7 s at SMASH_COLD_TIMEOUT_SEC=1) for pages
        // to be compressed AND reclaimed, so each disable drains real
        // compressed pages while the workers fault on them; re-enable resets
        // every cold streak, so shorter windows would never compress.
        usleep(static_cast<useconds_t>(3500000 + xorshift(r) % 3500000));
        if (g_stop.load()) break;
        Status pre{};
        g_status(&pre);
        g_drained_from.fetch_add(pre.compressed_pages, std::memory_order_relaxed);
        const int rc = g_disable();
        if (rc == 0) {
            Status s{};
            g_status(&s);
            if (s.compressed_pages != 0 || !s.inert) {
                fprintf(stderr, "FAIL: smash_disable()==0 but compressed=%llu inert=%d\n",
                        s.compressed_pages, s.inert);
                g_fail.fetch_add(1);
            }
        } else {
            fprintf(stderr, "FAIL: smash_disable() = %d\n", rc);
            g_fail.fetch_add(1);
        }
        usleep(static_cast<useconds_t>(300000 + xorshift(r) % 1200000));
        if (g_enable() != 0) { fprintf(stderr, "FAIL: smash_enable()\n"); g_fail.fetch_add(1); }
        g_toggles.fetch_add(1);
    }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    const char* es = getenv("SMASH_SOAK_SEC");
    const int secs = argc > 1 ? atoi(argv[1]) : es ? atoi(es) : 15;
    const int nthreads = argc > 2 ? atoi(argv[2]) : 8;
    const char* tg = getenv("SMASH_SOAK_TOGGLE");
    const bool toggle = tg && tg[0] == '1';
    const char* sd = getenv("SMASH_SOAK_SEED");
    const uint64_t seed = sd ? strtoull(sd, nullptr, 10)
                             : static_cast<uint64_t>(time(nullptr)) ^ (static_cast<uint64_t>(getpid()) << 20);
    const char* lo = getenv("SMASH_LARGE_ONLY");
    fprintf(stderr, "soak_test: %d s, %d threads, toggle=%d, mode=%s, SMASH_SOAK_SEED=%llu\n", secs,
            nthreads, toggle ? 1 : 0, lo && lo[0] == '1' ? "large-only" : "full",
            (unsigned long long)seed);

    g_status = reinterpret_cast<status_fn>(dlsym(RTLD_DEFAULT, "smash_safety_status"));
    g_disable = reinterpret_cast<toggle_fn>(dlsym(RTLD_DEFAULT, "smash_disable"));
    g_enable = reinterpret_cast<toggle_fn>(dlsym(RTLD_DEFAULT, "smash_enable"));
    if (!g_status || !g_disable || !g_enable) {
        fprintf(stderr, "FAIL: smash safety API not exported (not running under libsmash?)\n");
        return 1;
    }

    g_cold = static_cast<unsigned char*>(malloc(kColdBytes));
    fillObj(g_cold, kColdBytes, 7);
    g_ext_cold = static_cast<unsigned char*>(mmap(nullptr, kExtColdBytes, PROT_READ | PROT_WRITE,
                                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (g_ext_cold == MAP_FAILED) { fprintf(stderr, "FAIL: mmap\n"); return 1; }
    fillObj(g_ext_cold, kExtColdBytes, 9);

    pthread_t th[64];
    const int n = nthreads > 64 ? 64 : nthreads;
    for (int i = 0; i < n; ++i)
        pthread_create(&th[i], nullptr, worker,
                       reinterpret_cast<void*>(seed * 0x9E3779B97F4A7C15ull + static_cast<uint64_t>(i)));
    pthread_t tt{};
    if (toggle) pthread_create(&tt, nullptr, toggler, reinterpret_cast<void*>(seed ^ 0xabcdefull));

    Status s{};
    for (int t = 0; t < secs; ++t) {
        sleep(1);
        if ((t + 1) % 5 == 0) {
            g_status(&s);
            fprintf(stderr, "  t=%ds ops=%llu compressed=%llu reclaimed=%llu faults=%llu toggles=%llu\n",
                    t + 1, (unsigned long long)g_ops.load(), s.compressed_pages, s.reclaimed_pages,
                    s.faults, (unsigned long long)g_toggles.load());
        }
        if (g_fail.load()) break;
    }
    g_stop.store(true);
    for (int i = 0; i < n; ++i) pthread_join(th[i], nullptr);
    if (toggle) pthread_join(tt, nullptr);
    if (!checkObj(g_cold, kColdBytes, 7)) { fprintf(stderr, "FAIL: cold set\n"); g_fail.fetch_add(1); }
    if (!checkObj(g_ext_cold, kExtColdBytes, 9)) { fprintf(stderr, "FAIL: external cold set\n"); g_fail.fetch_add(1); }
    g_status(&s);
    if (s.decompress_failures) {
        fprintf(stderr, "FAIL: %llu decompression failures\n", s.decompress_failures);
        g_fail.fetch_add(1);
    }
    fprintf(stderr,
            "soak_test: ops=%llu faults=%llu decompress_faults=%llu toggles=%llu "
            "compressed_at_disable=%llu -> %s\n",
            (unsigned long long)g_ops.load(), s.faults, s.decompress_faults,
            (unsigned long long)g_toggles.load(), (unsigned long long)g_drained_from.load(),
            g_fail.load() ? "FAIL" : "PASS");
    return g_fail.load() ? 1 : 0;
}
