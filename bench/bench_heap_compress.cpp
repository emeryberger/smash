// bench_heap_compress.cpp - Measure compression ratios on real heap pages
//
// Samples heap pages from the current process after filling memory with
// real application workloads, then compresses each page with LZ4 and
// multiple zstd levels to produce per-algorithm ratio statistics.
//
// Usage:
//   bench_heap_compress --app sqlite [--rows N]
//   bench_heap_compress --app rocksdb [--keys N] [--value-size N]
//
// Output: METRIC lines suitable for the paper's algorithm comparison table.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <lz4.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

static const size_t kPageSize =
#if defined(__aarch64__) && defined(__APPLE__)
    16384;  // ARM64 macOS
#else
    4096;   // Linux (x86_64 and aarch64)
#endif

// ── Page compression ──────────────────────────────────────────────────

struct PageStats {
    double ratio_lz4;
    double ratio_zstd1;
    double ratio_zstd3;
    double ratio_zstd9;
};

static PageStats compressPage(const void* page) {
    PageStats s{};
    char comp_buf[kPageSize * 2];

    int lz4_sz = LZ4_compress_default(
        static_cast<const char*>(page), comp_buf,
        static_cast<int>(kPageSize), static_cast<int>(sizeof(comp_buf)));
    s.ratio_lz4 = lz4_sz > 0 ? static_cast<double>(lz4_sz) / kPageSize : 1.0;

    size_t z1 = ZSTD_compress(comp_buf, sizeof(comp_buf), page, kPageSize, 1);
    s.ratio_zstd1 = !ZSTD_isError(z1) ? static_cast<double>(z1) / kPageSize : 1.0;

    size_t z3 = ZSTD_compress(comp_buf, sizeof(comp_buf), page, kPageSize, 3);
    s.ratio_zstd3 = !ZSTD_isError(z3) ? static_cast<double>(z3) / kPageSize : 1.0;

    size_t z9 = ZSTD_compress(comp_buf, sizeof(comp_buf), page, kPageSize, 9);
    s.ratio_zstd9 = !ZSTD_isError(z9) ? static_cast<double>(z9) / kPageSize : 1.0;

    return s;
}

// ── Heap page enumeration ─────────────────────────────────────────────

#ifdef __APPLE__
static std::vector<uintptr_t> enumerateHeapPages() {
    std::vector<uintptr_t> pages;
    mach_port_t task = mach_task_self();
    vm_address_t addr = 0;
    vm_size_t size = 0;
    natural_t depth = 1;

    while (true) {
        struct vm_region_submap_info_64 info;
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;

        kern_return_t kr = vm_region_recurse_64(
            task, &addr, &size, &depth,
            (vm_region_info_64_t)&info, &count);
        if (kr != KERN_SUCCESS)
            break;

        if (info.is_submap) {
            depth++;
            continue;
        }

        bool writable = (info.protection & VM_PROT_WRITE) != 0;
        bool readable = (info.protection & VM_PROT_READ) != 0;
        if (writable && readable && size >= kPageSize) {
            for (vm_address_t p = addr; p + kPageSize <= addr + size; p += kPageSize) {
                char vec[1];
                if (mincore(reinterpret_cast<void*>(p), kPageSize, vec) == 0 && (vec[0] & 1)) {
                    pages.push_back(p);
                }
            }
        }

        addr += size;
    }

    return pages;
}
#else // Linux
static std::vector<uintptr_t> enumerateHeapPages() {
    std::vector<uintptr_t> pages;

    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return pages;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        uintptr_t start = 0, end = 0;
        char perms[5] = {};
        if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3)
            continue;

        // Only read-write private regions (heap-like)
        if (perms[0] != 'r' || perms[1] != 'w' || perms[3] != 'p')
            continue;

        size_t region_size = end - start;
        if (region_size < kPageSize)
            continue;

        // Check residency in batches
        size_t npages = region_size / kPageSize;
        const size_t kBatch = 4096;
        for (size_t off = 0; off < npages; off += kBatch) {
            size_t count = std::min(kBatch, npages - off);
            unsigned char vec[kBatch];
            uintptr_t base = start + off * kPageSize;
            if (mincore(reinterpret_cast<void*>(base),
                        count * kPageSize, vec) == 0) {
                for (size_t i = 0; i < count; i++) {
                    if (vec[i] & 1)
                        pages.push_back(base + i * kPageSize);
                }
            }
        }
    }

    fclose(f);
    return pages;
}
#endif

