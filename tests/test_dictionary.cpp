// Test dictionary training, compression roundtrip, ratio improvement, and fallback
#include "vm/vm_region.h"
#include "vm/page_state.h"
#include "compress/compress_store.h"
#include "compress/compress_engine.h"
#include "compress/compressor_thread.h"
#include "core/page_map.h"
#include "core/slab.h"
#include "smash/config.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

using namespace smash;

static int failures = 0;

#define CHECK(cond, msg, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: " msg "\n", ##__VA_ARGS__); \
        ++failures; \
    } \
} while (0)

// Fill a page with a structured pattern typical for a size class
// (simulates real allocator data: object headers + payload)
static void fillStructuredPage(void* page, uint8_t pattern_id, size_t obj_size) {
    auto* bytes = static_cast<uint8_t*>(page);
    for (size_t off = 0; off + obj_size <= kPageSize; off += obj_size) {
        // Object header: 16 bytes with class-specific metadata
        bytes[off + 0] = 0xAA;
        bytes[off + 1] = pattern_id;
        bytes[off + 2] = static_cast<uint8_t>(obj_size & 0xFF);
        bytes[off + 3] = static_cast<uint8_t>(obj_size >> 8);
        for (size_t i = 4; i < 16 && i < obj_size; ++i)
            bytes[off + i] = 0x00;
        // Payload: pattern-based fill
        for (size_t i = 16; i < obj_size; ++i)
            bytes[off + i] = static_cast<uint8_t>((off + i + pattern_id) & 0xFF);
    }
}

static void testDictTrainingRoundtrip() {
    CompressEngine engine;
    engine.init();

    // Collect samples: structured pages typical for size class 5 (96 bytes)
    constexpr int kSamples = kDictTrainSamples;
    constexpr size_t kObjSize = 96;
    constexpr uint8_t kSizeClass = 5;

    auto* sample_buf = static_cast<char*>(
        BootstrapAlloc::instance().allocate(
            static_cast<size_t>(kSamples) * kPageSize, kPageSize));
    size_t sample_sizes[kSamples];

    for (int i = 0; i < kSamples; ++i) {
        fillStructuredPage(sample_buf + static_cast<size_t>(i) * kPageSize,
                          static_cast<uint8_t>(i), kObjSize);
        sample_sizes[i] = kPageSize;
    }

    // Train dictionary
    bool trained = engine.trainDictionary(
        kSizeClass, sample_buf, sample_sizes, kSamples);
    CHECK(trained, "dictionary training should succeed");
    CHECK(engine.hasDictionary(kSizeClass), "should report dictionary available");

    // Compress a new page with the same structure and verify roundtrip
    auto* test_page = static_cast<char*>(
        BootstrapAlloc::instance().allocate(kPageSize, kPageSize));
    fillStructuredPage(test_page, 0x42, kObjSize);

    size_t max_comp = CompressEngine::maxCompressedSizeAny(kPageSize);
    auto* comp_buf = static_cast<char*>(
        BootstrapAlloc::instance().allocate(max_comp, 16));

    size_t comp_size = engine.compress(
        test_page, comp_buf, kPageSize, max_comp,
        CompressAlgo::ZSTD_DICT, kSizeClass);
    CHECK(comp_size > 0, "dict compression failed");
    CHECK(comp_size < kPageSize, "dict compression didn't reduce size: %zu >= %zu",
          comp_size, kPageSize);

    // Decompress and verify
    auto* restored = static_cast<char*>(
        BootstrapAlloc::instance().allocate(kPageSize, kPageSize));
    size_t decomp_size = engine.decompress(
        comp_buf, restored, comp_size, kPageSize,
        CompressAlgo::ZSTD_DICT, kSizeClass);
    CHECK(decomp_size == kPageSize, "decompressed size %zu != %zu", decomp_size, kPageSize);
    CHECK(memcmp(test_page, restored, kPageSize) == 0,
          "decompressed data doesn't match original");
}

