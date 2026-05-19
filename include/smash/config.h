// smash/config.h - Compile-time tuning knobs
#pragma once

#include <alloc8/platform.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace smash {

// ── Page size ────────────────────────────────────────────────────────────────
inline constexpr size_t kPageSize = ALLOC8_PAGE_SIZE;
inline constexpr int kPageShift = (kPageSize == 16384) ? 14 : 12;

// ── Size classes ─────────────────────────────────────────────────────────────
inline constexpr size_t kMaxSmallSize = 16384;
inline constexpr int kNumClasses = 36;
inline constexpr size_t kMinAlignment = 16;

// ── Arenas ───────────────────────────────────────────────────────────────────
// kMaxArenas is the compile-time upper bound for arena arrays.
// Runtime arena count is determined by getNumArenas() based on CPU count.
#ifndef SMASH_MAX_ARENAS
inline constexpr int kMaxArenas = 128;  // max supported arenas (must be power of 2)
#else
inline constexpr int kMaxArenas = SMASH_MAX_ARENAS;
#endif

// For backward compatibility, kNumArenas is now an alias for getNumArenas()
// in most places. Code that needs the compile-time max should use kMaxArenas.
#ifndef SMASH_NUM_ARENAS
inline constexpr int kNumArenasDefault = 4;  // fallback if CPU detection fails
#else
inline constexpr int kNumArenasDefault = SMASH_NUM_ARENAS;
#endif

// Dynamic arena count based on CPU count using balls-to-bins analysis.
// With n threads and m arenas, max load per arena ~ n/m + sqrt(2n*ln(m)/m).
// For good load balancing, we want m ~ sqrt(n) * c where c ~ 2-4.
// We use m = max(4, min(kMaxArenas, roundUpPow2(sqrt(ncpu) * 4))).
[[gnu::always_inline]]
inline int getNumArenas() {
    static std::atomic<int> cached{-1};
    int v = cached.load(std::memory_order_relaxed);
    if (v < 0) [[unlikely]] {
        // Check for explicit override first
        const char* env = getenv("SMASH_NUM_ARENAS");
        if (env && *env) {
            int parsed = atoi(env);
            if (parsed > 0 && parsed <= kMaxArenas) {
                // Round up to power of 2
                int p = 1;
                while (p < parsed) p <<= 1;
                v = (p <= kMaxArenas) ? p : kMaxArenas;
                cached.store(v, std::memory_order_relaxed);
                return v;
            }
        }
        // Auto-scale based on CPU count
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        if (ncpu <= 0) ncpu = 4;
        // m = sqrt(ncpu) * 4, rounded up to power of 2, clamped to [4, kMaxArenas]
        // For ncpu=4: m=8, ncpu=16: m=16, ncpu=64: m=32, ncpu=192: m=64
        double target = 4.0;
        if (ncpu > 4) {
            // Use sqrt(ncpu) * 4 for larger CPU counts
            double sqrtn = 1.0;
            for (int i = 0; i < 20 && sqrtn * sqrtn < ncpu; ++i) sqrtn += 0.5;
            target = sqrtn * 4.0;
        }
        int m = 4;
        while (m < target && m < kMaxArenas) m <<= 1;
        v = m;
        cached.store(v, std::memory_order_relaxed);
    }
    return v;
}

// Arena mask for fast modulo (arenas must be power of 2)
[[gnu::always_inline]]
inline int getArenaMask() {
    return getNumArenas() - 1;
}

// Legacy alias - compile-time arrays still need a constexpr size
inline constexpr int kNumArenas = kMaxArenas;

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
// Compile-time max for array sizing (uses kMaxArenas, not runtime count)
inline constexpr int kTotalArenas = kColdArenaFeedback ? (kMaxArenas * 2) : kMaxArenas;

// Runtime total arena count (hot + cold if feedback enabled)
[[gnu::always_inline]]
inline int getTotalArenas() {
    return kColdArenaFeedback ? (getNumArenas() * 2) : getNumArenas();
}

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

