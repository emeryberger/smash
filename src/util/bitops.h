// smash/src/util/bitops.h - Bit manipulation primitives
#pragma once

#include <cstdint>
#include <cstddef>

namespace smash {

// Count trailing zeros (undefined for 0)
inline int ctz(uint64_t x) {
    return __builtin_ctzll(x);
}

// Count leading zeros (undefined for 0)
inline int clz(uint64_t x) {
    return __builtin_clzll(x);
}

// Population count
inline int popcount(uint64_t x) {
    return __builtin_popcountll(x);
}

// Floor of log2 (undefined for 0)
inline int log2Floor(uint64_t x) {
    return 63 - __builtin_clzll(x);
}

// Ceil of log2 (returns 0 for x <= 1)
inline int log2Ceil(uint64_t x) {
    if (x <= 1) return 0;
    return 64 - __builtin_clzll(x - 1);
}

// Round up to next multiple of alignment (alignment must be power of 2)
inline size_t roundUp(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// Check if value is a power of 2
inline bool isPowerOf2(size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

} // namespace smash
