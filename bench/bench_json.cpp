// bench_json.cpp - JSON processing workload for A/B comparison
//
// Uses standard malloc/free (via cJSON). Run directly for system malloc baseline,
// or with DYLD_INSERT_LIBRARIES=libsmash.dylib for Smash comparison.
//
// Workload phases:
//   1. Generate ~50MB JSON, parse into DOM tree (hundreds of thousands of allocations)
//   2. Cooling: idle for several seconds so pages go cold and compress
//   3. Serve: access only hot records (top 1%) via O(1) index; cold records stay compressed
//   4. Cold re-access: touch cold records to trigger decompression faults

#include "cJSON.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
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
static int g_num_records = 100000;
static double g_hot_fraction = 0.01;  // 1% hot — keeps hot set small for clear cold separation

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
    // Use direct syscall to avoid Smash's read() interposition which can fail
    // when ifstream's internal buffer is in compressed memory
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

static uint64_t s_rng[4] = {0x12345678ULL, 0x9ABCDEF0ULL, 0xDEADBEEFULL, 0xCAFEBABEULL};

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

// Generate a random-ish string of given length
static std::string rng_string(size_t len) {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
    std::string s(len, ' ');
    for (size_t i = 0; i < len; ++i) {
        s[i] = charset[rng_next() % (sizeof(charset) - 1)];
    }
    return s;
}

// ── JSON generation ─────────────────────────────────────────────────────────

static std::string generateJSON(int num_records) {
    fprintf(stderr, "Generating JSON with %d records...\n", num_records);

    std::string json;
    json.reserve(static_cast<size_t>(num_records) * 500);
    json += "[\n";

    for (int i = 0; i < num_records; ++i) {
        if (i > 0) json += ",\n";
        json += "  {";

        // Basic fields
        json += "\"id\":" + std::to_string(i);
        json += ",\"name\":\"" + rng_string(15 + rng_range(10)) + "\"";
        json += ",\"email\":\"" + rng_string(10) + "@example.com\"";
        json += ",\"age\":" + std::to_string(18 + rng_range(60));
        json += ",\"active\":" + std::string(rng_range(2) ? "true" : "false");

        // Nested address object
        json += ",\"address\":{";
        json += "\"street\":\"" + rng_string(20 + rng_range(15)) + "\"";
        json += ",\"city\":\"" + rng_string(8 + rng_range(8)) + "\"";
        json += ",\"state\":\"" + rng_string(2) + "\"";
        json += ",\"zip\":\"" + std::to_string(10000 + rng_range(90000)) + "\"";
        json += "}";

        // Tags array (3-6 strings)
        int num_tags = 3 + static_cast<int>(rng_range(4));
        json += ",\"tags\":[";
        for (int t = 0; t < num_tags; ++t) {
            if (t > 0) json += ",";
            json += "\"" + rng_string(5 + rng_range(10)) + "\"";
        }
        json += "]";

        // Scores array (5 numbers)
        json += ",\"scores\":[";
        for (int s = 0; s < 5; ++s) {
            if (s > 0) json += ",";
            json += std::to_string(rng_range(100));
        }
        json += "]";

        // Bio field (longer string to add bulk)
        json += ",\"bio\":\"" + rng_string(80 + rng_range(120)) + "\"";

        json += "}";
    }

    json += "\n]\n";
    return json;
}

