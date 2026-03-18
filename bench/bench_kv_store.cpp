// bench_kv_store.cpp - Key-value store simulation for A/B comparison
//
// Uses standard malloc/free (via std::unordered_map). Run directly for system
// malloc baseline, or with DYLD_INSERT_LIBRARIES=libsmash.dylib for Smash.
//
// Workload phases:
//   1. Fill: 1M entries in unordered_map (~200 byte compressible values)
//   2. Cooling: complete idle so all pages go cold and compress
//   3. Serve: hot-only access (first 5% of keys), cold entries stay compressed
//   4. Cold re-access: touch cold keys to trigger decompression, measure latency

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#endif

// ── Compressor trigger ──────────────────────────────────────────────────────
// When loaded via DYLD_INSERT_LIBRARIES, alloc8 intercepts pthread_create and
// calls threadInit() for new threads. Smash defers compression until the second
// threadInit() call (the first is the main thread during early DYLD init).
// Spawning any thread triggers this second call and starts the compressor.

static void triggerCompressorStart() {
    // Need two thread creations: first increments count 0→1 (doesn't start),
    // second increments 1→2 (old value ≥ 1 → starts compressor).
    for (int i = 0; i < 2; ++i) {
        std::thread helper([] {
            volatile void* p = malloc(64);
            free(const_cast<void*>(p));
        });
        helper.join();
    }
}

// ── Configuration ───────────────────────────────────────────────────────────

static int g_cool_duration_sec = 10;
static int g_serve_duration_sec = 20;
static int g_num_entries = 1000000;
static double g_hot_fraction = 0.05;  // 5% hot — small enough that 95% of pages go cold

// ── RSS sampling ────────────────────────────────────────────────────────────

static size_t getCurrentRSSBytes() {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return info.resident_size;
    }
    return 0;
#elif defined(__linux__)
    // Use direct syscall to avoid Smash's read() interposition
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

// ── Deterministic PRNG (xoshiro256**) ───────────────────────────────────────

static uint64_t s_rng[4] = {0xA5A5A5A5ULL, 0x5A5A5A5AULL, 0x13579BDFULL, 0xFDB97531ULL};

static inline uint64_t rotl(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static uint64_t rng_next() {
    uint64_t result = rotl(s_rng[1] * 5, 7) * 9;
    uint64_t t = s_rng[1] << 17;
    s_rng[2] ^= s_rng[0];
    s_rng[3] ^= s_rng[1];
    s_rng[1] ^= s_rng[2];
    s_rng[0] ^= s_rng[3];
    s_rng[2] ^= t;
    s_rng[3] = rotl(s_rng[3], 45);
    return result;
}

static uint64_t rng_range(uint64_t max) {
    return rng_next() % max;
}

// Generate a compressible value string (~200 bytes with repeating patterns)
static std::string makeValue(uint64_t seed) {
    std::string val;
    val.reserve(200);
    // Mix of repeating words and seed-dependent content for compressibility
    static const char* words[] = {
        "the ", "data ", "value ", "store ", "cache ", "entry ", "record ",
        "field ", "index ", "query "
    };
    while (val.size() < 200) {
        if ((seed + val.size()) % 3 == 0) {
            val += words[(seed + val.size()) % 10];
        } else {
            char buf[16];
            snprintf(buf, sizeof(buf), "%04llx",
                     static_cast<unsigned long long>((seed * 2654435761ULL + val.size()) & 0xFFFF));
            val += buf;
        }
    }
    val.resize(200);
    return val;
}

// ── RSS timeline recording ───────────────────────────────────────────────

struct RssSample { double time_sec; double rss_mb; };
static std::vector<RssSample> g_rss_timeline;
static std::chrono::steady_clock::time_point g_t0;

static void recordRss() {
    double t = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - g_t0).count();
    double rss = getCurrentRSSBytes() / (1024.0 * 1024.0);
    g_rss_timeline.push_back({t, rss});
}

static void dumpRssTimeline(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "time_sec,rss_mb\n");
    for (auto& s : g_rss_timeline)
        fprintf(f, "%.2f,%.1f\n", s.time_sec, s.rss_mb);
    fclose(f);
}

