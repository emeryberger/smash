// bench_rocksdb.cpp - RocksDB block cache benchmark
//
// Populates a RocksDB database, then does hot reads (recent keys) while
// older data goes cold in the block cache. Measures RSS at each phase.
//
// Build: link against -lrocksdb
// Usage: bench_rocksdb --db /tmp/testdb --keys 500000 --value-size 512 --cool 10 --serve 20

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/cache.h>
#include <rocksdb/filter_policy.h>
// ROCKSDB_MAJOR — included explicitly rather than relying on db.h pulling it
// in, so the version test below can never silently evaluate 0 >= 10 and pick
// the wrong DB::Open overload.
#include <rocksdb/version.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <chrono>
#include <random>
#include <thread>
#include <vector>
#include <algorithm>
#include <sys/resource.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#endif

static double get_rss_mb() {
#if defined(__APPLE__)
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) == KERN_SUCCESS) {
        return (double)info.resident_size / (1024.0 * 1024.0);
    }
    return 0.0;
#elif defined(__linux__)
    int fd = open("/proc/self/statm", O_RDONLY);
    if (fd < 0) return 0.0;
    char buf[128];
    ssize_t n = syscall(SYS_read, fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0.0;
    buf[n] = '\0';
    long sz_pages = 0, resident_pages = 0;
    if (sscanf(buf, "%ld %ld", &sz_pages, &resident_pages) >= 2) {
        return (double)(resident_pages * 4096) / (1024.0 * 1024.0);
    }
    return 0.0;
#else
    return 0.0;
#endif
}

static std::string make_key(int i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "key:%08d", i);
    return std::string(buf);
}

static std::string make_value(int i, int size) {
    // JSON-like value with some structure for compressibility
    std::string val;
    val.reserve(size);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"name\":\"record_%08d\",\"score\":%d,\"group\":%d,\"data\":\"",
             i, i, i % 1000, i % 50);
    val = buf;
    // Pad with semi-compressible content
    const char* padding = "the_data_value_store_cache_entry_record_field_index_query_";
    while ((int)val.size() < size - 2) {
        int remaining = size - 2 - (int)val.size();
        int chunk = remaining < 57 ? remaining : 57;
        val.append(padding, chunk);
    }
    val += "\"}";
    if ((int)val.size() > size) val.resize(size);
    return val;
}

struct RssSample { double time_sec; double rss_mb; };
static std::vector<RssSample> g_rss_timeline;
static std::chrono::steady_clock::time_point g_t0;

static void recordRss() {
    double t = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - g_t0).count();
    double rss = get_rss_mb();
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

int main(int argc, char* argv[]) {
    std::string db_path = "/tmp/bench_rocksdb_test";
    int num_keys = 500000;
    int value_size = 512;
    int cool_duration = 10;
    int serve_duration = 20;
    std::string compression_name = "none";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) db_path = argv[++i];
        else if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc) num_keys = atoi(argv[++i]);
        else if (strcmp(argv[i], "--value-size") == 0 && i + 1 < argc) value_size = atoi(argv[++i]);
        else if (strcmp(argv[i], "--cool") == 0 && i + 1 < argc) cool_duration = atoi(argv[++i]);
        else if (strcmp(argv[i], "--serve") == 0 && i + 1 < argc) serve_duration = atoi(argv[++i]);
        else if (strcmp(argv[i], "--compression") == 0 && i + 1 < argc) compression_name = argv[++i];
    }

    // Map compression name to RocksDB enum
    rocksdb::CompressionType compression_type = rocksdb::kNoCompression;
    if (compression_name == "lz4") compression_type = rocksdb::kLZ4Compression;
    else if (compression_name == "snappy") compression_type = rocksdb::kSnappyCompression;
    else if (compression_name == "zstd") compression_type = rocksdb::kZSTD;
    else if (compression_name == "zlib") compression_type = rocksdb::kZlibCompression;
    else if (compression_name != "none") {
        fprintf(stderr, "Unknown compression: %s (use none/lz4/snappy/zstd/zlib)\n",
                compression_name.c_str());
        return 1;
    }

    g_t0 = std::chrono::steady_clock::now();
    g_rss_timeline.reserve(200);

    fprintf(stderr, "=== RocksDB Block Cache Benchmark ===\n");
    fprintf(stderr, "Keys: %d, Value size: %d, Cool: %ds, Serve: %ds, Compression: %s\n\n",
            num_keys, value_size, cool_duration, serve_duration, compression_name.c_str());

    // Configure RocksDB with a large block cache
    rocksdb::Options options;
    options.create_if_missing = true;
    options.write_buffer_size = 64 * 1024 * 1024;  // 64MB memtable
    options.max_write_buffer_number = 2;
    options.target_file_size_base = 64 * 1024 * 1024;

    rocksdb::BlockBasedTableOptions table_options;
    // 256MB block cache — large enough that cold blocks stay resident
    table_options.block_cache = rocksdb::NewLRUCache(256 * 1024 * 1024);
    table_options.block_size = 16 * 1024;  // 16KB blocks
    table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
    options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

    options.compression = compression_type;

    // RocksDB 10.0.1 replaced the `DB**` overload of DB::Open with
    // `std::unique_ptr<DB>*` (9.x has only the raw form, 10+ only the
    // unique_ptr form).  Normalize on the owning unique_ptr so every `db->`
    // use below — and the close at the end of main — is version-independent.
    std::unique_ptr<rocksdb::DB> db;
