// bench_sqlite.cpp - SQLite in-memory database workload for A/B comparison
//
// Uses standard malloc/free (via SQLite amalgamation). Run directly for system
// malloc baseline, or with DYLD_INSERT_LIBRARIES=libsmash.dylib for Smash.
//
// Workload phases:
//   1. Fill: 500K rows in in-memory DB (~200-300MB heap, compressible text)
//   2. Cooling: complete idle so all pages go cold and compress
//   3. Serve: query only recent rows (top 5%) — simulates time-series access
//   4. Cold re-access: SELECT old rows (bottom 50%) to measure decompression latency

#include "sqlite3.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <string>
#include <vector>
#include <algorithm>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

// ── Compressor trigger ──────────────────────────────────────────────────────

static void triggerCompressorStart() {
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
static int g_num_rows = 500000;
static double g_hot_fraction = 0.05;  // 5% hot — recent rows only

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
    // Use direct read() to avoid malloc re-entry when Smash is active.
    // /proc/self/statm fields: size resident shared text lib data dt (in pages)
    int fd = open("/proc/self/statm", O_RDONLY);
    if (fd < 0) return 0;
    char buf[128];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    long sz_pages = 0, resident_pages = 0;
    if (sscanf(buf, "%ld %ld", &sz_pages, &resident_pages) >= 2) {
        return static_cast<size_t>(resident_pages) * 4096;
    }
    return 0;
#else
    return 0;
#endif
}

// ── Deterministic PRNG (xoshiro256**) ───────────────────────────────────────

static uint64_t s_rng[4] = {0xBEEFCAFEULL, 0xDEAD1234ULL, 0x42424242ULL, 0xABCD9876ULL};

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

// Generate a compressible text string (~300 bytes with repeating word patterns)
static std::string makeText(uint64_t seed) {
    std::string val;
    val.reserve(300);
    static const char* words[] = {
        "the ", "data ", "value ", "store ", "cache ", "entry ", "record ",
        "field ", "index ", "query ", "result ", "table ", "column ", "row "
    };
    while (val.size() < 300) {
        if ((seed + val.size()) % 3 == 0) {
            val += words[(seed + val.size()) % 14];
        } else {
            char buf[16];
            snprintf(buf, sizeof(buf), "%04llx",
                     static_cast<unsigned long long>((seed * 2654435761ULL + val.size()) & 0xFFFF));
            val += buf;
        }
    }
    val.resize(300);
    return val;
}

