// bench_arena_segregation.cpp - Measure page-level compression quality
//
// Demonstrates how allocation layout affects compression: when objects of
// different compressibility share pages, per-page ratios degrade because
// a single incompressible object "poisons" an otherwise compressible page.
//
// The benchmark allocates objects from a SINGLE thread with multiple call
// sites interleaved (A,B,C,A,B,C...) — the worst case for any allocator's
// segregation. It then measures per-page compression ratios to show how
// well the allocator segregated by content type.
//
// Data types (all same size class to eliminate size-class-based segregation):
//   Site A: JSON-like strings (highly compressible — repeated keys, ASCII)
//   Site B: Float arrays (moderately compressible — IEEE754 patterns)
//   Site C: Random bytes (incompressible — simulates encrypted/hashed data)
//   Site D: Integer sequences (compressible — deltas are small)
//
// Hotness: after allocation, only sites A and D are "hot" (accessed every
// second). Sites B and C go cold. This tests whether the compressor can
// selectively compress only the cold pages without disturbing hot ones.
//
// Run:
//   ./bench_arena_segregation                   # system malloc baseline
//   LD_PRELOAD=libsmash.so SMASH_COLD_TIMEOUT_SEC=2 ./bench_arena_segregation

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <mutex>

#include <lz4.h>
#include <zstd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <unistd.h>
#elif defined(__linux__)
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#endif

static size_t getPageSize() {
    static size_t ps = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    return ps;
}

static size_t getCurrentRSSBytes() {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return info.resident_size;
    return 0;
#elif defined(__linux__)
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0) return 0;
    char buf[4096];
    ssize_t n = syscall(SYS_read, fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    const char* p = strstr(buf, "VmRSS:");
    if (!p) return 0;
    size_t kb = 0;
    sscanf(p, "VmRSS: %zu kB", &kb);
    return kb * 1024;
#else
    return 0;
#endif
}

// All objects are 256 bytes (same size class)
static constexpr size_t kObjSize = 256;

// ── Allocation sites (noinline for distinct return addresses) ────────────────

// To get distinct return addresses visible to smash's __builtin_return_address,
// each wrapper must have a real stack frame (not a tail-call). Writing to a
// volatile AFTER malloc returns forces the compiler to use call+ret (not jmp).
static volatile void* _site_a, *_site_b, *_site_c, *_site_d;
__attribute__((noinline)) static void* alloc_json()   { void* p = malloc(kObjSize); _site_a = p; return p; }
__attribute__((noinline)) static void* alloc_floats() { void* p = malloc(kObjSize); _site_b = p; return p; }
__attribute__((noinline)) static void* alloc_random() { void* p = malloc(kObjSize); _site_c = p; return p; }
__attribute__((noinline)) static void* alloc_ints()   { void* p = malloc(kObjSize); _site_d = p; return p; }

// ── Fill patterns (realistic data types) ─────────────────────────────────────

static void fill_json(void* p, uint64_t seq) {
    // JSON-like: repeated key names + short varying values
    char* dst = static_cast<char*>(p);
    int n = snprintf(dst, kObjSize,
        "{\"id\":%lu,\"name\":\"user_%lu\",\"email\":\"u%lu@example.com\","
        "\"active\":true,\"score\":0,\"level\":1,\"role\":\"member\","
        "\"created\":\"2024-01-01\",\"tags\":[\"a\",\"b\"]}",
        (unsigned long)seq, (unsigned long)(seq % 10000),
        (unsigned long)seq);
    if (n < (int)kObjSize) memset(dst + n, ' ', kObjSize - n);
}

static void fill_floats(void* p, uint64_t seq) {
    // IEEE754 float array — moderately compressible (sign+exponent bits repeat)
    float* dst = static_cast<float*>(p);
    for (size_t i = 0; i < kObjSize / sizeof(float); ++i) {
        dst[i] = sinf(static_cast<float>(seq + i) * 0.01f) * 100.0f;
    }
}

