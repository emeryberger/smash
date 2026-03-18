// bench_algo_compare.cpp - Compare compression algorithms on real page data
//
// Compares: WKdm (macOS), LZ4 (Linux default), LZ4-HC, zstd-1, zstd-3, zstd-9
// Also tests multi-page compression (2-page and 4-page groups).
//
// Data sources: JSON-like, KV-like, SQLite-like, mostly-zeroed, random pages.
// Measures compression ratio and throughput (compress + decompress).

#include "smash/config.h"
#include "core/bootstrap_alloc.h"
#include "compress/compress_engine.h"
#include "compress/wkdm.h"

#include <lz4.h>
#include <lz4hc.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <algorithm>

using namespace smash;

// ── Data generators (same as bench_dict_experiment) ─────────────────────

static std::mt19937 rng(42);

static void fillJsonLike(void* page, int variant) {
    auto* p = static_cast<char*>(page);
    int off = 0;
    const char* keys[] = {"id", "name", "email", "address", "phone",
                          "status", "balance", "created_at", "updated_at", "tags"};
    while (off + 200 < static_cast<int>(kPageSize)) {
        off += snprintf(p + off, kPageSize - off,
            R"({"%.4s":%d,"%.5s":"user_%d@example.com","%.7s":"%.3s Street %d","%.5s":"+1-%03d-%04d","%.6s":"active","%.7s":%d.%02d})""\n",
            keys[0], variant * 10000 + off, keys[2], variant * 100 + off,
            keys[3], keys[1], off % 999, keys[4], (variant + off) % 999,
            (off * 7 + variant) % 9999, keys[5], keys[6],
            (off * 13 + variant) % 99999, (off * 3) % 100);
    }
    if (off < static_cast<int>(kPageSize))
        memset(p + off, 0, kPageSize - off);
}

static void fillKVLike(void* page, int variant) {
    auto* p = static_cast<uint8_t*>(page);
    memset(p, 0, kPageSize);
    size_t off = 0;
    int entry = 0;
    while (off + 64 < kPageSize) {
        uint32_t key_hash = static_cast<uint32_t>((variant * 1000 + entry) * 2654435761u);
        uint16_t val_len = 20 + (key_hash % 40);
        // Header: 4-byte hash, 2-byte key_len, 2-byte val_len, 4-byte flags, 4-byte expire
        memcpy(p + off, &key_hash, 4);
        uint16_t kl = 16;
        memcpy(p + off + 4, &kl, 2);
        memcpy(p + off + 6, &val_len, 2);
        uint32_t flags = 0;
        memcpy(p + off + 8, &flags, 4);
        uint32_t expire = static_cast<uint32_t>(1700000000 + variant * 100 + entry);
        memcpy(p + off + 12, &expire, 4);
        // Key
        snprintf(reinterpret_cast<char*>(p + off + 16), 17, "key:%07d:%04d",
                 variant, entry);
        // Value
        for (size_t i = 0; i < val_len; i++)
            p[off + 32 + i] = static_cast<uint8_t>((key_hash + i * 7) & 0xFF);
        off += 32 + val_len;
        // Align to 8 bytes
        off = (off + 7) & ~7ULL;
        entry++;
    }
}

static void fillSQLiteLike(void* page, int variant) {
    auto* p = static_cast<uint8_t*>(page);
    memset(p, 0, kPageSize);
    // B-tree page header (8 bytes)
    p[0] = 0x0D;  // leaf table page
    uint16_t ncells = 30 + (variant % 20);
    p[3] = static_cast<uint8_t>(ncells >> 8);
    p[4] = static_cast<uint8_t>(ncells & 0xFF);

    size_t off = 100;  // cell content area
    for (int i = 0; i < ncells && off + 100 < kPageSize; i++) {
        // Cell header: rowid (varint) + payload size (varint)
        uint64_t rowid = static_cast<uint64_t>(variant * 1000 + i);
        p[off++] = static_cast<uint8_t>((rowid >> 8) & 0x7F) | 0x80;
        p[off++] = static_cast<uint8_t>(rowid & 0x7F);
        // Type codes
        p[off++] = 0x05;  // integer
        p[off++] = 0x17;  // text
        p[off++] = 0x07;  // float
        // Payload
        uint32_t val = static_cast<uint32_t>(variant * 100 + i);
        memcpy(p + off, &val, 4); off += 4;
        int tlen = snprintf(reinterpret_cast<char*>(p + off), 60,
                            "row_%d_col_%d_data_%d", variant, i, val);
        off += tlen + 1;
        double fval = variant * 1.5 + i * 0.01;
        memcpy(p + off, &fval, 8); off += 8;
    }
}