// ── Main ────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // Parse flags
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--quick") == 0) {
            g_cool_duration_sec = 5;
            g_serve_duration_sec = 10;
            g_num_entries = 500000;
        }
    }

    int hot_count = std::max(1, static_cast<int>(g_num_entries * g_hot_fraction));

    g_t0 = std::chrono::steady_clock::now();
    g_rss_timeline.reserve(200);

    fprintf(stderr, "=== KV Store Benchmark ===\n");
    fprintf(stderr, "Entries: %d, Hot: %d (%.0f%%), Cool: %ds, Serve: %ds\n\n",
            g_num_entries, hot_count, g_hot_fraction * 100,
            g_cool_duration_sec, g_serve_duration_sec);

    // ── Fill phase ──────────────────────────────────────────────────────────

    std::unordered_map<std::string, std::string> store;
    store.reserve(static_cast<size_t>(g_num_entries));

    auto t_fill_start = std::chrono::steady_clock::now();
    for (int i = 0; i < g_num_entries; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "key:%07d", i);
        store[key] = makeValue(static_cast<uint64_t>(i));
    }
    auto t_fill_end = std::chrono::steady_clock::now();

    double fill_sec = std::chrono::duration<double>(t_fill_end - t_fill_start).count();
    fprintf(stderr, "Filled %d entries in %.2fs\n", g_num_entries, fill_sec);

    size_t peak_rss = getCurrentRSSBytes();
    fprintf(stderr, "Peak RSS after fill: %.1f MB\n\n", peak_rss / (1024.0 * 1024.0));
    recordRss();

    printf("METRIC fill_time_sec %.2f\n", fill_sec);
    printf("METRIC peak_rss_mb %.1f\n", peak_rss / (1024.0 * 1024.0));

    // Start the Smash compressor (no-op under system malloc)
    triggerCompressorStart();

    // ── Phase 1: Cooling ────────────────────────────────────────────────────
    // Complete idle — no memory access at all. Under Smash, the compressor
    // scans every 1s, marks untouched pages cold after 2 ticks, then compresses.
    // After ~3s, LZ4 compression should kick in.

    fprintf(stderr, "Cooling phase: %d seconds (no access, waiting for compression)...\n",
            g_cool_duration_sec);

    for (int sec = 1; sec <= g_cool_duration_sec; ++sec) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        size_t rss = getCurrentRSSBytes();
        fprintf(stderr, "  t=%2ds: RSS=%.1f MB\n", sec, rss / (1024.0 * 1024.0));
        recordRss();
    }

    size_t post_cool_rss = getCurrentRSSBytes();
    double cool_reduction = (peak_rss > 0)
        ? (1.0 - static_cast<double>(post_cool_rss) / static_cast<double>(peak_rss)) * 100.0
        : 0.0;

    fprintf(stderr, "Post-cooling RSS: %.1f MB (%.1f%% reduction from peak)\n\n",
            post_cool_rss / (1024.0 * 1024.0), cool_reduction);

    printf("METRIC post_cool_rss_mb %.1f\n", post_cool_rss / (1024.0 * 1024.0));
    printf("METRIC cool_reduction_pct %.1f\n", cool_reduction);

    // ── Phase 2: Hot-only serve ─────────────────────────────────────────────
    // Access ONLY the first 5% of keys. Since these were inserted first, their
    // map nodes and value strings are allocated on early pages. The remaining
    // 95% of entries sit on pages that stay cold and compressed.

    fprintf(stderr, "Serve phase: %d seconds (hot-only, keys 0..%d)...\n",
            g_serve_duration_sec, hot_count - 1);

    // Pre-generate hot key strings to avoid touching any cold pages during lookup
    std::vector<std::string> hot_keys;
    hot_keys.reserve(static_cast<size_t>(hot_count));
    for (int i = 0; i < hot_count; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "key:%07d", i);
        hot_keys.emplace_back(key);
    }

    size_t min_serve_rss = getCurrentRSSBytes();
    long total_ops = 0;

    // Latency tracking for hot accesses (sample every 16th op)
    std::vector<double> hot_latencies;
    hot_latencies.reserve(500000);

    for (int sec = 0; sec < g_serve_duration_sec; ++sec) {
        int ops_this_sec = 0;
        auto sec_start = std::chrono::steady_clock::now();

        while (true) {
            int idx = static_cast<int>(rng_range(static_cast<uint64_t>(hot_count)));
            const std::string& key = hot_keys[static_cast<size_t>(idx)];

            auto op_start = std::chrono::steady_clock::now();

            // 90% reads, 10% writes (writes update value in-place)
            if (rng_range(100) < 90) {
                auto it = store.find(key);
                if (it != store.end()) {
                    volatile char c = it->second[0];
                    (void)c;
                }
            } else {
                store[key] = makeValue(rng_next());
            }

            auto op_end = std::chrono::steady_clock::now();

            if ((ops_this_sec & 0xF) == 0) {
                hot_latencies.push_back(
                    std::chrono::duration<double, std::micro>(op_end - op_start).count());
            }
            ops_this_sec++;

            auto elapsed = std::chrono::steady_clock::now() - sec_start;
            if (elapsed >= std::chrono::seconds(1)) break;
        }

        total_ops += ops_this_sec;
        size_t rss = getCurrentRSSBytes();
        if (rss > peak_rss) peak_rss = rss;
        if (rss < min_serve_rss) min_serve_rss = rss;

        fprintf(stderr, "  t=%2ds: RSS=%.1f MB  ops=%d\n",
                sec + 1, rss / (1024.0 * 1024.0), ops_this_sec);
        recordRss();
    }

    size_t steady_rss = getCurrentRSSBytes();
    double serve_reduction = (peak_rss > 0)
        ? (1.0 - static_cast<double>(min_serve_rss) / static_cast<double>(peak_rss)) * 100.0
        : 0.0;
    double ops_per_sec = static_cast<double>(total_ops) / g_serve_duration_sec;

    // Hot latency percentiles
    std::sort(hot_latencies.begin(), hot_latencies.end());
    double hot_p50 = hot_latencies.empty() ? 0 : hot_latencies[hot_latencies.size() / 2];
    double hot_p99 = hot_latencies.empty() ? 0 : hot_latencies[static_cast<size_t>(hot_latencies.size() * 0.99)];

    printf("METRIC steady_rss_mb %.1f\n", steady_rss / (1024.0 * 1024.0));
    printf("METRIC min_rss_mb %.1f\n", min_serve_rss / (1024.0 * 1024.0));
    printf("METRIC rss_reduction_pct %.1f\n", serve_reduction);
    printf("METRIC ops_per_sec %.0f\n", ops_per_sec);
    printf("METRIC hot_p50_us %.2f\n", hot_p50);
    printf("METRIC hot_p99_us %.2f\n", hot_p99);

    // ── Phase 3: Cold re-access ─────────────────────────────────────────────
    // Touch 10% of cold keys to trigger decompression faults and measure latency.

    int cold_start = hot_count;
    int cold_count = g_num_entries - hot_count;
    int cold_sample = std::max(1, cold_count / 10);

    fprintf(stderr, "\nCold re-access: touching %d cold keys...\n", cold_sample);

    std::vector<double> cold_latencies;
    cold_latencies.reserve(static_cast<size_t>(cold_sample));

    for (int i = 0; i < cold_sample; ++i) {
        int idx = cold_start + static_cast<int>(rng_range(static_cast<uint64_t>(cold_count)));
        char key[16];
        snprintf(key, sizeof(key), "key:%07d", idx);

        auto t0 = std::chrono::steady_clock::now();
        auto it = store.find(key);
        if (it != store.end()) {
            volatile char c = it->second[0];
            (void)c;
        }
        auto t1 = std::chrono::steady_clock::now();

        cold_latencies.push_back(
            std::chrono::duration<double, std::micro>(t1 - t0).count());
    }

    std::sort(cold_latencies.begin(), cold_latencies.end());
    double cold_p50 = cold_latencies.empty() ? 0 : cold_latencies[cold_latencies.size() / 2];
    double cold_p99 = cold_latencies.empty() ? 0 : cold_latencies[static_cast<size_t>(cold_latencies.size() * 0.99)];

    size_t post_reaccess_rss = getCurrentRSSBytes();
    recordRss();

    fprintf(stderr, "Cold access p50/p99: %.1f / %.1f us\n", cold_p50, cold_p99);
    fprintf(stderr, "Post re-access RSS: %.1f MB\n", post_reaccess_rss / (1024.0 * 1024.0));

    printf("METRIC cold_p50_us %.2f\n", cold_p50);
    printf("METRIC cold_p99_us %.2f\n", cold_p99);
    printf("METRIC post_reaccess_rss_mb %.1f\n", post_reaccess_rss / (1024.0 * 1024.0));

    // Dump RSS timeline if requested
    if (const char* rss_dir = std::getenv("SMASH_RSS_DIR")) {
        dumpRssTimeline((std::string(rss_dir) + "/kv_rss.csv").c_str());
    }

    // Dump CDF data if requested
    if (const char* lat_dir = std::getenv("SMASH_LATENCY_DIR")) {
        std::string path = std::string(lat_dir) + "/kv_cold.csv";
        FILE* f = fopen(path.c_str(), "w");
        if (f) {
            fprintf(f, "latency_us\n");
            for (double v : cold_latencies) fprintf(f, "%.3f\n", v);
            fclose(f);
        }
        std::string hot_path = std::string(lat_dir) + "/kv_hot.csv";
        f = fopen(hot_path.c_str(), "w");
        if (f) {
            fprintf(f, "latency_us\n");
            for (double v : hot_latencies) fprintf(f, "%.3f\n", v);
            fclose(f);
        }
    }

    fprintf(stderr, "\nResults:\n");
    fprintf(stderr, "  Peak RSS: %.1f MB\n", peak_rss / (1024.0 * 1024.0));
    fprintf(stderr, "  Post-cooling RSS: %.1f MB (%.1f%% reduction)\n",
            post_cool_rss / (1024.0 * 1024.0), cool_reduction);
    fprintf(stderr, "  Steady-state RSS: %.1f MB\n", steady_rss / (1024.0 * 1024.0));
    fprintf(stderr, "  Min serve RSS: %.1f MB (%.1f%% reduction)\n",
            min_serve_rss / (1024.0 * 1024.0), serve_reduction);
    fprintf(stderr, "  Ops/sec: %.0f\n", ops_per_sec);
    fprintf(stderr, "  Hot p50/p99: %.2f / %.2f us\n", hot_p50, hot_p99);
    fprintf(stderr, "  Cold p50/p99: %.1f / %.1f us\n", cold_p50, cold_p99);

    return 0;
}
