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
#ifndef SMASH_NUM_ARENAS
inline constexpr int kNumArenas = 4;  // must be power of 2
#else
inline constexpr int kNumArenas = SMASH_NUM_ARENAS;
#endif

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
#ifndef SMASH_COLD_TICKS
inline constexpr int kColdTicks = 2;
#else
inline constexpr int kColdTicks = SMASH_COLD_TICKS;
#endif
inline constexpr double kMinCompressRatio = 0.75;

// ── Adaptive compression (Phase 5+) ─────────────────────────────────────────
#ifndef SMASH_VERY_COLD_TICKS
inline constexpr int kVeryColdTicks = 60;         // ~1 min → zstd deep
#else
inline constexpr int kVeryColdTicks = SMASH_VERY_COLD_TICKS;
#endif
#ifndef SMASH_DICT_TRAIN_SAMPLES
inline constexpr int kDictTrainSamples = 0;       // disabled: dicts net-negative (see EXPERIMENTS.md)
#else
inline constexpr int kDictTrainSamples = SMASH_DICT_TRAIN_SAMPLES;
#endif
#ifndef SMASH_PREFETCH_WINDOW
inline constexpr int kPrefetchWindow = 2;         // pages each direction on fault
#else
inline constexpr int kPrefetchWindow = SMASH_PREFETCH_WINDOW;
#endif
inline constexpr int kZstdNormalLevel = 3;
inline constexpr int kZstdDeepLevel = 9;
inline constexpr int kDictLevel = 3;            // CDict built at level 3 (430KB vs 686KB at level 9)
inline constexpr size_t kDictCapacity = 16 * 1024; // 16KB dict (down from 32KB)
inline constexpr int kMaxDictClasses = 8;        // cap total dict memory overhead

// ── Compressor parallelism ───────────────────────────────────────────────────
#ifndef SMASH_COMPRESSOR_WORKERS
inline constexpr int kCompressorWorkers = 2;         // parallel compression workers
#else
inline constexpr int kCompressorWorkers = SMASH_COMPRESSOR_WORKERS;
#endif
#ifndef SMASH_COMPRESS_STORE_SHARDS
inline constexpr int kCompressStoreShards = 8;       // CompressStore lock shards
#else
inline constexpr int kCompressStoreShards = SMASH_COMPRESS_STORE_SHARDS;
#endif
inline constexpr int kChunkBits = 6;                 // 64 pages per chunk
inline constexpr int kChunkSize = 1 << kChunkBits;   // pages per chunk

// ── Zero-on-free ────────────────────────────────────────────────────────────
#ifdef SMASH_ABLATION_NO_ZERO_EAGER
inline constexpr bool kZeroOnFree = false;
#else
inline constexpr bool kZeroOnFree = true;
#endif
inline constexpr size_t kZeroOnFreeMaxSize = 128;  // eager memset up to this size

// ── Large allocation compression ────────────────────────────────────────────
// Only large allocations >= this size are placed in the VmRegion for
// compression tracking.  Smaller "large" allocs (16KB–256KB) are typically
// internal engine buffers accessed frequently; compressing them causes
// decompression storms when they're needed again.
#ifndef SMASH_LARGE_ALLOC_VM_THRESHOLD
inline constexpr size_t kLargeAllocVmThreshold = 1024 * 1024;  // 1 MB
#else
inline constexpr size_t kLargeAllocVmThreshold = SMASH_LARGE_ALLOC_VM_THRESHOLD;
#endif

// ── Virtual memory region ────────────────────────────────────────────────────
inline constexpr size_t kVmMaxPages = 1024 * 1024;  // 1M pages (~16GB on 16K pages)
inline constexpr size_t kVmRegionSize = kVmMaxPages * kPageSize;

} // namespace smash
