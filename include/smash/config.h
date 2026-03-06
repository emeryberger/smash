// smash/config.h - Compile-time tuning knobs
#pragma once

#include <alloc8/platform.h>
#include <cstddef>
#include <cstdint>

namespace smash {

// ── Page size ────────────────────────────────────────────────────────────────
inline constexpr size_t kPageSize = ALLOC8_PAGE_SIZE;
inline constexpr int kPageShift = (kPageSize == 16384) ? 14 : 12;

// ── Size classes ─────────────────────────────────────────────────────────────
inline constexpr size_t kMaxSmallSize = 16384;
inline constexpr int kNumClasses = 36;
inline constexpr size_t kMinAlignment = 16;

// ── Arenas ───────────────────────────────────────────────────────────────────
inline constexpr int kNumArenas = 4;  // must be power of 2

// ── Spans ────────────────────────────────────────────────────────────────────
inline constexpr int kTargetObjectsPerSpan = 64;
inline constexpr int kMaxSpanPages = 8;

// ── Bootstrap allocator ──────────────────────────────────────────────────────
inline constexpr size_t kBootstrapInitialSize = 64 * 1024 * 1024;   // 64 MB
inline constexpr size_t kBootstrapExpandSize = 16 * 1024 * 1024;    // 16 MB
inline constexpr int kBootstrapMaxRegions = 64;

// ── Thread cache ─────────────────────────────────────────────────────────────
inline constexpr int kThreadCacheMaxPerClass = 64;
inline constexpr int kThreadCacheBatchSize = 32;

// ── Page map ─────────────────────────────────────────────────────────────────
// 48-bit virtual address space assumed
inline constexpr int kAddressBits = 48;
inline constexpr int kPageMapL2Bits = 16;
inline constexpr size_t kPageMapL2Size = 1ULL << kPageMapL2Bits;
inline constexpr int kPageMapL1Bits = kAddressBits - kPageShift - kPageMapL2Bits;
inline constexpr size_t kPageMapL1Size = 1ULL << kPageMapL1Bits;

// ── Compression (Phase 3+) ───────────────────────────────────────────────────
inline constexpr int kCompressIntervalMs = 1000;
inline constexpr int kColdTicks = 2;
inline constexpr double kMinCompressRatio = 0.75;

// ── Adaptive compression (Phase 5+) ─────────────────────────────────────────
inline constexpr int kVeryColdTicks = 60;         // ~1 min → zstd deep
inline constexpr int kDictTrainSamples = 16;      // pages before dict training
inline constexpr int kPrefetchWindow = 2;         // pages each direction on fault
inline constexpr int kZstdNormalLevel = 3;
inline constexpr int kZstdDeepLevel = 9;

// ── Compressor parallelism ───────────────────────────────────────────────────
inline constexpr int kCompressorWorkers = 2;         // parallel compression workers
inline constexpr int kCompressStoreShards = 8;       // CompressStore lock shards
inline constexpr int kChunkBits = 6;                 // 64 pages per chunk
inline constexpr int kChunkSize = 1 << kChunkBits;   // pages per chunk

// ── Zero-on-free ────────────────────────────────────────────────────────────
inline constexpr bool kZeroOnFree = true;
inline constexpr size_t kZeroOnFreeMaxSize = 128;  // eager memset up to this size

// ── Large allocation compression ────────────────────────────────────────────
// Only large allocations >= this size are placed in the VmRegion for
// compression tracking.  Smaller "large" allocs (16KB–256KB) are typically
// internal engine buffers accessed frequently; compressing them causes
// decompression storms when they're needed again.
inline constexpr size_t kLargeAllocVmThreshold = 1024 * 1024;  // 1 MB

// ── Virtual memory region ────────────────────────────────────────────────────
inline constexpr size_t kVmMaxPages = 1024 * 1024;  // 1M pages (~16GB on 16K pages)
inline constexpr size_t kVmRegionSize = kVmMaxPages * kPageSize;

} // namespace smash