// ── Workload: SQLite ──────────────────────────────────────────────────

#ifdef HAS_SQLITE
#include <sqlite3.h>

static void workloadSQLite(int rows) {
    sqlite3* db = nullptr;
    sqlite3_open(":memory:", &db);

    sqlite3_exec(db, "PRAGMA page_size=16384; PRAGMA journal_mode=OFF; "
                     "PRAGMA synchronous=OFF; PRAGMA cache_size=-262144;",
                 nullptr, nullptr, nullptr);

    sqlite3_exec(db, "CREATE TABLE records ("
                     "id INTEGER PRIMARY KEY, name TEXT, email TEXT, "
                     "bio TEXT, json_data TEXT, created_at TEXT)",
                 nullptr, nullptr, nullptr);

    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT INTO records VALUES (?, ?, ?, ?, ?, ?)", -1, &stmt, nullptr);

    const char* words[] = {"the ", "data ", "value ", "record ", "entry ",
                           "field ", "index ", "query ", "cache ", "store "};

    for (int i = 0; i < rows; i++) {
        sqlite3_bind_int(stmt, 1, i);

        char name[64];
        snprintf(name, sizeof(name), "user_%d", i);
        sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);

        char email[128];
        snprintf(email, sizeof(email), "user_%d@example.com", i);
        sqlite3_bind_text(stmt, 3, email, -1, SQLITE_TRANSIENT);

        // Bio: ~300 bytes of compressible text
        char bio[400];
        int off = 0;
        for (int j = 0; off < 300; j++)
            off += snprintf(bio + off, sizeof(bio) - off, "%s", words[j % 10]);
        sqlite3_bind_text(stmt, 4, bio, -1, SQLITE_TRANSIENT);

        char json[256];
        snprintf(json, sizeof(json),
            R"({"id":%d,"score":%d,"tags":["a","b","c"]})", i, i * 7 % 100);
        sqlite3_bind_text(stmt, 5, json, -1, SQLITE_TRANSIENT);

        sqlite3_bind_text(stmt, 6, "2024-01-15T10:30:00Z", -1, SQLITE_STATIC);

        sqlite3_step(stmt);
        sqlite3_reset(stmt);

        if (i % 100000 == 0 && i > 0)
            fprintf(stderr, "  SQLite: %d/%d rows\n", i, rows);
    }

    sqlite3_finalize(stmt);
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);

    // Force a full table scan to ensure pages are in memory
    sqlite3_exec(db, "SELECT count(*) FROM records", nullptr, nullptr, nullptr);

    fprintf(stderr, "  SQLite: %d rows loaded, sampling pages...\n", rows);

    // Don't close db — we need the pages in memory for sampling
    (void)db;
}
#endif

// ── Workload: RocksDB ─────────────────────────────────────────────────

#ifdef HAS_ROCKSDB
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
// ROCKSDB_MAJOR — included explicitly rather than relying on db.h pulling it
// in, so the version test below can never silently evaluate 0 >= 10 and pick
// the wrong DB::Open overload.
#include <rocksdb/version.h>
#include <memory>