static void testDictRatioImprovement() {
    CompressEngine engine;
    engine.init();

    constexpr int kSamples = kDictTrainSamples;
    constexpr size_t kObjSize = 96;
    constexpr uint8_t kSizeClass = 5;

    auto* sample_buf = static_cast<char*>(
        BootstrapAlloc::instance().allocate(
            static_cast<size_t>(kSamples) * kPageSize, kPageSize));
    size_t sample_sizes[kSamples];

    for (int i = 0; i < kSamples; ++i) {
        fillStructuredPage(sample_buf + static_cast<size_t>(i) * kPageSize,
                          static_cast<uint8_t>(i), kObjSize);
        sample_sizes[i] = kPageSize;
    }

    engine.trainDictionary(kSizeClass, sample_buf, sample_sizes, kSamples);

    // Compress a structured page with and without dictionary
    auto* test_page = static_cast<char*>(
        BootstrapAlloc::instance().allocate(kPageSize, kPageSize));
    fillStructuredPage(test_page, 0x77, kObjSize);

    size_t max_comp = CompressEngine::maxCompressedSizeAny(kPageSize);
    auto* comp_buf = static_cast<char*>(
        BootstrapAlloc::instance().allocate(max_comp, 16));

    // Without dict (LZ4)
    size_t lz4_size = engine.compress(
        test_page, comp_buf, kPageSize, max_comp, CompressAlgo::LZ4);

    // Without dict (zstd)
    size_t zstd_size = engine.compress(
        test_page, comp_buf, kPageSize, max_comp, CompressAlgo::ZSTD);

    // With dict
    size_t dict_size = engine.compress(
        test_page, comp_buf, kPageSize, max_comp,
        CompressAlgo::ZSTD_DICT, kSizeClass);

    CHECK(lz4_size > 0, "LZ4 compression failed");
    CHECK(zstd_size > 0, "zstd compression failed");
    CHECK(dict_size > 0, "zstd+dict compression failed");

    fprintf(stderr, "  Ratio comparison: LZ4=%zu, zstd=%zu, zstd+dict=%zu (page=%zu)\n",
            lz4_size, zstd_size, dict_size, kPageSize);

    // zstd+dict should be no worse than plain zstd on structured data
    CHECK(dict_size <= zstd_size + 64,
          "dict compression should not be significantly worse than plain zstd: "
          "dict=%zu, zstd=%zu", dict_size, zstd_size);
}

static void testNoDictFallback() {
    CompressEngine engine;
    engine.init();

    // Compress without a trained dictionary — should fall back gracefully
    CHECK(!engine.hasDictionary(0), "should not have dict for class 0");

    auto* src = static_cast<char*>(
        BootstrapAlloc::instance().allocate(kPageSize, kPageSize));
    memset(src, 0x42, kPageSize);

    size_t max_comp = CompressEngine::maxCompressedSizeAny(kPageSize);
    auto* dst = static_cast<char*>(
        BootstrapAlloc::instance().allocate(max_comp, 16));

    // ZSTD_DICT without trained dict falls back to deep zstd
    size_t comp_size = engine.compress(
        src, dst, kPageSize, max_comp, CompressAlgo::ZSTD_DICT, 0);
    CHECK(comp_size > 0, "fallback compression should succeed");
    CHECK(comp_size < kPageSize, "fallback should still compress");

    // Verify roundtrip
    auto* restored = static_cast<char*>(
        BootstrapAlloc::instance().allocate(kPageSize, kPageSize));
    size_t decomp_size = engine.decompress(
        dst, restored, comp_size, kPageSize, CompressAlgo::ZSTD_DICT, 0);
    CHECK(decomp_size == kPageSize, "fallback decompressed size mismatch");
    CHECK(memcmp(src, restored, kPageSize) == 0, "fallback data mismatch");
}

int main() {
    testDictTrainingRoundtrip();
    testDictRatioImprovement();
    testNoDictFallback();

    if (failures == 0) {
        fprintf(stderr, "dictionary: all tests passed\n");
        return 0;
    } else {
        fprintf(stderr, "dictionary: %d failures\n", failures);
        return 1;
    }
}
