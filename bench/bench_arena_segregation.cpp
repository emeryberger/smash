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

struct CompressResult { double lz4, zstd1, zstd9; };

static CompressResult compressPage(const void* page_addr, size_t page_size) {
    CompressResult r{1.0, 1.0, 1.0};
    // LZ4
    {
        int bound = LZ4_compressBound(static_cast<int>(page_size));
        std::vector<char> dst(bound);
        int comp = LZ4_compress_default(
            static_cast<const char*>(page_addr), dst.data(),
            static_cast<int>(page_size), bound);
        if (comp > 0) r.lz4 = static_cast<double>(comp) / page_size;
    }
    // Zstd level 1
    {
        size_t bound = ZSTD_compressBound(page_size);
        std::vector<char> dst(bound);
        size_t comp = ZSTD_compress(dst.data(), bound, page_addr, page_size, 1);
        if (!ZSTD_isError(comp)) r.zstd1 = static_cast<double>(comp) / page_size;
    }
    // Zstd level 9
    {
        size_t bound = ZSTD_compressBound(page_size);
        std::vector<char> dst(bound);
        size_t comp = ZSTD_compress(dst.data(), bound, page_addr, page_size, 9);
        if (!ZSTD_isError(comp)) r.zstd9 = static_cast<double>(comp) / page_size;
    }
    return r;
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

    // ── Pure cooling phase: let ALL pages go cold first ─────────────────────
    printf("\nCooling 8s (all objects untouched)...\n");
    for (int t = 1; t <= 8; ++t) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (t % 4 == 0) {
            size_t rss = getCurrentRSSBytes();
            printf("  cool t=%ds: RSS=%.1f MB\n", t, rss / (1024.0 * 1024.0));
        }
    }
    size_t post_cool_rss = getCurrentRSSBytes();
    printf("Post-cool RSS: %.1f MB (%.1f%% reduction from peak)\n",
           post_cool_rss / (1024.0 * 1024.0),
           (1.0 - static_cast<double>(post_cool_rss) / peak_rss) * 100);

    // ── Serve phase: re-access only HOT objects ──────────────────────────────
    // After cooling, cold pages (FLOATS+RANDOM) are compressed.
    // Hot pages (JSON+INTS) decompress on first touch, then stay warm.
    printf("\nServe phase: accessing JSON+INTS (hot), FLOATS+RANDOM stay compressed...\n");

    // Build hot working set indices for fast iteration
    std::vector<void*> hot_ptrs;
    hot_ptrs.reserve(kObjectsPerSite * 2);
    for (auto& a : allocs)
        if (a.site == JSON || a.site == INTS) hot_ptrs.push_back(a.ptr);

    // Serve loop: access hot objects with realistic read/write pattern
    auto t_serve_start = std::chrono::steady_clock::now();
    size_t total_ops = 0;
    for (int t = 1; t <= kCoolSec; ++t) {
        // Each second: scan through hot objects, reading and occasionally writing
        for (size_t i = 0; i < hot_ptrs.size(); ++i) {
            // Read the object (simulates JSON parsing / counter lookup)
            volatile uint64_t sum = 0;
            auto* p = static_cast<volatile uint64_t*>(hot_ptrs[i]);
            for (size_t w = 0; w < kObjSize / 8; w += 4)
                sum += p[w];
            (void)sum;
            total_ops++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (t % 4 == 0) {
            size_t rss = getCurrentRSSBytes();
            printf("  t=%ds: RSS=%.1f MB  ops=%zu\n", t, rss / (1024.0 * 1024.0), total_ops);
        }
    }
    auto t_serve_end = std::chrono::steady_clock::now();
    double serve_sec = std::chrono::duration<double>(t_serve_end - t_serve_start).count();
    double ops_per_sec = static_cast<double>(total_ops) / serve_sec;

    size_t cool_rss = getCurrentRSSBytes();
    double reduction = (1.0 - static_cast<double>(cool_rss) / peak_rss) * 100;

    // Cold re-access: touch a sample of cold objects (simulates rare model reload)
    printf("\nCold re-access: reading 1000 FLOATS objects...\n");
    auto t_cold_start = std::chrono::steady_clock::now();
    volatile uint64_t cold_sum = 0;
    size_t cold_accessed = 0;
    for (auto& a : allocs) {
        if (a.site == FLOATS && cold_accessed < 1000) {
            auto* p = static_cast<uint64_t*>(a.ptr);
            for (size_t w = 0; w < kObjSize / 8; ++w)
                cold_sum += p[w];
            cold_accessed++;
        }
    }
    (void)cold_sum;
    auto t_cold_end = std::chrono::steady_clock::now();
    double cold_us = std::chrono::duration<double>(t_cold_end - t_cold_start).count() * 1e6;
    double cold_per_obj_us = cold_us / cold_accessed;
    printf("  Cold access: %zu objects in %.0f us (%.1f us/obj)\n",
           cold_accessed, cold_us, cold_per_obj_us);

    // ── Page analysis ────────────────────────────────────────────────────────

    struct PageInfo { size_t counts[NUM_SITES] = {}; uintptr_t addr = 0; bool is_hot = false; };
    std::unordered_map<uintptr_t, PageInfo> pages;
    for (auto& a : allocs) {
        uintptr_t pg = reinterpret_cast<uintptr_t>(a.ptr) & ~(page_size - 1);
        auto& pi = pages[pg];
        pi.counts[a.site]++;
        pi.addr = pg;
        if (a.site == JSON || a.site == INTS) pi.is_hot = true;
    }

    size_t total_pages = pages.size();
    size_t homogeneous = 0;
    size_t fully_mixed = 0;

    // Per-algorithm accumulators
    struct AlgoStats {
        double sum_all = 0, sum_hot = 0, sum_cold = 0;
        double sum_per_site[NUM_SITES] = {};
        size_t count_hot = 0, count_cold = 0;
    };
    AlgoStats lz4_stats, zstd1_stats, zstd9_stats;
    size_t count_per_site[NUM_SITES] = {};
    size_t pages_compressible_lz4 = 0;
    size_t hot_pages = 0, cold_pages = 0;

    for (auto& [addr, pi] : pages) {
        size_t total = 0, max_count = 0;
        int dominant_site = -1;
        for (int s = 0; s < NUM_SITES; ++s) {
            total += pi.counts[s];
            if (pi.counts[s] > max_count) { max_count = pi.counts[s]; dominant_site = s; }
        }
        if (total == 0) continue;

        bool is_homo = (max_count * 100 / total) >= 75;
        if (is_homo) homogeneous++;

        int sites_present = 0;
        for (int s = 0; s < NUM_SITES; ++s) if (pi.counts[s] > 0) sites_present++;
        if (sites_present == 4 && max_count * 100 / total < 40) fully_mixed++;

        auto cr = compressPage(reinterpret_cast<const void*>(addr), page_size);
        lz4_stats.sum_all += cr.lz4;
        zstd1_stats.sum_all += cr.zstd1;
        zstd9_stats.sum_all += cr.zstd9;
        if (cr.lz4 < 0.75) pages_compressible_lz4++;

        if (pi.is_hot) {
            lz4_stats.sum_hot += cr.lz4; zstd1_stats.sum_hot += cr.zstd1; zstd9_stats.sum_hot += cr.zstd9;
            lz4_stats.count_hot++; zstd1_stats.count_hot++; zstd9_stats.count_hot++;
            hot_pages++;
        } else {
            lz4_stats.sum_cold += cr.lz4; zstd1_stats.sum_cold += cr.zstd1; zstd9_stats.sum_cold += cr.zstd9;
            lz4_stats.count_cold++; zstd1_stats.count_cold++; zstd9_stats.count_cold++;
            cold_pages++;
        }

        if (is_homo && dominant_site >= 0) {
            lz4_stats.sum_per_site[dominant_site] += cr.lz4;
            zstd1_stats.sum_per_site[dominant_site] += cr.zstd1;
            zstd9_stats.sum_per_site[dominant_site] += cr.zstd9;
            count_per_site[dominant_site]++;
        }
    }

    // ── Output ───────────────────────────────────────────────────────────────

    printf("\n--- Segregation ---\n");
    printf("Total pages: %zu (hot: %zu, cold: %zu)\n", total_pages, hot_pages, cold_pages);
    printf("Homogeneous (>=75%% single site): %zu (%.0f%%)\n",
           homogeneous, 100.0 * homogeneous / total_pages);
    printf("Fully mixed (4 sites, none >40%%): %zu (%.0f%%)\n",
           fully_mixed, 100.0 * fully_mixed / total_pages);

    printf("\n--- Compression Ratios (lower = better) ---\n");
    printf("%-10s %8s %8s %8s\n", "", "LZ4", "zstd-1", "zstd-9");
    printf("%-10s %8.3f %8.3f %8.3f\n", "All pages",
           lz4_stats.sum_all / total_pages, zstd1_stats.sum_all / total_pages, zstd9_stats.sum_all / total_pages);
    if (hot_pages > 0)
        printf("%-10s %8.3f %8.3f %8.3f\n", "Hot pages",
               lz4_stats.sum_hot / hot_pages, zstd1_stats.sum_hot / hot_pages, zstd9_stats.sum_hot / hot_pages);
    if (cold_pages > 0)
        printf("%-10s %8.3f %8.3f %8.3f\n", "Cold pages",
               lz4_stats.sum_cold / cold_pages, zstd1_stats.sum_cold / cold_pages, zstd9_stats.sum_cold / cold_pages);

    printf("\n--- Per-Site Ratios (homogeneous pages only) ---\n");
    printf("%-10s %8s %8s %8s  %s\n", "Site", "LZ4", "zstd-1", "zstd-9", "Hot?");
    for (int s = 0; s < NUM_SITES; ++s) {
        if (count_per_site[s] > 0) {
            bool hot = (s == JSON || s == INTS);
            printf("%-10s %8.3f %8.3f %8.3f  %s\n", site_names[s],
                   lz4_stats.sum_per_site[s] / count_per_site[s],
                   zstd1_stats.sum_per_site[s] / count_per_site[s],
                   zstd9_stats.sum_per_site[s] / count_per_site[s],
                   hot ? "HOT" : "cold");
        }
    }

    printf("\n--- Performance ---\n");
    printf("Serve: %.1f sec, %.0f ops/sec (hot object accesses)\n", serve_sec, ops_per_sec);
    printf("Cold re-access: %.1f us/object (decompression cost if compressed)\n", cold_per_obj_us);

    printf("\n--- RSS ---\n");
    printf("Peak: %.1f MB, Post-cool: %.1f MB (%.1f%% reduction)\n",
           peak_rss / (1024.0 * 1024.0), cool_rss / (1024.0 * 1024.0), reduction);

    printf("\nMETRIC total_data_mb %.1f\n", alloc_mb);
    printf("METRIC peak_rss_mb %.1f\n", peak_rss / (1024.0 * 1024.0));
    printf("METRIC post_cool_rss_mb %.1f\n", cool_rss / (1024.0 * 1024.0));
    printf("METRIC rss_reduction_pct %.1f\n", reduction);
    printf("METRIC serve_sec %.2f\n", serve_sec);
    printf("METRIC ops_per_sec %.0f\n", ops_per_sec);
    printf("METRIC cold_access_us_per_obj %.1f\n", cold_per_obj_us);
    printf("METRIC homogeneity_pct %.1f\n", 100.0 * homogeneous / total_pages);
    printf("METRIC fully_mixed_pct %.1f\n", 100.0 * fully_mixed / total_pages);
    printf("METRIC hot_pages %zu\n", hot_pages);
    printf("METRIC cold_pages %zu\n", cold_pages);
    printf("METRIC avg_lz4 %.3f\n", lz4_stats.sum_all / total_pages);
    printf("METRIC avg_zstd1 %.3f\n", zstd1_stats.sum_all / total_pages);
    printf("METRIC avg_zstd9 %.3f\n", zstd9_stats.sum_all / total_pages);
    printf("METRIC hot_lz4 %.3f\n", hot_pages ? lz4_stats.sum_hot / hot_pages : 0.0);
    printf("METRIC cold_lz4 %.3f\n", cold_pages ? lz4_stats.sum_cold / cold_pages : 0.0);
    printf("METRIC hot_zstd1 %.3f\n", hot_pages ? zstd1_stats.sum_hot / hot_pages : 0.0);
    printf("METRIC cold_zstd1 %.3f\n", cold_pages ? zstd1_stats.sum_cold / cold_pages : 0.0);
    printf("METRIC hot_zstd9 %.3f\n", hot_pages ? zstd9_stats.sum_hot / hot_pages : 0.0);
    printf("METRIC cold_zstd9 %.3f\n", cold_pages ? zstd9_stats.sum_cold / cold_pages : 0.0);
    for (int s = 0; s < NUM_SITES; ++s)
        if (count_per_site[s] > 0) {
            printf("METRIC %s_lz4 %.3f\n", site_names[s], lz4_stats.sum_per_site[s] / count_per_site[s]);
            printf("METRIC %s_zstd1 %.3f\n", site_names[s], zstd1_stats.sum_per_site[s] / count_per_site[s]);
            printf("METRIC %s_zstd9 %.3f\n", site_names[s], zstd9_stats.sum_per_site[s] / count_per_site[s]);
        }

    for (auto& a : allocs) free(a.ptr);
    return 0;
}