static void fillZeroed(void* page) {
    memset(page, 0, kPageSize);
    // Sprinkle a few non-zero words (like partially freed slab page)
    auto* words = static_cast<uint32_t*>(page);
    for (int i = 0; i < 20; i++) {
        words[rng() % (kPageSize / 4)] = rng();
    }
}

static void fillRandom(void* page) {
    auto* words = static_cast<uint32_t*>(page);
    for (size_t i = 0; i < kPageSize / 4; i++)
        words[i] = rng();
}

// ── Algorithm wrappers ──────────────────────────────────────────────────

struct AlgoResult {
    size_t comp_size;
    double comp_ns;
    double decomp_ns;
    bool verified;
};

using CompFunc = AlgoResult(*)(const void*, size_t, void*, size_t, void*, void*);

static AlgoResult benchWKdm(const void* src, size_t src_size,
                            void* dst, size_t dst_cap,
                            void* scratch, void* verify_buf) {
    AlgoResult r{};
    auto t0 = std::chrono::high_resolution_clock::now();
    r.comp_size = WKdm::compress(src, dst, src_size, dst_cap, scratch);
    auto t1 = std::chrono::high_resolution_clock::now();
    r.comp_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();

    if (r.comp_size > 0) {
        auto t2 = std::chrono::high_resolution_clock::now();
        size_t ds = WKdm::decompress(dst, verify_buf, r.comp_size, src_size);
        auto t3 = std::chrono::high_resolution_clock::now();
        r.decomp_ns = std::chrono::duration<double, std::nano>(t3 - t2).count();
        r.verified = (ds == src_size && memcmp(src, verify_buf, src_size) == 0);
    }
    return r;
}

static AlgoResult benchLZ4(const void* src, size_t src_size,
                           void* dst, size_t dst_cap,
                           void* scratch, void* verify_buf) {
    AlgoResult r{};
    auto t0 = std::chrono::high_resolution_clock::now();
    int cs = LZ4_compress_default(static_cast<const char*>(src),
                                  static_cast<char*>(dst),
                                  static_cast<int>(src_size),
                                  static_cast<int>(dst_cap));
    auto t1 = std::chrono::high_resolution_clock::now();
    r.comp_size = cs > 0 ? static_cast<size_t>(cs) : 0;
    r.comp_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();

    if (r.comp_size > 0) {
        auto t2 = std::chrono::high_resolution_clock::now();
        int ds = LZ4_decompress_safe(static_cast<const char*>(dst),
                                     static_cast<char*>(verify_buf),
                                     cs, static_cast<int>(src_size));
        auto t3 = std::chrono::high_resolution_clock::now();
        r.decomp_ns = std::chrono::duration<double, std::nano>(t3 - t2).count();
        r.verified = (ds == static_cast<int>(src_size) &&
                      memcmp(src, verify_buf, src_size) == 0);
    }
    return r;
}

static AlgoResult benchLZ4HC(const void* src, size_t src_size,
                             void* dst, size_t dst_cap,
                             void* scratch, void* verify_buf) {
    AlgoResult r{};
    auto t0 = std::chrono::high_resolution_clock::now();
    int cs = LZ4_compress_HC(static_cast<const char*>(src),
                             static_cast<char*>(dst),
                             static_cast<int>(src_size),
                             static_cast<int>(dst_cap),
                             LZ4HC_CLEVEL_DEFAULT);
    auto t1 = std::chrono::high_resolution_clock::now();
    r.comp_size = cs > 0 ? static_cast<size_t>(cs) : 0;
    r.comp_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();

    if (r.comp_size > 0) {
        auto t2 = std::chrono::high_resolution_clock::now();
        int ds = LZ4_decompress_safe(static_cast<const char*>(dst),
                                     static_cast<char*>(verify_buf),
                                     cs, static_cast<int>(src_size));
        auto t3 = std::chrono::high_resolution_clock::now();
        r.decomp_ns = std::chrono::duration<double, std::nano>(t3 - t2).count();
        r.verified = (ds == static_cast<int>(src_size) &&
                      memcmp(src, verify_buf, src_size) == 0);
    }
    return r;
}

template<int Level>
static AlgoResult benchZstd(const void* src, size_t src_size,
                            void* dst, size_t dst_cap,
                            void* scratch, void* verify_buf) {
    AlgoResult r{};
    auto t0 = std::chrono::high_resolution_clock::now();
    size_t cs = ZSTD_compress(dst, dst_cap, src, src_size, Level);
    auto t1 = std::chrono::high_resolution_clock::now();
    r.comp_size = ZSTD_isError(cs) ? 0 : cs;
    r.comp_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();

    if (r.comp_size > 0) {
        auto t2 = std::chrono::high_resolution_clock::now();
        size_t ds = ZSTD_decompress(verify_buf, src_size, dst, cs);
        auto t3 = std::chrono::high_resolution_clock::now();
        r.decomp_ns = std::chrono::duration<double, std::nano>(t3 - t2).count();
        r.verified = (!ZSTD_isError(ds) && ds == src_size &&
                      memcmp(src, verify_buf, src_size) == 0);
    }
    return r;
}