// ── RSS timeline recording ─────────────────────────────────────────────────

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
            g_num_records = 50000;
        }
    }

    int hot_count = std::max(1, static_cast<int>(g_num_records * g_hot_fraction));

    g_t0 = std::chrono::steady_clock::now();
    g_rss_timeline.reserve(200);

    fprintf(stderr, "=== JSON Processing Benchmark ===\n");
    fprintf(stderr, "Records: %d, Hot: %d (%.0f%%), Cool: %ds, Serve: %ds\n\n",
            g_num_records, hot_count, g_hot_fraction * 100,
            g_cool_duration_sec, g_serve_duration_sec);

    // ── Generate & parse JSON ───────────────────────────────────────────────

    auto t_gen_start = std::chrono::steady_clock::now();
    std::string json_text = generateJSON(g_num_records);
    auto t_gen_end = std::chrono::steady_clock::now();

    double gen_sec = std::chrono::duration<double>(t_gen_end - t_gen_start).count();
    double json_mb = json_text.size() / (1024.0 * 1024.0);
    fprintf(stderr, "Generated %.1f MB JSON in %.2fs\n", json_mb, gen_sec);

    auto t_parse_start = std::chrono::steady_clock::now();
    cJSON* root = cJSON_Parse(json_text.c_str());
    auto t_parse_end = std::chrono::steady_clock::now();

    if (!root) {
        fprintf(stderr, "ERROR: cJSON_Parse failed\n");
        return 1;
    }

    double parse_sec = std::chrono::duration<double>(t_parse_end - t_parse_start).count();
    double parse_throughput = json_mb / parse_sec;
    fprintf(stderr, "Parsed in %.2fs (%.1f MB/s)\n", parse_sec, parse_throughput);

    printf("METRIC json_size_mb %.1f\n", json_mb);
    printf("METRIC parse_throughput_mbs %.1f\n", parse_throughput);

    // Free the raw JSON text — only the DOM tree remains
    { std::string().swap(json_text); }

    // ── Build O(1) index ────────────────────────────────────────────────────
    // cJSON_GetArrayItem is O(n) linked-list walk — we need direct pointers
    // so hot accesses don't touch cold nodes.

    int array_size = cJSON_GetArraySize(root);
    std::vector<cJSON*> index;
    index.reserve(static_cast<size_t>(array_size));
    for (cJSON* child = root->child; child; child = child->next) {
        index.push_back(child);
    }
    fprintf(stderr, "Built index for %zu records\n", index.size());

    size_t peak_rss = getCurrentRSSBytes();
    fprintf(stderr, "Post-parse RSS (DOM only): %.1f MB\n\n", peak_rss / (1024.0 * 1024.0));
    recordRss();

    // Start the Smash compressor (no-op under system malloc)
    triggerCompressorStart();

    // ── Phase 1: Cooling ────────────────────────────────────────────────────
    // Complete idle — no memory access. Under Smash, the compressor thread
    // scans every 1s, marks untouched pages cold after 2 ticks, then compresses.
    // After ~3s idle, LZ4 compression should be visible.

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

    printf("METRIC peak_rss_mb %.1f\n", peak_rss / (1024.0 * 1024.0));
    printf("METRIC post_cool_rss_mb %.1f\n", post_cool_rss / (1024.0 * 1024.0));
    printf("METRIC cool_reduction_pct %.1f\n", cool_reduction);

    // ── Phase 2: Hot-only serve ─────────────────────────────────────────────
    // Access ONLY the hot set (first 1% of records) via direct index.
    // Cold records (99%) stay compressed — their pages are never touched.

    fprintf(stderr, "Serve phase: %d seconds (hot-only, %d records)...\n",
            g_serve_duration_sec, hot_count);

    size_t min_serve_rss = getCurrentRSSBytes();
    long total_accesses = 0;

    for (int sec = 0; sec < g_serve_duration_sec; ++sec) {
        auto t0 = std::chrono::steady_clock::now();
        int accesses_this_sec = 0;

        while (true) {
            int idx = static_cast<int>(rng_range(static_cast<uint64_t>(hot_count)));
            cJSON* record = index[static_cast<size_t>(idx)];

            // Walk the record's direct children (up to 3) via next pointers.
            // Avoids cJSON_GetObjectItemCaseSensitive which does string comparisons
            // that trigger many page faults on compressed nodes.
            if (record) {
                cJSON* child = record->child;
                for (int f = 0; f < 3 && child; ++f, child = child->next) {
                    if (child->valuestring) {
                        volatile char c = child->valuestring[0];
                        (void)c;
                    }
                }
            }
            accesses_this_sec++;

            auto elapsed = std::chrono::steady_clock::now() - t0;
            if (elapsed >= std::chrono::seconds(1)) break;
        }

        total_accesses += accesses_this_sec;
        size_t rss = getCurrentRSSBytes();
        if (rss < min_serve_rss) min_serve_rss = rss;

        fprintf(stderr, "  t=%2ds: RSS=%.1f MB  accesses=%d\n",
                sec + 1, rss / (1024.0 * 1024.0), accesses_this_sec);
        recordRss();
    }

    size_t steady_rss = getCurrentRSSBytes();
    double serve_reduction = (peak_rss > 0)
        ? (1.0 - static_cast<double>(min_serve_rss) / static_cast<double>(peak_rss)) * 100.0
        : 0.0;

    printf("METRIC steady_rss_mb %.1f\n", steady_rss / (1024.0 * 1024.0));
    printf("METRIC min_rss_mb %.1f\n", min_serve_rss / (1024.0 * 1024.0));
    printf("METRIC rss_reduction_pct %.1f\n", serve_reduction);
    printf("METRIC total_accesses %ld\n", total_accesses);
    printf("METRIC accesses_per_sec %.0f\n",
           static_cast<double>(total_accesses) / g_serve_duration_sec);

    // ── Phase 3: Cold re-access ─────────────────────────────────────────────
    // Touch 10% of cold records to trigger decompression and measure latency.

    int cold_start = hot_count;
    int cold_count = array_size - hot_count;
    int cold_sample = std::max(1, cold_count / 10);

    fprintf(stderr, "\nCold re-access: touching %d cold records...\n", cold_sample);

    std::vector<double> cold_latencies;
    cold_latencies.reserve(static_cast<size_t>(cold_sample));

    for (int i = 0; i < cold_sample; ++i) {
        int idx = cold_start + static_cast<int>(rng_range(static_cast<uint64_t>(cold_count)));
        cJSON* record = index[static_cast<size_t>(idx)];

        auto t0 = std::chrono::steady_clock::now();
        cJSON* bio = cJSON_GetObjectItemCaseSensitive(record, "bio");
        if (bio && bio->valuestring) {
            volatile char c = bio->valuestring[0];
            (void)c;
        }
        auto t1 = std::chrono::steady_clock::now();

        cold_latencies.push_back(
            std::chrono::duration<double, std::micro>(t1 - t0).count());
    }

    std::sort(cold_latencies.begin(), cold_latencies.end());
    double cold_p50 = cold_latencies[cold_latencies.size() / 2];
    double cold_p99 = cold_latencies[static_cast<size_t>(cold_latencies.size() * 0.99)];

    size_t post_reaccess_rss = getCurrentRSSBytes();
    recordRss();
    fprintf(stderr, "Cold access p50/p99: %.1f / %.1f us\n", cold_p50, cold_p99);
    fprintf(stderr, "Post re-access RSS: %.1f MB\n", post_reaccess_rss / (1024.0 * 1024.0));

    printf("METRIC cold_p50_us %.1f\n", cold_p50);
    printf("METRIC cold_p99_us %.1f\n", cold_p99);
    printf("METRIC post_reaccess_rss_mb %.1f\n", post_reaccess_rss / (1024.0 * 1024.0));

    // Dump RSS timeline if requested
    if (const char* rss_dir = std::getenv("SMASH_RSS_DIR")) {
        dumpRssTimeline((std::string(rss_dir) + "/json_rss.csv").c_str());
    }

    // Dump CDF data if requested
    if (const char* lat_dir = std::getenv("SMASH_LATENCY_DIR")) {
        std::string path = std::string(lat_dir) + "/json_cold.csv";
        FILE* f = fopen(path.c_str(), "w");
        if (f) {
            fprintf(f, "latency_us\n");
            for (double v : cold_latencies) fprintf(f, "%.3f\n", v);
            fclose(f);
        }
    }

    // Cleanup
    cJSON_Delete(root);

    return 0;
}