static void workloadRocksDB(int keys, int value_size) {
    rocksdb::Options options;
    options.create_if_missing = true;
    options.compression = rocksdb::kNoCompression;

    rocksdb::BlockBasedTableOptions table_opts;
    table_opts.block_size = 16384;
    table_opts.block_cache = rocksdb::NewLRUCache(256 * 1024 * 1024);
    options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_opts));

    // RocksDB 10.0.1 replaced the `DB**` overload of DB::Open with
    // `std::unique_ptr<DB>*`.  Either way the DB is deliberately *not* closed
    // (see the end of this function — its pages must stay resident for the
    // sampling that follows), so on 10+ the unique_ptr is released rather than
    // left to close the DB at scope exit.
    rocksdb::DB* db = nullptr;
#if ROCKSDB_MAJOR >= 10
    std::unique_ptr<rocksdb::DB> db_owner;
    rocksdb::DB::Open(options, "/tmp/bench_heap_rocksdb", &db_owner);
    db = db_owner.release();
#else
    rocksdb::DB::Open(options, "/tmp/bench_heap_rocksdb", &db);
#endif

    // Fill
    for (int i = 0; i < keys; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key:%08d", i);

        std::string value(value_size, '\0');
        snprintf(value.data(), value.size(),
            R"({"id":%d,"name":"user_%d","score":%d,"data":")", i, i, i % 1000);
        size_t hdr = strlen(value.c_str());
        for (size_t j = hdr; j < value.size() - 2; j++)
            value[j] = 'a' + (j % 26);
        value[value.size() - 2] = '"';
        value[value.size() - 1] = '}';

        db->Put(rocksdb::WriteOptions(), key, value);

        if (i % 100000 == 0 && i > 0)
            fprintf(stderr, "  RocksDB: %d/%d keys\n", i, keys);
    }

    // Force compaction and cache fill
    db->CompactRange(rocksdb::CompactRangeOptions(), nullptr, nullptr);

    // Read all to populate block cache
    auto* iter = db->NewIterator(rocksdb::ReadOptions());
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {}
    delete iter;

    fprintf(stderr, "  RocksDB: %d keys loaded, sampling pages...\n", keys);

    // Don't close — need pages in memory
    (void)db;
}
#endif

// ── Workload: malloc-based (generic heap fill) ────────────────────────

struct HeapAlloc {
    void* ptr;
    size_t size;
};

// Fill heap with allocations of various sizes to create a realistic heap
static std::vector<HeapAlloc> workloadGenericHeap(
    const char* label, size_t total_bytes, size_t alloc_size,
    void (*fill_fn)(void*, size_t, int))
{
    std::vector<HeapAlloc> allocs;
    size_t allocated = 0;
    int idx = 0;

    while (allocated < total_bytes) {
        void* p = malloc(alloc_size);
        if (!p) break;
        fill_fn(p, alloc_size, idx);
        allocs.push_back({p, alloc_size});
        allocated += alloc_size;
        idx++;

        if (idx % 100000 == 0)
            fprintf(stderr, "  %s: %.1f / %.1f MB\n",
                    label, allocated / 1e6, total_bytes / 1e6);
    }

    fprintf(stderr, "  %s: %.1f MB in %zu allocs, sampling pages...\n",
            label, allocated / 1e6, allocs.size());
    return allocs;
}

// Memcached-like: slab items with key-value data
static void fillMemcachedLike(void* ptr, size_t size, int idx) {
    auto* p = static_cast<char*>(ptr);
    memset(p, 0, size);
    // Item header (like memcached's item struct)
    uint32_t flags = 0;
    uint32_t exptime = 1700000000 + idx;
    uint16_t nkey = 24;
    uint32_t nbytes = static_cast<uint32_t>(size - 48);
    memcpy(p, &flags, 4);
    memcpy(p + 4, &exptime, 4);
    memcpy(p + 8, &nkey, 2);
    memcpy(p + 10, &nbytes, 4);
    // Key
    snprintf(p + 48, nkey + 1, "mc:sess:%012d", idx);
    // Value: JSON-like session data
    int off = 48 + nkey;
    off += snprintf(p + off, size - off,
        R"({"user_id":%d,"session":"%08x","ts":%d,"data":{"cart":[)",
        idx, (unsigned)(idx * 2654435761u), 1700000000 + idx);
    for (int i = 0; i < 3 && off + 20 < (int)size; i++)
        off += snprintf(p + off, size - off, R"({"item":%d,"qty":%d},)", idx*10+i, i+1);
    if (off + 5 < (int)size)
        off += snprintf(p + off, size - off, "]}})");
}