static uint64_t splitmix64(uint64_t* state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void fill_random(void* p, uint64_t* seed) {
    // Cryptographic-quality random (incompressible)
    uint64_t* dst = static_cast<uint64_t*>(p);
    for (size_t i = 0; i < kObjSize / sizeof(uint64_t); ++i)
        dst[i] = splitmix64(seed);
}

static void fill_ints(void* p, uint64_t seq) {
    // Monotonic integers with small deltas (highly compressible via delta coding)
    uint32_t* dst = static_cast<uint32_t*>(p);
    uint32_t base = static_cast<uint32_t>(seq * 100);
    for (size_t i = 0; i < kObjSize / sizeof(uint32_t); ++i)
        dst[i] = base + static_cast<uint32_t>(i);
}

// ── Page compression measurement ─────────────────────────────────────────────

enum Site { JSON = 0, FLOATS = 1, RANDOM = 2, INTS = 3, NUM_SITES = 4 };
static const char* site_names[] = {"json", "floats", "random", "ints"};

static double compressPageLZ4(const void* page_addr, size_t page_size) {
    int bound = LZ4_compressBound(static_cast<int>(page_size));
    std::vector<char> dst(bound);
    int comp = LZ4_compress_default(
        static_cast<const char*>(page_addr), dst.data(),
        static_cast<int>(page_size), bound);
    if (comp <= 0) return 1.0;
    return static_cast<double>(comp) / static_cast<double>(page_size);
}

int main() {
    const size_t page_size = getPageSize();
    printf("=== Arena Segregation Benchmark ===\n");
    printf("Object size: %zu, page size: %zu, objects/page: %zu\n",
           kObjSize, page_size, page_size / kObjSize);

    constexpr size_t kObjectsPerSite = 32768;  // ~8 MB per site, ~32 MB total
    constexpr size_t kTotalObjects = kObjectsPerSite * NUM_SITES;
    constexpr int kCoolSec = 12;

    struct Alloc { void* ptr; Site site; };
    std::vector<Alloc> allocs;
    allocs.reserve(kTotalObjects);

    uint64_t seed_r = 42;

    printf("\nAllocating %zu objects (%zu per site, strictly interleaved)...\n",
           kTotalObjects, kObjectsPerSite);

    // Strictly interleaved: JSON, FLOATS, RANDOM, INTS, JSON, FLOATS, ...
    // This is the WORST CASE for page homogeneity — forces all 4 types onto
    // shared pages under any allocator that doesn't segregate by call site.
    for (size_t i = 0; i < kObjectsPerSite; ++i) {
        void* pj = alloc_json();
        fill_json(pj, i);
        allocs.push_back({pj, JSON});

        void* pf = alloc_floats();
        fill_floats(pf, i);
        allocs.push_back({pf, FLOATS});

        void* pr = alloc_random();
        fill_random(pr, &seed_r);
        allocs.push_back({pr, RANDOM});

        void* pi = alloc_ints();
        fill_ints(pi, i);
        allocs.push_back({pi, INTS});
    }

    size_t peak_rss = getCurrentRSSBytes();
    double alloc_mb = static_cast<double>(kTotalObjects * kObjSize) / (1024 * 1024);
    printf("Total data: %.1f MB, Peak RSS: %.1f MB\n",
           alloc_mb, peak_rss / (1024.0 * 1024.0));

    // ── Hotness simulation ───────────────────────────────────────────────────
    // Touch JSON and INTS objects periodically (hot), leave FLOATS and RANDOM cold.
    printf("\nSimulating hotness (JSON+INTS hot, FLOATS+RANDOM cold) for %ds...\n", kCoolSec);

    for (int t = 1; t <= kCoolSec; ++t) {
        // Touch hot objects (read a byte to mark as accessed)
        for (auto& a : allocs) {
            if (a.site == JSON || a.site == INTS) {
                volatile char c = static_cast<volatile char*>(a.ptr)[0];
                (void)c;
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (t % 4 == 0) {
            size_t rss = getCurrentRSSBytes();
            printf("  t=%ds: RSS=%.1f MB\n", t, rss / (1024.0 * 1024.0));
        }
    }

    size_t cool_rss = getCurrentRSSBytes();
    double reduction = (1.0 - static_cast<double>(cool_rss) / peak_rss) * 100;

    // ── Page analysis ────────────────────────────────────────────────────────

    struct PageInfo { size_t counts[NUM_SITES] = {}; uintptr_t addr = 0; };
    std::unordered_map<uintptr_t, PageInfo> pages;
    for (auto& a : allocs) {
        uintptr_t pg = reinterpret_cast<uintptr_t>(a.ptr) & ~(page_size - 1);
        pages[pg].counts[a.site]++;
        pages[pg].addr = pg;
    }

    size_t total_pages = pages.size();
    size_t homogeneous = 0;    // >=75% single site
    size_t fully_mixed = 0;    // all 4 sites present, none >40%
    double sum_ratio = 0;
    double sum_ratio_per_site[NUM_SITES] = {};
    size_t count_per_site[NUM_SITES] = {};
    size_t pages_compressible = 0;

    for (auto& [addr, pi] : pages) {
        size_t total = 0;
        size_t max_count = 0;
        int dominant_site = -1;
        for (int s = 0; s < NUM_SITES; ++s) {
            total += pi.counts[s];
            if (pi.counts[s] > max_count) {
                max_count = pi.counts[s];
                dominant_site = s;
            }
        }
        if (total == 0) continue;

        bool is_homo = (max_count * 100 / total) >= 75;
        if (is_homo) homogeneous++;

        int sites_present = 0;
        for (int s = 0; s < NUM_SITES; ++s)
            if (pi.counts[s] > 0) sites_present++;
        if (sites_present == 4 && max_count * 100 / total < 40)
            fully_mixed++;

        double ratio = compressPageLZ4(reinterpret_cast<const void*>(addr), page_size);
        sum_ratio += ratio;
        if (ratio < 0.75) pages_compressible++;

        if (is_homo && dominant_site >= 0) {
            sum_ratio_per_site[dominant_site] += ratio;
            count_per_site[dominant_site]++;
        }
    }

    // ── Output ───────────────────────────────────────────────────────────────

    printf("\n--- Page Analysis ---\n");
    printf("Total pages: %zu\n", total_pages);
    printf("Homogeneous (>=75%% single site): %zu (%.0f%%)\n",
           homogeneous, 100.0 * homogeneous / total_pages);
    printf("Fully mixed (4 sites, none >40%%): %zu (%.0f%%)\n",
           fully_mixed, 100.0 * fully_mixed / total_pages);
    printf("Avg LZ4 ratio: %.3f\n", sum_ratio / total_pages);
    printf("Pages compressible (ratio<0.75): %zu (%.0f%%)\n",
           pages_compressible, 100.0 * pages_compressible / total_pages);
    for (int s = 0; s < NUM_SITES; ++s) {
        if (count_per_site[s] > 0)
            printf("  %s-dominated pages: %zu  avg ratio: %.3f\n",
                   site_names[s], count_per_site[s],
                   sum_ratio_per_site[s] / count_per_site[s]);
    }

    printf("\n--- RSS ---\n");
    printf("Peak: %.1f MB, Post-cool: %.1f MB (%.1f%% reduction)\n",
           peak_rss / (1024.0 * 1024.0), cool_rss / (1024.0 * 1024.0), reduction);

    // Machine-readable
    printf("\nMETRIC total_data_mb %.1f\n", alloc_mb);
    printf("METRIC peak_rss_mb %.1f\n", peak_rss / (1024.0 * 1024.0));
    printf("METRIC post_cool_rss_mb %.1f\n", cool_rss / (1024.0 * 1024.0));
    printf("METRIC rss_reduction_pct %.1f\n", reduction);
    printf("METRIC homogeneity_pct %.1f\n", 100.0 * homogeneous / total_pages);
    printf("METRIC fully_mixed_pct %.1f\n", 100.0 * fully_mixed / total_pages);
    printf("METRIC avg_ratio %.3f\n", sum_ratio / total_pages);
    printf("METRIC pages_compressible_pct %.0f\n", 100.0 * pages_compressible / total_pages);
    for (int s = 0; s < NUM_SITES; ++s)
        if (count_per_site[s] > 0)
            printf("METRIC ratio_%s %.3f\n", site_names[s],
                   sum_ratio_per_site[s] / count_per_site[s]);

    for (auto& a : allocs) free(a.ptr);
    return 0;
}