#if ROCKSDB_MAJOR >= 10
    rocksdb::Status s = rocksdb::DB::Open(options, db_path, &db);
#else
    rocksdb::DB* db_raw = nullptr;
    rocksdb::Status s = rocksdb::DB::Open(options, db_path, &db_raw);
    db.reset(db_raw);
#endif
    if (!s.ok()) {
        fprintf(stderr, "Failed to open DB: %s\n", s.ToString().c_str());
        return 1;
    }

    // ── Fill ──
    fprintf(stderr, "Populating %d keys...\n", num_keys);
    auto t0 = std::chrono::steady_clock::now();

    rocksdb::WriteOptions write_opts;
    write_opts.disableWAL = true;  // Speed up writes

    for (int i = 0; i < num_keys; i++) {
        std::string key = make_key(i);
        std::string val = make_value(i, value_size);
        s = db->Put(write_opts, key, val);
        if (!s.ok()) {
            fprintf(stderr, "Put failed: %s\n", s.ToString().c_str());
            break;
        }
        if (i > 0 && i % 100000 == 0) {
            fprintf(stderr, "  %d/%d...\n", i, num_keys);
        }
    }

    // Force flush to SST files so data goes through block cache on reads
    db->Flush(rocksdb::FlushOptions());

    // Do a full scan to populate the block cache
    fprintf(stderr, "Warming block cache with full scan...\n");
    {
        rocksdb::ReadOptions read_opts;
        auto* it = db->NewIterator(read_opts);
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            // Touch each value to load blocks
            volatile size_t len = it->value().size();
            (void)len;
        }
        delete it;
    }

    auto t1 = std::chrono::steady_clock::now();
    double fill_sec = std::chrono::duration<double>(t1 - t0).count();
    double peak_rss = get_rss_mb();

    recordRss();

    fprintf(stderr, "Fill: %.2fs, RSS: %.1f MB\n\n", fill_sec, peak_rss);
    printf("METRIC compression %s\n", compression_name.c_str());
    printf("METRIC fill_sec %.2f\n", fill_sec);
    printf("METRIC peak_rss_mb %.1f\n", peak_rss);

    // ── Cool ──
    fprintf(stderr, "Cooling for %ds...\n", cool_duration);
    for (int sec = 1; sec <= cool_duration; sec++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        double rss = get_rss_mb();
        if (sec % 3 == 0 || sec == 1)
            fprintf(stderr, "  t=%ds: RSS=%.1f MB\n", sec, rss);
        recordRss();
    }
    double cool_rss = get_rss_mb();
    printf("METRIC cool_rss_mb %.1f\n", cool_rss);

    // ── Serve: hot reads on recent 5% of keys ──
    int hot_start = num_keys - num_keys / 20;
    int hot_range = num_keys / 20;
    fprintf(stderr, "\nServing hot reads (keys %d..%d) for %ds...\n",
            hot_start, num_keys - 1, serve_duration);

    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> hot_dist(hot_start, num_keys - 1);

    long total_ops = 0;
    double min_rss = get_rss_mb();
    rocksdb::ReadOptions read_opts;

    // Hot latency tracking (sample every 16th op)
    std::vector<double> hot_latencies;
    hot_latencies.reserve(500000);

    for (int sec = 0; sec < serve_duration; sec++) {
        auto sec_start = std::chrono::steady_clock::now();
        long ops = 0;
        while (true) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - sec_start).count() >= 1.0) break;

            // Batch of 10 reads
            for (int j = 0; j < 10; j++) {
                std::string key = make_key(hot_dist(rng));
                std::string val;
                auto op_start = std::chrono::steady_clock::now();
                db->Get(read_opts, key, &val);
                auto op_end = std::chrono::steady_clock::now();
                if ((ops & 0xF) == 0) {
                    hot_latencies.push_back(
                        std::chrono::duration<double, std::micro>(op_end - op_start).count());
                }
                ops++;
            }
        }
        total_ops += ops;
        double rss = get_rss_mb();
        if (rss < min_rss) min_rss = rss;
        if ((sec + 1) % 5 == 0)
            fprintf(stderr, "  t=%ds: RSS=%.1f MB ops=%ld\n", sec + 1, rss, ops);
        recordRss();
    }

    std::sort(hot_latencies.begin(), hot_latencies.end());
    double hot_p50 = hot_latencies.empty() ? 0 : hot_latencies[hot_latencies.size() / 2];
    double hot_p99 = hot_latencies.empty() ? 0 : hot_latencies[(size_t)(hot_latencies.size() * 0.99)];

    double serve_rss = get_rss_mb();
    long ops_per_sec = total_ops / serve_duration;
    printf("METRIC serve_rss_mb %.1f\n", serve_rss);
    printf("METRIC min_rss_mb %.1f\n", min_rss);
    printf("METRIC ops_per_sec %ld\n", ops_per_sec);
    printf("METRIC hot_p50_us %.2f\n", hot_p50);
    printf("METRIC hot_p99_us %.2f\n", hot_p99);

    // ── Cold re-access: scan oldest 10% of keys ──
    int cold_count = num_keys / 10;
    fprintf(stderr, "\nCold re-access: reading %d oldest keys...\n", cold_count);

    std::vector<double> cold_latencies;
    cold_latencies.reserve(cold_count);

    auto cold_start_time = std::chrono::steady_clock::now();
    for (int i = 0; i < cold_count; i++) {
        std::string key = make_key(i);
        std::string val;
        auto op_start = std::chrono::steady_clock::now();
        db->Get(read_opts, key, &val);
        auto op_end = std::chrono::steady_clock::now();
        cold_latencies.push_back(
            std::chrono::duration<double, std::micro>(op_end - op_start).count());
    }
    auto cold_end_time = std::chrono::steady_clock::now();
    double cold_sec = std::chrono::duration<double>(cold_end_time - cold_start_time).count();
    double cold_rss = get_rss_mb();

    std::sort(cold_latencies.begin(), cold_latencies.end());
    double cold_p50 = cold_latencies.empty() ? 0 : cold_latencies[cold_latencies.size() / 2];
    double cold_p99 = cold_latencies.empty() ? 0 : cold_latencies[(size_t)(cold_latencies.size() * 0.99)];

    fprintf(stderr, "Cold access: %.3fs, p50=%.1fus, p99=%.1fus, RSS: %.1f MB\n",
            cold_sec, cold_p50, cold_p99, cold_rss);
    printf("METRIC cold_sec %.3f\n", cold_sec);
    printf("METRIC cold_p50_us %.2f\n", cold_p50);
    printf("METRIC cold_p99_us %.2f\n", cold_p99);
    printf("METRIC cold_rss_mb %.1f\n", cold_rss);
    recordRss();

    if (const char* rss_dir = std::getenv("SMASH_RSS_DIR")) {
        dumpRssTimeline((std::string(rss_dir) + "/rocksdb_rss.csv").c_str());
    }

    // Dump CDF data if requested
    if (const char* lat_dir = std::getenv("SMASH_LATENCY_DIR")) {
        std::string path = std::string(lat_dir) + "/rocksdb_cold.csv";
        FILE* f = fopen(path.c_str(), "w");
        if (f) {
            fprintf(f, "latency_us\n");
            for (double v : cold_latencies) fprintf(f, "%.3f\n", v);
            fclose(f);
        }
        std::string hot_path = std::string(lat_dir) + "/rocksdb_hot.csv";
        f = fopen(hot_path.c_str(), "w");
        if (f) {
            fprintf(f, "latency_us\n");
            for (double v : hot_latencies) fprintf(f, "%.3f\n", v);
            fclose(f);
        }
    }

    fflush(stdout);
    db.reset();  // close the DB (was `delete db` before the unique_ptr port)
    return 0;
}