// C1b: Adaptive per-page cap driven by compression/decompression feedback.
// When on, the heap tracks per-(base_arena, size_class) counters for both
// successful compression events (cold evidence) and fault decompression
// events (hot evidence, i.e., the page was re-warmed after being cold).
// q̂ = decomp / (comp + decomp) is an online Pareto estimator; N is chosen
// so (1 - q̂)^N >= kAdaptiveCapTargetPct / 100 (i.e., target probability
// that a freshly allocated page stays uniformly cold for one tick window).
// Below kAdaptiveCapMinSamples total events the cap is disabled
// (insufficient evidence).  For hot-dominated buckets (q̂ too high to
// satisfy the target at any N >= kAdaptiveCapMin), the cap is also
// disabled — under-packing a hot bucket just balloons RSS.
#ifdef SMASH_ADAPTIVE_CAP
inline constexpr bool kAdaptiveCap = true;
#else
inline constexpr bool kAdaptiveCap = false;
#endif
#ifndef SMASH_ADAPTIVE_CAP_TARGET_PCT
inline constexpr int kAdaptiveCapTargetPct = 50;   // P(page cold) target
#else
inline constexpr int kAdaptiveCapTargetPct = SMASH_ADAPTIVE_CAP_TARGET_PCT;
#endif
#ifndef SMASH_ADAPTIVE_CAP_MIN
inline constexpr uint32_t kAdaptiveCapMin = 4;     // floor on N
#else
inline constexpr uint32_t kAdaptiveCapMin = SMASH_ADAPTIVE_CAP_MIN;
#endif
#ifndef SMASH_ADAPTIVE_CAP_MIN_SAMPLES
inline constexpr uint32_t kAdaptiveCapMinSamples = 16;
#else
inline constexpr uint32_t kAdaptiveCapMinSamples = SMASH_ADAPTIVE_CAP_MIN_SAMPLES;
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

// Cohort measurement: track per-page thread/call-site mixing and print
// a summary each compressor tick.  Measurement only — no behavioral change.
#ifdef SMASH_MEASURE_COHORTS
inline constexpr bool kMeasureCohorts = true;
#else
inline constexpr bool kMeasureCohorts = false;
#endif

// ── Spans ────────────────────────────────────────────────────────────────────
inline constexpr int kTargetObjectsPerSpan = 64;
inline constexpr int kMaxSpanPages = 8;

// ── Bootstrap allocator ──────────────────────────────────────────────────────
inline constexpr size_t kBootstrapInitialSize = 64 * 1024 * 1024;   // 64 MB
inline constexpr size_t kBootstrapExpandSize = 16 * 1024 * 1024;    // 16 MB
inline constexpr int kBootstrapMaxRegions = 64;

// ── Thread cache ─────────────────────────────────────────────────────────────
// Doubled from 64/32 to halve the rate of slab-lock acquisitions on
// pgbench-class workloads.  Per-thread memory cost: 64 size classes ×
// 128 ptrs × 8 B = 64 KiB per thread (was 32 KiB).  Cool-tail RSS ratio
// at 99.3 % is the gate — a per-thread 32 KiB inflation is negligible.
inline constexpr int kThreadCacheMaxPerClass = 128;
inline constexpr int kThreadCacheBatchSize = 64;

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
// Default 10 ticks (= 10 s at the 1 s tick interval). Lower values compress
// more aggressively but starve continuous workloads of read-only working set
// — PROT_READ-monitoring is blind to reads, so a page being constantly read
// looks identical to an idle one. Span::allocate() resets cold_count on
// alloc activity, but the program may go from "burst-allocating" to
// "read-only execution" with no further allocs, and a 2-tick floor will
// then catch the read-only working set in 2 s flat. 10 ticks gives the
// workload time to establish access patterns through the rc-backoff
// mechanism (one fault → eff_floor doubles) before the wave of fresh
// pages becomes eligible.
//
// Tests pin their own value via SMASH_COLD_TIMEOUT_SEC / SMASH_COLD_TICKS,
// so unit-test latency is unaffected.
#ifndef SMASH_COLD_TICKS
inline constexpr int kColdTicksDefault = 10;
#else
inline constexpr int kColdTicksDefault = SMASH_COLD_TICKS;
#endif
inline constexpr double kMinCompressRatio = 0.75;

// Runtime cold timeout: SMASH_COLD_TIMEOUT_SEC overrides kColdTicks.
// This is the primary time-space tradeoff dial.  Lower values compress
// sooner (more space savings, more decompression faults on re-access).
// Higher values are conservative (less savings, fewer faults).
// Default: kColdTicksDefault * kCompressIntervalMs / 1000.
inline int getColdTicks() {
    static int ticks = []() {
        const char* env = getenv("SMASH_COLD_TIMEOUT_SEC");
        if (env) {
            double sec = atof(env);
            if (sec > 0)
                return static_cast<int>(sec * 1000.0 / kCompressIntervalMs + 0.5);
        }
        return kColdTicksDefault;
    }();
    return ticks;
}

// ── Adaptive compression (Phase 5+) ─────────────────────────────────────────
#ifndef SMASH_VERY_COLD_TICKS
inline constexpr int kVeryColdTicks = 60;         // ~1 min → zstd deep
#else
inline constexpr int kVeryColdTicks = SMASH_VERY_COLD_TICKS;
#endif