// Redis-like: simple string values with key prefix
static void fillRedisLike(void* ptr, size_t size, int idx) {
    auto* p = static_cast<char*>(ptr);
    memset(p, 0, size);
    // Redis SDS-like header
    uint32_t len = static_cast<uint32_t>(size - 16);
    uint32_t alloc_sz = len;
    memcpy(p, &len, 4);
    memcpy(p + 4, &alloc_sz, 4);
    p[8] = 1;  // type byte
    // String content: key-value data
    int off = 16;
    off += snprintf(p + off, size - off,
        "data_entry_key:%07d_the_value_store_cache_record_field_"
        "index_query_data_entry_key:%07d_padding_to_make_this_"
        "about_two_hundred_bytes_of_compressible_content_end", idx, idx);
}

// DuckDB-like: columnar data (arrays of ints, strings, floats)
static void fillDuckDBLike(void* ptr, size_t size, int idx) {
    auto* p = static_cast<uint8_t*>(ptr);
    memset(p, 0, size);
    // Simulate a column chunk: array of int32 values
    auto* ints = reinterpret_cast<int32_t*>(p);
    size_t n_ints = size / 4;
    for (size_t i = 0; i < n_ints; i++) {
        // Columnar data: values from same column are similar
        ints[i] = static_cast<int32_t>(idx * 1000 + i);
    }
}

// ── Statistics ─────────────────────────────────────────────────────────

struct AlgoStats {
    double avg;
    double median;
};

static AlgoStats computeStats(std::vector<double>& v) {
    if (v.empty()) return {1.0, 1.0};
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    return {sum / v.size(), v[v.size() / 2]};
}

// ── Main ──────────────────────────────────────────────────────────────

static void usage() {
    fprintf(stderr,
        "Usage: bench_heap_compress --app <name> [options]\n"
        "  Apps: sqlite, rocksdb, memcached, redis, duckdb\n"
        "  --rows N       (sqlite, default 500000)\n"
        "  --keys N       (rocksdb, default 200000)\n"
        "  --value-size N (rocksdb, default 256)\n"
        "  --total-mb N   (memcached/redis/duckdb heap fill, default 200)\n"
        "  --alloc-size N (memcached/redis/duckdb object size, default 256)\n"
    );
}