// Helper to execute SQL and check for errors
static void exec_sql(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n  statement: %s\n", err, sql);
        sqlite3_free(err);
    }
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
            g_num_rows = 250000;
        }
    }

    int hot_start = g_num_rows - static_cast<int>(g_num_rows * g_hot_fraction);
    int hot_count = g_num_rows - hot_start;

    fprintf(stderr, "=== SQLite In-Memory Benchmark ===\n");
    fprintf(stderr, "Rows: %d, Hot: %d (top %.0f%% by id), Cool: %ds, Serve: %ds\n\n",
            g_num_rows, hot_count, g_hot_fraction * 100,
            g_cool_duration_sec, g_serve_duration_sec);

    g_t0 = std::chrono::steady_clock::now();
    g_rss_timeline.reserve(200);

    // ── Open in-memory database ────────────────────────────────────────────

    sqlite3* db = nullptr;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        fprintf(stderr, "ERROR: Failed to open in-memory database\n");
        return 1;
    }

    exec_sql(db, "PRAGMA journal_mode=OFF");
    exec_sql(db, "PRAGMA synchronous=OFF");

    exec_sql(db,
        "CREATE TABLE records ("
        "  id INTEGER PRIMARY KEY,"
        "  name TEXT NOT NULL,"
        "  email TEXT NOT NULL,"
        "  bio TEXT NOT NULL,"
        "  json_data TEXT NOT NULL,"
        "  created_at TEXT NOT NULL"
        ")");

    // ── Fill phase ─────────────────────────────────────────────────────────

    auto t_fill_start = std::chrono::steady_clock::now();

    exec_sql(db, "BEGIN TRANSACTION");

    sqlite3_stmt* insert_stmt = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT INTO records (id, name, email, bio, json_data, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        -1, &insert_stmt, nullptr);

    for (int i = 0; i < g_num_rows; ++i) {
        uint64_t seed = static_cast<uint64_t>(i);

        // name: ~20 chars
        char name[32];
        snprintf(name, sizeof(name), "user_%07d", i);

        // email
        char email[48];
        snprintf(email, sizeof(email), "user%07d@example.com", i);

        // bio: ~300 byte compressible text
        std::string bio = makeText(seed);

        // json_data: ~300 byte compressible text (simulates JSON blob)
        std::string json_data = makeText(seed * 7 + 13);

        // created_at: timestamp-like string
        char created_at[32];
        snprintf(created_at, sizeof(created_at), "2024-%02d-%02d %02d:%02d:%02d",
                 1 + static_cast<int>(rng_range(12)),
                 1 + static_cast<int>(rng_range(28)),
                 static_cast<int>(rng_range(24)),
                 static_cast<int>(rng_range(60)),
                 static_cast<int>(rng_range(60)));

        sqlite3_bind_int(insert_stmt, 1, i);
        sqlite3_bind_text(insert_stmt, 2, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 3, email, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 4, bio.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 5, json_data.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_stmt, 6, created_at, -1, SQLITE_TRANSIENT);

        sqlite3_step(insert_stmt);
        sqlite3_reset(insert_stmt);

        if ((i + 1) % 100000 == 0) {
            fprintf(stderr, "  Inserted %d/%d rows...\n", i + 1, g_num_rows);
        }
    }

    sqlite3_finalize(insert_stmt);
    exec_sql(db, "COMMIT");

    auto t_fill_end = std::chrono::steady_clock::now();
    double fill_sec = std::chrono::duration<double>(t_fill_end - t_fill_start).count();
    fprintf(stderr, "Filled %d rows in %.2fs\n", g_num_rows, fill_sec);

    size_t peak_rss = getCurrentRSSBytes();
    fprintf(stderr, "Peak RSS after fill: %.1f MB\n\n", peak_rss / (1024.0 * 1024.0));
    recordRss();

    printf("METRIC fill_time_sec %.2f\n", fill_sec);
    printf("METRIC peak_rss_mb %.1f\n", peak_rss / (1024.0 * 1024.0));

    // Start the Smash compressor (no-op under system malloc)
    triggerCompressorStart();

    // ── Phase 1: Cooling ───────────────────────────────────────────────────

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

    // ── Phase 2: Hot-only serve ────────────────────────────────────────────
    // Simulate time-series access: only query recent rows (top 5% by id).
    // Mix: 70% point SELECT by id, 20% range SELECT (last 1000 rows), 10% INSERT

    fprintf(stderr, "Serve phase: %d seconds (hot-only, ids %d..%d)...\n",
            g_serve_duration_sec, hot_start, g_num_rows - 1);

    // Prepare statements
    sqlite3_stmt* select_by_id = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT id, name, email, bio FROM records WHERE id = ?",
        -1, &select_by_id, nullptr);

    sqlite3_stmt* select_range = nullptr;
    char range_sql[128];
    snprintf(range_sql, sizeof(range_sql),
        "SELECT id, name FROM records WHERE id >= ? ORDER BY id LIMIT 1000");
    sqlite3_prepare_v2(db, range_sql, -1, &select_range, nullptr);

    sqlite3_stmt* insert_new = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO records (id, name, email, bio, json_data, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        -1, &insert_new, nullptr);

    int next_id = g_num_rows;
    size_t min_serve_rss = getCurrentRSSBytes();
    long total_ops = 0;

    std::vector<double> hot_latencies;
    hot_latencies.reserve(500000);

    for (int sec = 0; sec < g_serve_duration_sec; ++sec) {
        int ops_this_sec = 0;
        auto sec_start = std::chrono::steady_clock::now();

        while (true) {
            auto op_start = std::chrono::steady_clock::now();

            int r = static_cast<int>(rng_range(100));
            if (r < 70) {
                // 70%: point SELECT by id in hot range
                int id = hot_start + static_cast<int>(rng_range(static_cast<uint64_t>(hot_count)));
                sqlite3_bind_int(select_by_id, 1, id);
                while (sqlite3_step(select_by_id) == SQLITE_ROW) {
                    // Touch the result columns
                    volatile const unsigned char* v = sqlite3_column_text(select_by_id, 3);
                    (void)v;
                }
                sqlite3_reset(select_by_id);
            } else if (r < 90) {
                // 20%: range SELECT within hot range
                int start = hot_start + static_cast<int>(rng_range(static_cast<uint64_t>(
                    hot_count > 1000 ? hot_count - 1000 : 1)));
                sqlite3_bind_int(select_range, 1, start);
                while (sqlite3_step(select_range) == SQLITE_ROW) {
                    volatile const unsigned char* v = sqlite3_column_text(select_range, 1);
                    (void)v;
                }
                sqlite3_reset(select_range);
            } else {
                // 10%: INSERT new row
                char name[32], email[48], created[32];
                snprintf(name, sizeof(name), "user_%07d", next_id);
                snprintf(email, sizeof(email), "user%07d@example.com", next_id);
                snprintf(created, sizeof(created), "2025-01-01 00:00:%02d", next_id % 60);
                std::string bio = makeText(static_cast<uint64_t>(next_id));
                std::string jdata = makeText(static_cast<uint64_t>(next_id) * 7 + 13);

                sqlite3_bind_int(insert_new, 1, next_id);
                sqlite3_bind_text(insert_new, 2, name, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_new, 3, email, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_new, 4, bio.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_new, 5, jdata.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_new, 6, created, -1, SQLITE_TRANSIENT);
                sqlite3_step(insert_new);
                sqlite3_reset(insert_new);
                next_id++;
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

    sqlite3_finalize(select_by_id);
    sqlite3_finalize(select_range);
    sqlite3_finalize(insert_new);

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

    // ── Phase 3: Cold re-access ────────────────────────────────────────────
    // Query old rows (bottom 50%) to trigger decompression faults.

    int cold_count = g_num_rows / 2;  // bottom 50%
    int cold_sample = std::max(1, cold_count / 10);

    fprintf(stderr, "\nCold re-access: querying %d old rows (ids 0..%d)...\n",
            cold_sample, cold_count - 1);

    sqlite3_stmt* cold_select = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT id, name, email, bio, json_data FROM records WHERE id = ?",
        -1, &cold_select, nullptr);

    std::vector<double> cold_latencies;
    cold_latencies.reserve(static_cast<size_t>(cold_sample));

    for (int i = 0; i < cold_sample; ++i) {
        int id = static_cast<int>(rng_range(static_cast<uint64_t>(cold_count)));

        auto t0 = std::chrono::steady_clock::now();
        sqlite3_bind_int(cold_select, 1, id);
        while (sqlite3_step(cold_select) == SQLITE_ROW) {
            volatile const unsigned char* v = sqlite3_column_text(cold_select, 3);
            (void)v;
            v = sqlite3_column_text(cold_select, 4);
            (void)v;
        }
        sqlite3_reset(cold_select);
        auto t1 = std::chrono::steady_clock::now();

        cold_latencies.push_back(
            std::chrono::duration<double, std::micro>(t1 - t0).count());
    }

    sqlite3_finalize(cold_select);

    std::sort(cold_latencies.begin(), cold_latencies.end());
    double cold_p50 = cold_latencies.empty() ? 0 : cold_latencies[cold_latencies.size() / 2];
    double cold_p99 = cold_latencies.empty() ? 0 : cold_latencies[static_cast<size_t>(cold_latencies.size() * 0.99)];

    size_t post_reaccess_rss = getCurrentRSSBytes();
    recordRss();

    fprintf(stderr, "Cold query p50/p99: %.1f / %.1f us\n", cold_p50, cold_p99);
    fprintf(stderr, "Post re-access RSS: %.1f MB\n", post_reaccess_rss / (1024.0 * 1024.0));

    printf("METRIC cold_p50_us %.2f\n", cold_p50);
    printf("METRIC cold_p99_us %.2f\n", cold_p99);
    printf("METRIC post_reaccess_rss_mb %.1f\n", post_reaccess_rss / (1024.0 * 1024.0));

    // Dump RSS timeline if requested
    if (const char* rss_dir = std::getenv("SMASH_RSS_DIR")) {
        dumpRssTimeline((std::string(rss_dir) + "/sqlite_rss.csv").c_str());
    }

    // Dump CDF data if requested
    if (const char* lat_dir = std::getenv("SMASH_LATENCY_DIR")) {
        std::string path = std::string(lat_dir) + "/sqlite_cold.csv";
        FILE* f = fopen(path.c_str(), "w");
        if (f) {
            fprintf(f, "latency_us\n");
            for (double v : cold_latencies) fprintf(f, "%.3f\n", v);
            fclose(f);
        }
        std::string hot_path = std::string(lat_dir) + "/sqlite_hot.csv";
        f = fopen(hot_path.c_str(), "w");
        if (f) {
            fprintf(f, "latency_us\n");
            for (double v : hot_latencies) fprintf(f, "%.3f\n", v);
            fclose(f);
        }
    }

    fprintf(stderr, "\nResults:\n");
    fprintf(stderr, "  Fill time: %.2fs\n", fill_sec);
    fprintf(stderr, "  Peak RSS: %.1f MB\n", peak_rss / (1024.0 * 1024.0));
    fprintf(stderr, "  Post-cooling RSS: %.1f MB (%.1f%% reduction)\n",
            post_cool_rss / (1024.0 * 1024.0), cool_reduction);
    fprintf(stderr, "  Steady-state RSS: %.1f MB\n", steady_rss / (1024.0 * 1024.0));
    fprintf(stderr, "  Min serve RSS: %.1f MB (%.1f%% reduction)\n",
            min_serve_rss / (1024.0 * 1024.0), serve_reduction);
    fprintf(stderr, "  Ops/sec: %.0f\n", ops_per_sec);
    fprintf(stderr, "  Hot p50/p99: %.2f / %.2f us\n", hot_p50, hot_p99);
    fprintf(stderr, "  Cold p50/p99: %.1f / %.1f us\n", cold_p50, cold_p99);

    sqlite3_close(db);
    return 0;
}
