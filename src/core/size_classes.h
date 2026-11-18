// smash/src/core/size_classes.h - Size class table and fast mapping
//
// 52 size classes with sub-linear spacing:
//   step 16:   16 .. 128     (8 classes)
//   step 32:   160 .. 256    (4 classes)
//   step 64:   320 .. 512    (4 classes)
//   step 128:  640 .. 1024   (4 classes)
//   step 128:  1152 .. 2048  (8 classes)
//   step 256:  2304 .. 4096  (8 classes)
//   step 512:  4608 .. 8192  (8 classes)
//   step 1024: 9216 .. 16384 (8 classes)
// Above 1 KiB the classes are 8 per doubling (worst-case rounding 12.5%,
// was 25% at 4 per doubling). The coarse spacing cost SQLite ~25%: its
// in-memory DB pages are ~4.37 KB, which rounded to 5120 and packed 6 to a
// 32 KiB span (5461 B each); they now take the 4608 class, 7 per span.
//
// SMASH_FINE_1K_CLASSES (CMake option, default off): 60 classes, with
// 1088 .. 2048 in steps of 64 (16 classes). Redis's 1 KiB values are 1030 B
// sds strings: 1152 B each (+12%) by default, 1088 B (+6%) with the option;
// glibc's boundary-tag chunks are 1040 B. See agents/containers.md.
#pragma once

#include "smash/config.h"
#include "../util/bitops.h"
#include <cstddef>
#include <cstdint>

namespace smash {

struct SizeClassInfo {
    uint32_t size;       // allocation size for this class
    uint32_t pages;      // pages per span
    uint32_t objects;    // objects per span
};

namespace detail {

constexpr uint32_t kClassSizes[kNumClasses] = {
    16,    32,    48,    64,    80,    96,    112,   128,    // step 16
    160,   192,   224,   256,                                // step 32
    320,   384,   448,   512,                                // step 64
    640,   768,   896,   1024,                               // step 128
#if SMASH_FINE_1K_CLASSES
    1088,  1152,  1216,  1280,  1344,  1408,  1472,  1536,   // step 64
    1600,  1664,  1728,  1792,  1856,  1920,  1984,  2048,   // step 64
#else
    1152,  1280,  1408,  1536,  1664,  1792,  1920,  2048,   // step 128
#endif
    2304,  2560,  2816,  3072,  3328,  3584,  3840,  4096,   // step 256
    4608,  5120,  5632,  6144,  6656,  7168,  7680,  8192,   // step 512
    9216,  10240, 11264, 12288, 13312, 14336, 15360, 16384,  // step 1024
};

constexpr SizeClassInfo computeClassInfo(uint32_t size) {
    uint32_t target_bytes = kTargetObjectsPerSpan * size;
    uint32_t pages = (target_bytes + kPageSize - 1) / kPageSize;
    if (pages < 1) pages = 1;
    if (pages > static_cast<uint32_t>(kMaxSpanPages)) {
        // Capped classes (large objects): pick the span length in
        // [kMaxSpanPages/2, kMaxSpanPages] that wastes the least tail, ties
        // to the longer span. E.g. 5120 B: 5 pages hold 4 exactly, where 8
        // pages held 6 and wasted 2 KiB per span.
        uint32_t best = kMaxSpanPages, best_waste = ~0u;
        for (uint32_t p = kMaxSpanPages; p >= kMaxSpanPages / 2; --p) {
            const uint32_t bytes = p * static_cast<uint32_t>(kPageSize);
            if (bytes < size) break;
            const uint32_t waste = bytes % size;
            // Compare waste as a fraction of the span: waste/bytes.
            if (best_waste == ~0u ||
                static_cast<uint64_t>(waste) * best * kPageSize <
                static_cast<uint64_t>(best_waste) * bytes) {
                best = p; best_waste = waste;
            }
        }
        pages = best;
    }
    uint32_t objects = (pages * kPageSize) / size;
    return {size, pages, objects};
}

constexpr auto buildClassTable() {
    struct Table { SizeClassInfo entries[kNumClasses]; };
    Table t{};
    for (int i = 0; i < kNumClasses; ++i)
        t.entries[i] = computeClassInfo(kClassSizes[i]);
    return t;
}

constexpr auto kClassTableStorage = buildClassTable();

// Lookup table for sizes 0..1024, indexed by (size + 15) >> 4
// Maps to size class index.
constexpr auto buildSmallLookup() {
    struct Table { uint8_t entries[65]; };
    Table t{};
    // For each 16-byte-aligned bucket, find the smallest class that fits
    for (int idx = 0; idx <= 64; ++idx) {
        uint32_t maxSize = idx * 16;  // max size this bucket covers
        if (maxSize == 0) maxSize = 1;
        // Find first class with size >= maxSize
        for (int c = 0; c < kNumClasses; ++c) {
            if (kClassSizes[c] >= maxSize) {
                t.entries[idx] = static_cast<uint8_t>(c);
                break;
            }
        }
    }
    return t;
}

constexpr auto kSmallLookupStorage = buildSmallLookup();

} // namespace detail

inline constexpr const SizeClassInfo* kSizeClasses = detail::kClassTableStorage.entries;
inline constexpr const uint8_t* kSmallLookup = detail::kSmallLookupStorage.entries;

// Fast size → class index. Returns kNumClasses if size > kMaxSmallSize.
inline uint8_t sizeToClass(size_t size) {
    if (size == 0) size = 1;
    if (size <= 1024) [[likely]] {
        return kSmallLookup[(size + 15) >> 4];
    }
    if (size <= kMaxSmallSize) {
#if SMASH_FINE_1K_CLASSES
        if (size <= 2048) return static_cast<uint8_t>(20 + ((size - 1025) >> 6));
#endif
        // Log2-based computation for sizes 1025..16384
        int bits = 63 - __builtin_clzll(size - 1);   // floor(log2(size-1))
        int group = bits - 10;                         // 0..3
        size_t groupBase = 1ULL << bits;               // 1024, 2048, 4096, 8192
        // step = groupBase / 8 (128, 256, 512, 1024): a shift, not a div.
        int sub = static_cast<int>((size - groupBase - 1) >> (bits - 3));
        return static_cast<uint8_t>(20 + group * 8 + sub + (SMASH_FINE_1K_CLASSES ? 8 : 0));
    }
    return kNumClasses;
}

// Class size for a given class index
inline uint32_t classSize(uint8_t sc) {
    return kSizeClasses[sc].size;
}

} // namespace smash
