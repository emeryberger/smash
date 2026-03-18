// bench_allocator_compare.cpp - Compare page compressibility across allocator substrates
//
// Single-source benchmark compiled multiple times, each linked against a different
// allocator. Measures how allocator design (metadata placement, zero-on-free, arena
// segregation) affects page compressibility using real application workloads.
//
// Usage: bench_alloc_<name> --app=sqlite|memcached|redis|duckdb --alloc-size=N --count=N --free-pct=N

#include <lz4.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/mman.h>
#else
#include <sys/mman.h>
#endif

#ifndef ALLOC_NAME
#define ALLOC_NAME "unknown"
#endif

static constexpr size_t kPageSize =
#if defined(__aarch64__) && defined(__APPLE__)
    16384;
#else
    4096;
#endif

static std::mt19937 g_rng(42);

// ── Application-realistic fill functions ─────────────────────────────

// Memcached-like: slab items with header, key, and JSON session values
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

// Redis-like: SDS string with key-value content
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

// DuckDB-like: columnar data (arrays of int32 values from same column)
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

// Separate functions for mixed allocation to create distinct return addresses
// for arena routing (Smash routes same call site to same arena)
__attribute__((noinline)) void* alloc_memcached(size_t sz) { return malloc(sz); }
__attribute__((noinline)) void* alloc_redis(size_t sz) { return malloc(sz); }
__attribute__((noinline)) void* alloc_duckdb(size_t sz) { return malloc(sz); }

// ── Heap page enumeration (for SQLite mode) ──────────────────────────

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
            size_t bcount = std::min(kBatch, npages - off);
            unsigned char vec[kBatch];
            uintptr_t base = start + off * kPageSize;
            if (mincore(reinterpret_cast<void*>(base),
                        bcount * kPageSize, vec) == 0) {
                for (size_t i = 0; i < bcount; i++) {
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

// ── SQLite workload ──────────────────────────────────────────────────

#ifdef HAS_SQLITE
#include <sqlite3.h>

static sqlite3* g_sqlite_db = nullptr;

static void workloadSQLite(int rows) {
    sqlite3_open(":memory:", &g_sqlite_db);

    sqlite3_exec(g_sqlite_db,
        "PRAGMA page_size=16384; PRAGMA journal_mode=OFF; "
        "PRAGMA synchronous=OFF; PRAGMA cache_size=-262144;",
        nullptr, nullptr, nullptr);

    sqlite3_exec(g_sqlite_db,
        "CREATE TABLE records ("
        "id INTEGER PRIMARY KEY, name TEXT, email TEXT, "
        "bio TEXT, json_data TEXT, created_at TEXT)",
        nullptr, nullptr, nullptr);

    sqlite3_exec(g_sqlite_db, "BEGIN", nullptr, nullptr, nullptr);

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(g_sqlite_db,
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
    sqlite3_exec(g_sqlite_db, "COMMIT", nullptr, nullptr, nullptr);

    // Force a full table scan to ensure pages are in memory
    sqlite3_exec(g_sqlite_db, "SELECT count(*) FROM records", nullptr, nullptr, nullptr);

    fprintf(stderr, "  SQLite: %d rows loaded, sampling pages...\n", rows);

    // Don't close db -- we need the pages in memory for sampling
}
#endif

// ── Page compression measurement ─────────────────────────────────────

struct PageStats {
    double ratio_lz4;
    double ratio_zstd;
};

static PageStats compressPage(const void* page) {
    PageStats s{};
    char comp_buf[kPageSize * 2];

    int lz4_size = LZ4_compress_default(
        static_cast<const char*>(page), comp_buf,
        static_cast<int>(kPageSize), static_cast<int>(sizeof(comp_buf)));
    s.ratio_lz4 = lz4_size > 0 ? static_cast<double>(lz4_size) / kPageSize : 1.0;

    size_t zstd_size = ZSTD_compress(comp_buf, sizeof(comp_buf), page, kPageSize, 9);
    s.ratio_zstd = !ZSTD_isError(zstd_size) ? static_cast<double>(zstd_size) / kPageSize : 1.0;

    return s;
}

// ── Argument parsing ─────────────────────────────────────────────────

struct Args {
    std::string app = "memcached";
    int alloc_size = 256;
    int count = 100000;
    int free_pct = 50;
};

static Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.rfind("--app=", 0) == 0) a.app = arg.substr(6);
        else if (arg.rfind("--alloc-size=", 0) == 0) a.alloc_size = std::atoi(arg.c_str() + 13);
        else if (arg.rfind("--size=", 0) == 0) a.alloc_size = std::atoi(arg.c_str() + 7);
        else if (arg.rfind("--count=", 0) == 0) a.count = std::atoi(arg.c_str() + 8);
        else if (arg.rfind("--free-pct=", 0) == 0) a.free_pct = std::atoi(arg.c_str() + 11);
    }
    return a;
}