// ── Main ────────────────────────────────────────────────────────────────

struct DataType {
    const char* name;
    void (*fill)(void* page, int variant);
};

int main() {
    constexpr int kPages = 100;
    constexpr int kMaxGroupSize = 4;
    constexpr int kIter = 5; // iterations for timing

    printf("Page size: %zu bytes\n\n", kPageSize);

    // Allocate pages
    auto* pages = static_cast<char*>(aligned_alloc(kPageSize, kPages * kPageSize));
    size_t max_comp = kPageSize * kMaxGroupSize * 2; // generous
    auto* comp_buf = static_cast<char*>(malloc(max_comp));
    auto* verify_buf = static_cast<char*>(aligned_alloc(kPageSize, kPages * kPageSize));
    auto* scratch = static_cast<char*>(aligned_alloc(kPageSize, kPageSize * kMaxGroupSize));

    struct AlgoDesc {
        const char* name;
        const char* os;     // which OS uses this
        CompFunc func;
    };

    AlgoDesc algos[] = {
        {"WKdm",    "macOS VM",         benchWKdm},
        {"LZ4",     "Linux zswap",      benchLZ4},
        {"LZ4-HC",  "(high compress)",   benchLZ4HC},
        {"zstd-1",  "Linux zswap opt",  benchZstd<1>},
        {"zstd-3",  "Smash cold",       benchZstd<3>},
        {"zstd-9",  "Smash very-cold",  benchZstd<9>},
    };
    int nalgo = sizeof(algos) / sizeof(algos[0]);

    auto fillJsonWrap = [](void* p, int v) { fillJsonLike(p, v); };
    auto fillKVWrap = [](void* p, int v) { fillKVLike(p, v); };
    auto fillSQLWrap = [](void* p, int v) { fillSQLiteLike(p, v); };
    auto fillZeroWrap = [](void* p, int v) { (void)v; fillZeroed(p); };
    auto fillRandWrap = [](void* p, int v) { (void)v; fillRandom(p); };

    struct DataDesc {
        const char* name;
        void (*fill)(void*, int);
    } data_types[] = {
        {"json",    fillJsonWrap},
        {"kv",      fillKVWrap},
        {"sqlite",  fillSQLWrap},
        {"zeroed",  fillZeroWrap},
        {"random",  fillRandWrap},
    };
    int ndata = sizeof(data_types) / sizeof(data_types[0]);

    // ── Part 1: Single-page comparison ──────────────────────────────────

    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("  SINGLE PAGE COMPRESSION (%zu bytes/page, %d pages averaged)\n", kPageSize, kPages);
    printf("═══════════════════════════════════════════════════════════════════\n\n");

    printf("%-8s %-10s %8s %8s %10s %10s %5s\n",
           "Data", "Algorithm", "CompSize", "Ratio", "Comp MB/s", "Dec MB/s", "OK");
    printf("──────── ────────── ──────── ──────── ────────── ────────── ─────\n");

    for (int d = 0; d < ndata; d++) {
        // Fill pages
        for (int i = 0; i < kPages; i++)
            data_types[d].fill(pages + i * kPageSize, i);

        for (int a = 0; a < nalgo; a++) {
            size_t total_comp = 0;
            double total_comp_ns = 0, total_decomp_ns = 0;
            int ok_count = 0;
            int comp_count = 0;
            int verify_attempts = 0;

            for (int iter = 0; iter < kIter; iter++) {
                for (int i = 0; i < kPages; i++) {
                    AlgoResult r = algos[a].func(
                        pages + i * kPageSize, kPageSize,
                        comp_buf, max_comp, scratch, verify_buf);
                    if (iter == 0 && r.comp_size > 0) {
                        total_comp += r.comp_size;
                        comp_count++;
                    }
                    total_comp_ns += r.comp_ns;
                    total_decomp_ns += r.decomp_ns;
                    if (r.comp_size > 0) {
                        verify_attempts++;
                        if (r.verified) ok_count++;
                    }
                }
            }

            double avg_comp = comp_count > 0
                ? static_cast<double>(total_comp) / comp_count : 0;
            double ratio = avg_comp > 0 ? avg_comp / kPageSize : 1.0;
            double comp_mbs = (kPageSize * kPages * kIter) /
                              (total_comp_ns / 1e9) / (1024 * 1024);
            double decomp_mbs = (total_decomp_ns > 0)
                ? (kPageSize * kPages * kIter) /
                  (total_decomp_ns / 1e9) / (1024 * 1024)
                : 0;
            bool all_ok = (verify_attempts == 0) ||
                          (ok_count == verify_attempts);

            printf("%-8s %-10s %8.0f %7.1f%% %9.0f %9.0f %5s\n",
                   a == 0 ? data_types[d].name : "",
                   algos[a].name, avg_comp, ratio * 100,
                   comp_mbs, decomp_mbs,
                   all_ok ? "yes" : "FAIL");
        }
        printf("\n");
    }

    // ── Part 2: Multi-page compression ──────────────────────────────────

    printf("\n═══════════════════════════════════════════════════════════════════\n");
    printf("  MULTI-PAGE COMPRESSION (group size = 1, 2, 4 pages)\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");

    printf("%-8s %-10s %6s %8s %8s %10s %10s\n",
           "Data", "Algorithm", "Group", "CompSize", "Ratio", "Comp MB/s", "Dec MB/s");
    printf("──────── ────────── ────── ──────── ──────── ────────── ──────────\n");

    int group_sizes[] = {1, 2, 4};
    // Only test LZ4 and zstd-3 for multi-page (WKdm is designed for single pages)
    int multi_algos[] = {1, 4, 5}; // LZ4, zstd-3, zstd-9

    for (int d = 0; d < ndata - 1; d++) { // skip random
        for (int i = 0; i < kPages; i++)
            data_types[d].fill(pages + i * kPageSize, i);

        for (int gi = 0; gi < 3; gi++) {
            int gs = group_sizes[gi];
            size_t group_bytes = gs * kPageSize;
            int ngroups = kPages / gs;

            for (int mi = 0; mi < 3; mi++) {
                int a = multi_algos[mi];
                size_t total_comp = 0;
                double total_comp_ns = 0, total_decomp_ns = 0;
                int comp_count = 0;

                for (int iter = 0; iter < kIter; iter++) {
                    for (int g = 0; g < ngroups; g++) {
                        AlgoResult r = algos[a].func(
                            pages + g * group_bytes, group_bytes,
                            comp_buf, max_comp, scratch, verify_buf);
                        if (iter == 0 && r.comp_size > 0) {
                            total_comp += r.comp_size;
                            comp_count++;
                        }
                        total_comp_ns += r.comp_ns;
                        total_decomp_ns += r.decomp_ns;
                    }
                }

                double avg_comp = comp_count > 0
                    ? static_cast<double>(total_comp) / comp_count : 0;
                double ratio = avg_comp > 0 ? avg_comp / group_bytes : 1.0;
                double comp_mbs = (group_bytes * ngroups * kIter) /
                                  (total_comp_ns / 1e9) / (1024 * 1024);
                double decomp_mbs = (total_decomp_ns > 0)
                    ? (group_bytes * ngroups * kIter) /
                      (total_decomp_ns / 1e9) / (1024 * 1024)
                    : 0;

                printf("%-8s %-10s %4dx%zu %8.0f %7.1f%% %9.0f %9.0f\n",
                       (gi == 0 && mi == 0) ? data_types[d].name : "",
                       algos[a].name, gs, kPageSize / 1024,
                       avg_comp, ratio * 100, comp_mbs, decomp_mbs);
            }
        }
        printf("\n");
    }

    // ── Part 3: OS comparison summary ───────────────────────────────────

    printf("\n═══════════════════════════════════════════════════════════════════\n");
    printf("  OS VM COMPRESSOR COMPARISON NOTES\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");
    printf("macOS:   WKdm (word-key dictionary, 16-entry dict, 2-bit tags)\n");
    printf("         Operates on 32-bit words, very fast, lower ratios than LZ4.\n");
    printf("         Designed for 4KB pages; Apple has WKdm_compress_16k for ARM64.\n\n");
    printf("Linux:   LZ4 (default for zswap since kernel 5.x), LZO (older default)\n");
    printf("         zstd also available as zswap backend. 4KB pages.\n\n");
    printf("Windows: Xpress (LZ77 + Huffman). MemCompression process.\n");
    printf("         Similar performance profile to LZ4. 4KB pages.\n\n");
    printf("Smash:   LZ4 (cold) -> zstd-9 (very cold). 16KB pages (ARM64 macOS).\n");
    printf("         Multi-algorithm adaptive: best of both worlds.\n\n");

    free(pages);
    free(comp_buf);
    free(verify_buf);
    free(scratch);
    return 0;
}