// ── Recompression-thrash back-off ──────────────────────────────────────────
//
// When a page is compressed and then immediately faulted back (compress →
// decompress → recompress loop), per-page `recompress_count_` is bumped on
// each fault. The compressor's phase 2 gate raises the effective cold-tick
// floor by `floor << min(recompress_count + bucket_bias, kMaxBackoffShift)`,
// so a thrashy page must stay idle proportionally longer before being
// eligible for compression again. The penalty decays once the page truly
// cools off — see compressor_thread.h::phase1Range.
#ifndef SMASH_RECOMPRESS_MAX_SHIFT
inline constexpr int kMaxBackoffShift = 6;        // 64× floor max
#else
inline constexpr int kMaxBackoffShift = SMASH_RECOMPRESS_MAX_SHIFT;
#endif

#ifndef SMASH_RECOMPRESS_MAX_FLOOR_TICKS
inline constexpr int kMaxEffectiveFloorTicks = 200;  // sanity cap (~3.3 min)
#else
inline constexpr int kMaxEffectiveFloorTicks = SMASH_RECOMPRESS_MAX_FLOOR_TICKS;
#endif

#ifndef SMASH_RECOMPRESS_DECAY_TICKS
inline constexpr int kRcDecayColdTicks = 8;       // decay edge spacing in cold-ticks
#else
inline constexpr int kRcDecayColdTicks = SMASH_RECOMPRESS_DECAY_TICKS;
#endif

// EMA divisor (×256 fixed-point) for proportional per-bucket bias on the
// back-off shift. `bucket_bias = bucket_ema_x256 / kBucketRcBiasThreshold_x256`
// (capped at kMaxBackoffShift). 256 = "every full unit of average rc in this
// bucket adds +1 to the shift", so a single recompress event in a bucket
// immediately gives every other page in that bucket bias=1 (eff_floor=2×).
// This is load-bearing: without aggressive propagation, fresh pages from a
// thrashy call site each have to individually thrash before any backoff
// applies, which doesn't converge on workloads that allocate hundreds of
// pages per second from the same site.
#ifndef SMASH_RECOMPRESS_BUCKET_BIAS_X256
inline constexpr int kBucketRcBiasThreshold_x256 = 256;
#else
inline constexpr int kBucketRcBiasThreshold_x256 = SMASH_RECOMPRESS_BUCKET_BIAS_X256;
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

// Tiered recompression (in development): when true, the compressor scans
// COMPRESSED pages and upgrades fast-tier blobs (zstd-1 by default, LZ4
// when SMASH_USE_LZ4) to deep-tier (zstd-9) once a page has stayed cold
// past kVeryColdTicks.  Without this flag, the initial algorithm chosen
// at compress time is final until the page is decompressed.
//
// Does NOT change the initial tier — see SMASH_USE_LZ4 for that.
// See TIERED_RECOMPRESSION.md for design and measurements.
#ifndef SMASH_NO_TIERED_RECOMPRESSION
inline constexpr bool kTieredRecompression = true;
#else
inline constexpr bool kTieredRecompression = false;
#endif

// When true, use LZ4 as the fast compression tier (opt-in via SMASH_USE_LZ4).
// When false (default), use zstd-1 as the fast tier for better ratios.
//
// We measured LZ4 initial + zstd-9 upgrade on memcached and Redis: peak
// fill_rss is unchanged vs zstd-1 (the bottleneck isn't the algorithm),
// and cool_rss costs ~9-33 MB vs zstd-1 due to whole-region waste from
// "stranger" blobs that prevent draining.  Keep as opt-in; the
// CompressStore freelist (this branch) makes the cost bounded when
// users do opt in.
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
inline constexpr int kCompressorWorkers = 2;         // initial compression workers
#else
inline constexpr int kCompressorWorkers = SMASH_COMPRESSOR_WORKERS;
#endif
#ifndef SMASH_MAX_COMPRESSOR_WORKERS
inline constexpr int kMaxCompressorWorkers = 8;      // max workers (pre-allocated pool)
#else
inline constexpr int kMaxCompressorWorkers = SMASH_MAX_COMPRESSOR_WORKERS;
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

// Mode helpers used to wrap their cached values in `static T cached = []()...();`,
// which triggers the C++ magic-static guard: an acquire-load + branch on every
// call.  perf-record on a smash-LD_PRELOADed micro-bench showed those guards
// taking ~7-10 % of free()'s CPU.  Switch to a hand-rolled lazy init backed by
// std::atomic<int>: the post-init fast path is a single relaxed `ldr`, no
// acquire fence, no guard check.  -1 = uninitialised, all other values are
// the cached state.
inline SmashMode getSmashMode() {
    static std::atomic<int> cached{-1};
    int v = cached.load(std::memory_order_relaxed);
    if (v < 0) [[unlikely]] {
        const char* env = getenv("SMASH_MODE");
        v = (env && strcmp(env, "compress_only") == 0)
            ? static_cast<int>(SmashMode::CompressOnly)
            : static_cast<int>(SmashMode::Full);
        cached.store(v, std::memory_order_relaxed);
    }
    return static_cast<SmashMode>(v);
}