// ── Main ─────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    Args args = parseArgs(argc, argv);

    // ── SQLite mode: use real SQLite in-memory DB, enumerate all heap pages ──
    if (args.app == "sqlite") {
#ifdef HAS_SQLITE
        // Snapshot pages before SQLite workload
        auto pages_before = enumerateHeapPages();
        std::set<uintptr_t> pre_set(pages_before.begin(), pages_before.end());

        workloadSQLite(args.count);

        // Enumerate all heap pages after loading; new pages are SQLite's
        auto pages_after = enumerateHeapPages();
        std::set<uintptr_t> live_pages;
        for (uintptr_t p : pages_after) {
            if (pre_set.find(p) == pre_set.end())
                live_pages.insert(p);
        }

        // Check residency
        int resident_pages = 0;
        for (uintptr_t page : live_pages) {
#ifdef __APPLE__
            char vec[1];
            if (mincore(reinterpret_cast<void*>(page), kPageSize, vec) == 0 && (vec[0] & 1))
                resident_pages++;
#else
            unsigned char vec[1];
            if (mincore(reinterpret_cast<void*>(page), kPageSize, vec) == 0 && (vec[0] & 1))
                resident_pages++;
#endif
        }

        // Compress each page
        std::vector<double> ratios_lz4, ratios_zstd;
        ratios_lz4.reserve(live_pages.size());
        ratios_zstd.reserve(live_pages.size());

        for (uintptr_t page : live_pages) {
#ifdef __APPLE__
            char vec[1];
            if (mincore(reinterpret_cast<void*>(page), kPageSize, vec) != 0) continue;
            if (!(vec[0] & 1)) continue;
#else
            unsigned char vec[1];
            if (mincore(reinterpret_cast<void*>(page), kPageSize, vec) != 0) continue;
            if (!(vec[0] & 1)) continue;
#endif
            PageStats s = compressPage(reinterpret_cast<const void*>(page));
            ratios_lz4.push_back(s.ratio_lz4);
            ratios_zstd.push_back(s.ratio_zstd);
        }

        if (ratios_lz4.empty()) {
            fprintf(stderr, "No compressible pages found\n");
            return 1;
        }

        auto computeStats = [](std::vector<double>& v) -> std::tuple<double, double, double> {
            std::sort(v.begin(), v.end());
            double sum = 0;
            for (double x : v) sum += x;
            double avg = sum / v.size();
            double median = v[v.size() / 2];
            int compressible = 0;
            for (double x : v) if (x < 0.75) compressible++;
            double pct = 100.0 * compressible / v.size();
            return {avg, median, pct};
        };

        auto [avg_lz4, med_lz4, pct_lz4] = computeStats(ratios_lz4);
        auto [avg_zstd, med_zstd, pct_zstd] = computeStats(ratios_zstd);

        printf("METRIC allocator %s\n", ALLOC_NAME);
        printf("METRIC app %s\n", args.app.c_str());
        printf("METRIC obj_count %d\n", args.count);
        printf("METRIC free_pct 0\n");
        printf("METRIC total_pages %zu\n", live_pages.size());
        printf("METRIC resident_pages %d\n", resident_pages);
        printf("METRIC avg_ratio_lz4 %.4f\n", avg_lz4);
        printf("METRIC avg_ratio_zstd %.4f\n", avg_zstd);
        printf("METRIC median_ratio_lz4 %.4f\n", med_lz4);
        printf("METRIC median_ratio_zstd %.4f\n", med_zstd);
        printf("METRIC pct_compressible_lz4 %.1f\n", pct_lz4);
        printf("METRIC pct_compressible_zstd %.1f\n", pct_zstd);

        sqlite3_close(g_sqlite_db);
        return 0;
#else
        fprintf(stderr, "SQLite support not compiled (need -DHAS_SQLITE and link -lsqlite3)\n");
        return 1;
#endif
    }

    // ── Non-SQLite modes: allocate objects, fill, free fraction, measure ──

    // Select fill function and alloc call site based on app
    using FillFn = void (*)(void*, size_t, int);
    FillFn fill_fn = nullptr;

    if (args.app == "memcached") {
        fill_fn = fillMemcachedLike;
    } else if (args.app == "redis") {
        fill_fn = fillRedisLike;
    } else if (args.app == "duckdb") {
        fill_fn = fillDuckDBLike;
    } else {
        fprintf(stderr, "Unknown app: %s (use sqlite, memcached, redis, or duckdb)\n",
                args.app.c_str());
        return 1;
    }

    // Allocate objects using separate call sites for arena routing
    std::vector<void*> ptrs(args.count, nullptr);
    for (int i = 0; i < args.count; i++) {
        if (args.app == "memcached") {
            ptrs[i] = alloc_memcached(args.alloc_size);
        } else if (args.app == "redis") {
            ptrs[i] = alloc_redis(args.alloc_size);
        } else if (args.app == "duckdb") {
            ptrs[i] = alloc_duckdb(args.alloc_size);
        }
        if (!ptrs[i]) {
            fprintf(stderr, "malloc failed at i=%d\n", i);
            return 1;
        }
    }

    // Fill objects with application-realistic data
    for (int i = 0; i < args.count; i++) {
        fill_fn(ptrs[i], args.alloc_size, i);
    }

    // Record which pages contain our objects
    std::set<uintptr_t> pages_before;
    for (int i = 0; i < args.count; i++) {
        uintptr_t page = reinterpret_cast<uintptr_t>(ptrs[i]) & ~(kPageSize - 1);
        pages_before.insert(page);
    }

    // Free a fraction (every-other pattern to keep pages alive with holes)
    int freed = 0;
    for (int i = 0; i < args.count; i++) {
        if ((i % (100 / std::max(1, args.free_pct))) == 0) {
#ifdef ZERO_ON_FREE
            // Zero the object before freeing to isolate the effect of
            // zero-on-free independently of the allocator's own behavior
            memset(ptrs[i], 0, args.alloc_size);
#endif
            free(ptrs[i]);
            ptrs[i] = nullptr;
            freed++;
        }
    }

    // Collect pages that still have live objects
    std::set<uintptr_t> live_pages;
    for (int i = 0; i < args.count; i++) {
        if (ptrs[i]) {
            uintptr_t page = reinterpret_cast<uintptr_t>(ptrs[i]) & ~(kPageSize - 1);
            live_pages.insert(page);
        }
    }

    // Check pages are resident via mincore
    int resident_pages = 0;
    for (uintptr_t page : live_pages) {
#ifdef __APPLE__
        char vec[1];
        if (mincore(reinterpret_cast<void*>(page), kPageSize, vec) == 0 && (vec[0] & 1))
            resident_pages++;
#else
        unsigned char vec[1];
        if (mincore(reinterpret_cast<void*>(page), kPageSize, vec) == 0 && (vec[0] & 1))
            resident_pages++;
#endif
    }

    // Compress each live page
    std::vector<double> ratios_lz4, ratios_zstd;
    ratios_lz4.reserve(live_pages.size());
    ratios_zstd.reserve(live_pages.size());

    for (uintptr_t page : live_pages) {
#ifdef __APPLE__
        char vec[1];
        if (mincore(reinterpret_cast<void*>(page), kPageSize, vec) != 0) continue;
        if (!(vec[0] & 1)) continue;
#else
        unsigned char vec[1];
        if (mincore(reinterpret_cast<void*>(page), kPageSize, vec) != 0) continue;
        if (!(vec[0] & 1)) continue;
#endif
        PageStats s = compressPage(reinterpret_cast<const void*>(page));
        ratios_lz4.push_back(s.ratio_lz4);
        ratios_zstd.push_back(s.ratio_zstd);
    }

    if (ratios_lz4.empty()) {
        fprintf(stderr, "No compressible pages found\n");
        return 1;
    }

    // Compute statistics
    auto computeStats = [](std::vector<double>& v) -> std::tuple<double, double, double> {
        std::sort(v.begin(), v.end());
        double sum = 0;
        for (double x : v) sum += x;
        double avg = sum / v.size();
        double median = v[v.size() / 2];
        int compressible = 0;
        for (double x : v) if (x < 0.75) compressible++;
        double pct = 100.0 * compressible / v.size();
        return {avg, median, pct};
    };

    auto [avg_lz4, med_lz4, pct_lz4] = computeStats(ratios_lz4);
    auto [avg_zstd, med_zstd, pct_zstd] = computeStats(ratios_zstd);

    // Emit METRIC lines
    printf("METRIC allocator %s\n", ALLOC_NAME);
    printf("METRIC app %s\n", args.app.c_str());
    printf("METRIC alloc_size %d\n", args.alloc_size);
    printf("METRIC obj_count %d\n", args.count);
    printf("METRIC free_pct %d\n", args.free_pct);
    printf("METRIC total_pages %zu\n", live_pages.size());
    printf("METRIC resident_pages %d\n", resident_pages);
    printf("METRIC avg_ratio_lz4 %.4f\n", avg_lz4);
    printf("METRIC avg_ratio_zstd %.4f\n", avg_zstd);
    printf("METRIC median_ratio_lz4 %.4f\n", med_lz4);
    printf("METRIC median_ratio_zstd %.4f\n", med_zstd);
    printf("METRIC pct_compressible_lz4 %.1f\n", pct_lz4);
    printf("METRIC pct_compressible_zstd %.1f\n", pct_zstd);

    // Clean up remaining allocations
    for (int i = 0; i < args.count; i++) {
        if (ptrs[i]) free(ptrs[i]);
    }

    return 0;
}
