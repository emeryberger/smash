// smash/config.h - Compile-time tuning knobs
#pragma once

#include <alloc8/platform.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

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

// ── Reference-behavior homogeneity knobs (Apr 2026 design memo) ─────────────
//
// A3: Cold-bias feedback arenas.  When on, the slab array is doubled to
// maintain a hot sub-arena (index 0..kNumArenas-1) and a cold sub-arena
// (index kNumArenas..2*kNumArenas-1) for each (callsite, size class).
// A per-(base-arena, size-class) flag flips to cold once the compressor
// records >= kColdArenaThreshold successful compressions for that bucket;
// subsequent allocations route to the cold sub-arena, where they may be
// placed under an underfill policy (C1).
#ifdef SMASH_COLD_ARENA_FEEDBACK
inline constexpr bool kColdArenaFeedback = true;
#else
inline constexpr bool kColdArenaFeedback = false;
#endif
#ifndef SMASH_COLD_ARENA_THRESHOLD
inline constexpr uint32_t kColdArenaThreshold = 8;
#else
inline constexpr uint32_t kColdArenaThreshold = SMASH_COLD_ARENA_THRESHOLD;
#endif
inline constexpr int kTotalArenas = kColdArenaFeedback ? (kNumArenas * 2) : kNumArenas;

// C1: Per-page absolute cap on live objects.  The (1-q)^N argument for
// page-cold probability bounds N (objects per page), not the fraction of
// the span used.  A fractional denominator is the wrong knob: with
// denom=4, a 16B size class still puts 256 objects on a page (P(cold)
// ≈ 0 even for tiny q), while a 4KB size class drops to 1 object/page
// (huge underfill cost for the same probability bound).
//
// kMaxSlotsPerPage caps the number of slots whose start address lies on
// any single page.  Spans for cold sub-arenas (A3 on) or all spans (A3
// off) reserve only the first kMaxSlotsPerPage slots per page; the
// remaining bytes stay zero-filled and compress to near-nothing.
// 0 = no cap (default).  Cap is silently ignored when object_size >=
// kPageSize (slot already covers a full page).
#ifndef SMASH_MAX_SLOTS_PER_PAGE
inline constexpr uint32_t kMaxSlotsPerPage = 0;
#else
inline constexpr uint32_t kMaxSlotsPerPage = SMASH_MAX_SLOTS_PER_PAGE;
#endif

// A2-lite: Thread identity in the arena hash.  When on, callsiteArena()
// XORs a per-thread id into the (RA, frame, sc) hash so allocations from
// different threads are more likely to land in different arenas.  Cheap —
// uses a TLS-cached monotonic id assigned on first call.  Effectiveness
// is bounded by kNumArenas: with 4 arenas, ~4 threads saturate the space
// and the extra term is noise beyond that; consider raising kNumArenas
// to 8 or 16 for heavier multi-threaded workloads.
#ifdef SMASH_THREAD_ARENA_HASH
inline constexpr bool kThreadArenaHash = true;
#else
inline constexpr bool kThreadArenaHash = false;
#endif

// B1: Page-local batch refill.  When on, Slab::allocateBatch stops once
// the next object would fall on a different page than the batch's first
// object.  Keeps each thread-cache refill confined to one page, so
// objects allocated in the same burst share a lifecycle at page grain.
#ifdef SMASH_PAGE_LOCAL_BATCH
inline constexpr bool kPageLocalBatch = true;
#else
inline constexpr bool kPageLocalBatch = false;
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

// ── ROI model defaults (overridable via SMASH_ROI_THRESHOLD env var) ─────────
inline constexpr int kRoiThresholdDefault = 1024;

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
inline constexpr int kZstdFastLevel = 1;            // fast tier (replaces LZ4)
inline constexpr int kZstdNormalLevel = 3;
inline constexpr int kZstdDeepLevel = 9;

// When true, use LZ4 as the fast compression tier (original behavior).
// When false (default), use zstd-1 as the fast tier for better ratios.
#ifdef SMASH_USE_LZ4
inline constexpr bool kUseLz4FastTier = true;
#else
inline constexpr bool kUseLz4FastTier = false;
#endif
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
// All zeroing is deferred to the compressor thread (zeroFreeSlots).
// No zeroing occurs in the free() critical path.

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

// ── Runtime mode detection ───────────────────────────────────────────────────
// Set SMASH_MODE=compress_only to enable compress-only mode at runtime.
// In compress-only mode, malloc is forwarded to the system allocator and
// pages are tracked for compression. This works with any allocator (jemalloc,
// tcmalloc, etc.) but doesn't benefit from Smash's allocation optimizations.

enum class SmashMode { Full, CompressOnly };

inline SmashMode getSmashMode() {
    static SmashMode mode = []() {
        const char* env = getenv("SMASH_MODE");
        if (env && strcmp(env, "compress_only") == 0)
            return SmashMode::CompressOnly;
        return SmashMode::Full;
    }();
    return mode;
}

inline bool isCompressOnlyMode() {
    return getSmashMode() == SmashMode::CompressOnly;
}

} // namespace smash