int main(int argc, char* argv[]) {
    std::string app;
    int rows = 500000;
    int keys = 200000;
    int value_size = 256;
    int total_mb = 200;
    int alloc_size = 256;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--app") == 0 && i + 1 < argc)
            app = argv[++i];
        else if (strcmp(argv[i], "--rows") == 0 && i + 1 < argc)
            rows = atoi(argv[++i]);
        else if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc)
            keys = atoi(argv[++i]);
        else if (strcmp(argv[i], "--value-size") == 0 && i + 1 < argc)
            value_size = atoi(argv[++i]);
        else if (strcmp(argv[i], "--total-mb") == 0 && i + 1 < argc)
            total_mb = atoi(argv[++i]);
        else if (strcmp(argv[i], "--alloc-size") == 0 && i + 1 < argc)
            alloc_size = atoi(argv[++i]);
        else { usage(); return 1; }
    }

    if (app.empty()) { usage(); return 1; }

    std::vector<HeapAlloc> generic_allocs;

    // ── Run workload ──
    fprintf(stderr, "Running %s workload...\n", app.c_str());

    if (app == "sqlite") {
#ifdef HAS_SQLITE
        workloadSQLite(rows);
#else
        fprintf(stderr, "ERROR: built without SQLite support\n");
        return 1;
#endif
    } else if (app == "rocksdb") {
#ifdef HAS_ROCKSDB
        workloadRocksDB(keys, value_size);
#else
        fprintf(stderr, "ERROR: built without RocksDB support\n");
        return 1;
#endif
    } else if (app == "memcached") {
        generic_allocs = workloadGenericHeap(
            "memcached", total_mb * 1024ULL * 1024, alloc_size, fillMemcachedLike);
    } else if (app == "redis") {
        generic_allocs = workloadGenericHeap(
            "redis", total_mb * 1024ULL * 1024, alloc_size, fillRedisLike);
    } else if (app == "duckdb") {
        generic_allocs = workloadGenericHeap(
            "duckdb", total_mb * 1024ULL * 1024, alloc_size, fillDuckDBLike);
    } else {
        fprintf(stderr, "Unknown app: %s\n", app.c_str());
        return 1;
    }

    // ── Enumerate and compress heap pages ──
    auto pages = enumerateHeapPages();

    fprintf(stderr, "Found %zu resident heap pages\n", pages.size());

    // Sample up to 10000 pages for speed
    if (pages.size() > 10000) {
        // Deterministic shuffle
        std::mt19937 rng{42};
        std::shuffle(pages.begin(), pages.end(), rng);
        pages.resize(10000);
    }

    std::vector<double> r_lz4, r_zstd1, r_zstd3, r_zstd9;

    for (uintptr_t addr : pages) {
        // Skip pages that are all-zero (unmapped/decommitted)
        const auto* p = reinterpret_cast<const uint64_t*>(addr);
        bool all_zero = true;
        for (size_t i = 0; i < kPageSize / 8; i++) {
            if (p[i] != 0) { all_zero = false; break; }
        }
        if (all_zero) continue;

        PageStats s = compressPage(reinterpret_cast<const void*>(addr));
        r_lz4.push_back(s.ratio_lz4);
        r_zstd1.push_back(s.ratio_zstd1);
        r_zstd3.push_back(s.ratio_zstd3);
        r_zstd9.push_back(s.ratio_zstd9);
    }

    if (r_lz4.empty()) {
        fprintf(stderr, "No non-zero pages found\n");
        return 1;
    }

    auto s_lz4 = computeStats(r_lz4);
    auto s_zstd1 = computeStats(r_zstd1);
    auto s_zstd3 = computeStats(r_zstd3);
    auto s_zstd9 = computeStats(r_zstd9);

    // Emit results
    printf("METRIC app %s\n", app.c_str());
    printf("METRIC pages_sampled %zu\n", r_lz4.size());
    printf("METRIC avg_lz4 %.1f\n", s_lz4.avg * 100);
    printf("METRIC avg_zstd1 %.1f\n", s_zstd1.avg * 100);
    printf("METRIC avg_zstd3 %.1f\n", s_zstd3.avg * 100);
    printf("METRIC avg_zstd9 %.1f\n", s_zstd9.avg * 100);
    printf("METRIC median_lz4 %.1f\n", s_lz4.median * 100);
    printf("METRIC median_zstd1 %.1f\n", s_zstd1.median * 100);
    printf("METRIC median_zstd3 %.1f\n", s_zstd3.median * 100);
    printf("METRIC median_zstd9 %.1f\n", s_zstd9.median * 100);

    // Also print a summary table
    fprintf(stderr, "\n%-10s %8s %8s %8s %8s\n", "Metric", "LZ4", "zstd-1", "zstd-3", "zstd-9");
    fprintf(stderr, "%-10s %7.1f%% %7.1f%% %7.1f%% %7.1f%%\n", "Average",
            s_lz4.avg*100, s_zstd1.avg*100, s_zstd3.avg*100, s_zstd9.avg*100);
    fprintf(stderr, "%-10s %7.1f%% %7.1f%% %7.1f%% %7.1f%%\n", "Median",
            s_lz4.median*100, s_zstd1.median*100, s_zstd3.median*100, s_zstd9.median*100);

    // Free generic allocs
    for (auto& a : generic_allocs) free(a.ptr);

    return 0;
}