[[gnu::always_inline, gnu::hot]]
inline bool isCompressOnlyMode() {
    return getSmashMode() == SmashMode::CompressOnly;
}

// ── Large-only mode ─────────────────────────────────────────────────────────
// Set SMASH_LARGE_ONLY=1 to only manage large allocations through Smash.
// Small allocations (size <= kMaxSmallSize) pass through to the system
// allocator.  This avoids interfering with language runtimes that use their
// own small-object allocator (e.g. Python 3.13+ mimalloc).
[[gnu::always_inline, gnu::hot]]
inline bool isLargeOnlyMode() {
    static std::atomic<int> cached{-1};
    int v = cached.load(std::memory_order_relaxed);
    if (v < 0) [[unlikely]] {
        const char* env = getenv("SMASH_LARGE_ONLY");
        v = (env && env[0] == '1') ? 1 : 0;
        cached.store(v, std::memory_order_relaxed);
    }
    return v == 1;
}

// Threshold for what counts as "small" (passthrough to system) in
// LARGE_ONLY mode. Defaults to kMaxSmallSize (16 KB). Lower values
// route a larger fraction of allocations through smash — useful when
// the workload's byte-volume is dominated by mid-sized allocations
// (e.g. Firefox's 4 KB–7 KB bucket carries ~20 % of allocation volume
// while sitting under the default threshold). Must not exceed
// kMaxSmallSize since smash's slab path tops out there.
[[gnu::always_inline, gnu::hot]]
inline size_t largeOnlyThreshold() {
    static std::atomic<size_t> cached{0};   // 0 = unset
    size_t v = cached.load(std::memory_order_relaxed);
    if (v == 0) [[unlikely]] {
        const char* env = getenv("SMASH_LARGE_ONLY_THRESHOLD");
        size_t result = kMaxSmallSize;
        if (env && *env) {
            size_t parsed = static_cast<size_t>(atoll(env));
            if (parsed > 0 && parsed <= kMaxSmallSize) result = parsed;
        }
        cached.store(result, std::memory_order_relaxed);
        return result;
    }
    return v;
}

// ── Deferred-reclaim mode ───────────────────────────────────────────────────
// Set SMASH_DEFERRED_RECLAIM=1 to split compression into two phases:
//   Phase A: compress a copy, keep page accessible (PROT_RW) → COMPRESSED_SHADOW
//   Phase B: after N ticks of confirmed idleness, reclaim physical memory → COMPRESSED
// This makes Smash compatible with systems that have uncoordinated background
// threads (e.g. WiredTiger's eviction threads) that would crash on PROT_NONE.
[[gnu::always_inline, gnu::hot]]
inline bool isDeferredReclaimMode() {
    static std::atomic<int> cached{-1};
    int v = cached.load(std::memory_order_relaxed);
    if (v < 0) [[unlikely]] {
        const char* env = getenv("SMASH_DEFERRED_RECLAIM");
        v = (env && env[0] == '1') ? 1 : 0;
        cached.store(v, std::memory_order_relaxed);
    }
    return v == 1;
}

inline int getDeferredReclaimDelay() {
    static int ticks = []{
        const char* env = getenv("SMASH_DEFERRED_RECLAIM_DELAY");
        return (env && atoi(env) > 0) ? atoi(env) : 2;
    }();
    return ticks;
}

// ── Eager-zero mode ─────────────────────────────────────────────────────────
// Set SMASH_EAGER_ZERO=1 to memset newly-allocated buffers to zero on the
// malloc fast path, instead of relying on the compressor thread's deferred
// zero-on-free pass. This trades throughput for correctness with callers
// that assume malloc returns zeroed memory (technically UB, but real-world
// codebases rely on it). Used to A/B test whether deferred-zero is the
// source of data-corruption crashes (e.g. ARM64 PAC failures from stale
// pointer-shaped bytes in reissued slab slots).
[[gnu::always_inline, gnu::hot]]
inline bool isEagerZeroMode() {
    static std::atomic<int> cached{-1};
    int v = cached.load(std::memory_order_relaxed);
    if (v < 0) [[unlikely]] {
        const char* env = getenv("SMASH_EAGER_ZERO");
        v = (env && env[0] == '1') ? 1 : 0;
        cached.store(v, std::memory_order_relaxed);
    }
    return v == 1;
}

} // namespace smash
