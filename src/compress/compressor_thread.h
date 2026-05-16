// smash/src/compress/compressor_thread.h - Background scan-and-compress loop
//
// Integrates cold page detection (access tracking) and multi-algorithm
// compression (LZ4/zstd/zstd+dict). Supports adaptive algorithm selection
// based on cold duration, per-size-class dictionary training, prefetching
// of adjacent pages on fault, and batch decommit.
//
// Parallelism: adaptive worker pool (Little's Law) processes disjoint page ranges.
// Chunk bitmap skips EMPTY pages. Per-fault-slot DCtx avoids decompress races.
// Sharded CompressStore eliminates single lock bottleneck.
#pragma once

#include "smash/config.h"
#include "compress_store.h"
#include "compress_engine.h"
#include "compression_roi.h"
#include "../vm/vm_region.h"
#include "../vm/page_state.h"
#include "../vm/platform_mem.h"
#include "../core/bootstrap_alloc.h"
#include "../core/page_map.h"
#include "../util/spinlock.h"

#include <csignal>
#include <cstdio>
#include "../vm/fault_handler.h"
#include "../vm/syscall_compat.h"

#include <atomic>
#include <pthread.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <dirent.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include <cstdio>
#include <lz4.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>  // _mm_stream_si64, _mm_sfence
#endif

namespace smash {

class CompressorThread {
    // ── Shared state ──────────────────────────────────────────────────────
    VmRegion* vm_ = nullptr;
    PageStateTable* states_ = nullptr;
    PageLockTable* locks_ = nullptr;
    CompressStore* store_ = nullptr;
    CompressEngine* engine_ = nullptr;      // shared: dict training, dict lookup
    PageMap* page_map_ = nullptr;
    vm::FaultHandler* fault_handler_ = nullptr;

    // Optional pre-tick callback (e.g., for VM region scanning in compress-only mode)
    using PreTickFn = void(*)();
    PreTickFn pre_tick_fn_ = nullptr;

    // Optional callback invoked once per successful page compression.
    // Used by SmashHeap to drive the A3 cold-arena feedback loop:
    // arena_id and size_class identify the originating slab.
    using CompressedFn = void(*)(size_t page_idx, uint8_t arena_id,
                                 uint8_t sc, void* ctx);
    CompressedFn compressed_fn_ = nullptr;
    void* compressed_ctx_ = nullptr;

    // Decompressed-page hook (COMPRESSED → ACTIVE on fault).  A "re-warm"
    // is direct evidence the (arena, sc) bucket is hot for this page; the
    // heap uses it to drive adaptive under-packing.
    using DecompressedFn = void(*)(size_t page_idx, uint8_t arena_id,
                                   uint8_t sc, void* ctx);
    DecompressedFn decompressed_fn_ = nullptr;
    void* decompressed_ctx_ = nullptr;

    // Tier-upgrade telemetry: cumulative counts since process start.
    // tier_upgrade_attempts_: every page eligible for upgrade we visited.
    // tier_upgrade_success_:  upgrade actually wrote a smaller blob.
    std::atomic<uint64_t> tier_upgrade_attempts_{0};
    std::atomic<uint64_t> tier_upgrade_success_{0};

    // Per-page metadata (allocated from bootstrap, indexed by VmRegion page index)
    CompressedPageInfo* compressed_ = nullptr;
    std::atomic<bool>* accessed_ = nullptr;
    uint8_t* cold_count_ = nullptr;

    // Per-page tier marker for tiered recompression.  0 = no blob (page in
    // ACTIVE/COMPRESSING/etc.), 1 = fast-tier blob (LZ4 or zstd-1, eligible
    // for upgrade), 2 = deep-tier blob (zstd-9 or ZSTD_DICT, terminal).
    // Set when compressPage / recompressPage publishes a blob; checked in
    // phase2 to find upgrade candidates without re-running compression.
    enum TierLevel : uint8_t {
        kTierNone = 0,
        kTierFast = 1,
        kTierDeep = 2,
    };
    uint8_t* page_tier_ = nullptr;
    // Per-page recompression count: bumped on every COMPRESSED → ACTIVE
    // fault, decayed in phase1Range when the page settles cold.  phase2Range
    // raises the effective cold-tick floor by 2^min(rc + bucket_bias, kMaxBackoffShift).
    // Saturating uint8_t — the bucket EMA captures values above ~6 anyway.
    uint8_t* recompress_count_ = nullptr;

    // Per-(arena, size_class) recompression-rate signal. Single shared
    // table (not per-worker) so the fault handler can update it without
    // first finding the right worker. Relaxed atomics; tearing on the EMA
    // is acceptable.  Indexed by `arena * kNumClasses + size_class`.
    std::atomic<uint16_t>* bucket_rc_ema_x256_ = nullptr;  // ×256 fixed-point
    std::atomic<uint8_t>*  bucket_rc_count_ = nullptr;     // sample count, capped 64
    static constexpr size_t kBucketTableLen = kNumArenas * kNumClasses;

    // Cohort measurement (kMeasureCohorts).  Points to SmashHeap's CohortPage
    // array — opaque here to avoid circular include.  Layout matches
    // SmashHeap::CohortPage: {uint32_t first_tid, uint32_t first_ra,
    //                          uint8_t mixed_tid, uint8_t mixed_ra}.
    struct CohortPage {
        uint32_t first_tid;
        uint32_t first_ra;
        uint8_t  mixed_tid;
        uint8_t  mixed_ra;
    };
    CohortPage* cohort_pages_ = nullptr;
    size_t cohort_pages_len_ = 0;

    // ── Chunk bitmap for fast scanning ────────────────────────────────────
    // One bit per page in kChunkSize-page chunks. Rebuilt at tick start.
    // All three phases iterate only set bits via ctzll + mask clear.
    uint64_t* live_chunks_ = nullptr;
    size_t num_chunks_ = 0;

    // ── Fault handler slots ───────────────────────────────────────────────
    // Each slot has its own buffer and ZSTD DCtx to eliminate data races
    // between concurrent faulting threads and prefetch decompression.
    struct FaultSlot {
        void* buf = nullptr;
        ZSTD_DCtx* dctx = nullptr;
        std::atomic<bool> used{false};
    };
    static constexpr int kFaultSlotCount = 128;  // Enough for many concurrent faults + prefetch
    FaultSlot fault_slots_[kFaultSlotCount]{};

    int acquireFaultSlot() {
        for (int i = 0; i < kFaultSlotCount; ++i) {
            bool expected = false;
            if (fault_slots_[i].used.compare_exchange_strong(
                    expected, true, std::memory_order_acquire))
                return i;
        }
        return -1;
    }

    void releaseFaultSlot(int slot) {
        fault_slots_[slot].used.store(false, std::memory_order_release);
    }

    // ── Per-bucket sliding window stats ───────────────────────────────────
    // A bucket is (arena_id, size_class) per worker.  Each bucket tracks
    // observed compression ratio AND observed compression time per profile
    // (fast/deep), so the ROI model can compute cost/benefit from data the
    // workload has actually produced rather than from synthetic calibration
    // of zeros/random pages that may not resemble real allocator output.
    struct SizeClassStats {
        static constexpr int kWindow = 64;
        static constexpr int kTiers = 2;   // 0 = fast tier, 1 = deep tier
        // Ratio sliding window (0-255, mapping to 0-100% bytes saved).
        uint8_t ratios[kWindow]{};
        uint8_t head = 0, count = 0;
        uint16_t sum = 0;
        // Observed compression time EMA per tier, in microseconds × 16
        // (fixed-point to retain sub-microsecond precision without floats).
        uint32_t cost_ema_x16[kTiers]{};
        uint8_t  cost_count[kTiers]{};

        // UCB1-Tuned (Auer/Cesa-Bianchi/Fischer 2002) per-arm state for
        // tier selection.  Reward is bytes-saved-per-microsecond, clamped
        // to [0, kUcbRewardMaxBytesPerUs] and normalized to [0,1].  Mean
        // and M2 (sum of squared deviations) are updated via Welford.
        uint32_t arm_pulls[kTiers]{};
        double   arm_mean[kTiers]{};
        double   arm_m2[kTiers]{};
        // Per-bucket selection counter.  Used by SMASH_UCB_FORCE_DEEP_EVERY
        // to cycle deep-tier pulls without synchronizing the forcing across
        // buckets (which would create a tick-aligned CPU spike).
        uint32_t selections = 0;

        void record(size_t comp_size, size_t orig_size) {
            uint8_t r = 0;
            if (orig_size > 0 && comp_size < orig_size)
                r = static_cast<uint8_t>(255 * (orig_size - comp_size) / orig_size);
            if (count == kWindow) {
                sum -= ratios[head];
            } else {
                count++;
            }
            ratios[head] = r;
            sum += r;
            head = static_cast<uint8_t>((head + 1) % kWindow);
        }

        // Record compression time observed for a page compressed with the
        // given tier (0 = fast, 1 = deep).  EMA with α = 1/4.
        void recordCost(int tier, uint32_t elapsed_us) {
            if (tier < 0 || tier >= kTiers) return;
            uint32_t sample_x16 = elapsed_us * 16;
            if (cost_count[tier] == 0)
                cost_ema_x16[tier] = sample_x16;
            else
                cost_ema_x16[tier] += (static_cast<int32_t>(sample_x16) -
                    static_cast<int32_t>(cost_ema_x16[tier])) / 4;
            if (cost_count[tier] < 64) cost_count[tier]++;
        }

        // Observed compression microseconds for the given tier.  Returns 0
        // when fewer than 8 samples exist; callers should fall back to the
        // profile's calibrated throughput in that case.
        uint32_t observedCostUs(int tier) const {
            if (tier < 0 || tier >= kTiers) return 0;
            return cost_count[tier] >= 8
                ? (cost_ema_x16[tier] + 8) / 16 : 0;
        }

        // UCB reward update.  reward must be in [0,1].  Uses Welford so the
        // posterior mean and variance stay numerically stable as pulls grow.
        void recordReward(int tier, double reward) {
            if (tier < 0 || tier >= kTiers) return;
            if (reward < 0.0) reward = 0.0;
            if (reward > 1.0) reward = 1.0;
            uint32_t n = ++arm_pulls[tier];
            double delta  = reward - arm_mean[tier];
            arm_mean[tier] += delta / static_cast<double>(n);
            double delta2 = reward - arm_mean[tier];
            arm_m2[tier] += delta * delta2;
        }
    };

    // ── Per-arena UCB arm aggregate ───────────────────────────────────────
    // Aggregates (pulls, mean, m2) across all size classes within an arena
    // for each tier.  Used as a warm-start prior when a per-bucket arm
    // hasn't accumulated min_pulls yet — the arena aggregate is a
    // meaningfully better estimate than the uniform prior because arena
    // routing already groups call-sites with similar allocation patterns.
    struct ArenaArmStats {
        static constexpr int kTiers = SizeClassStats::kTiers;
        uint32_t pulls[kTiers]{};
        double   mean[kTiers]{};
        double   m2[kTiers]{};
        void recordReward(int tier, double reward) {
            if (tier < 0 || tier >= kTiers) return;
            if (reward < 0.0) reward = 0.0;
            if (reward > 1.0) reward = 1.0;
            uint32_t n = ++pulls[tier];
            double delta  = reward - mean[tier];
            mean[tier] += delta / static_cast<double>(n);
            double delta2 = reward - mean[tier];
            m2[tier] += delta * delta2;
        }
    };

    // ── Per-worker compression state ──────────────────────────────────────
    // Each worker has its own LZ4 state, ZSTD CCtx, and scratch buffers
    // so Phase 2 (compression) is embarrassingly parallel.
    struct CompressWorker {
        void* lz4_state = nullptr;
        ZSTD_CCtx* zstd_cctx = nullptr;
        void* page_buf = nullptr;
        void* compress_buf = nullptr;
        void* compress_buf2 = nullptr;  // second buffer for dict try-both experiment
        // ROI stats indexed by (arena_id, size_class).  Arena routing
        // produces structurally-homogeneous pages; aggregating stats across
        // arenas would wash out that homogeneity, so each arena gets its
        // own sliding window per size class.  Index via statsIndex(arena,sc).
        SizeClassStats sc_stats[kNumArenas * kNumClasses]{};
        // Per-arena UCB aggregate used as warm-start prior under
        // SMASH_UCB_WARMSTART=1.  Indexed by arena_id.
        ArenaArmStats arena_arm[kNumArenas]{};
        size_t range_start = 0, range_end = 0;

        // Compress using worker's own contexts, shared engine's dictionaries
        size_t compress(const void* src, void* dst, size_t src_size,
                       size_t dst_capacity, CompressAlgo algo,
                       uint8_t size_class, int zstd_level,
                       CompressEngine* shared_engine) {
            switch (algo) {
            case CompressAlgo::LZ4: {
                int result = LZ4_compress_fast_extState(
                    lz4_state,
                    static_cast<const char*>(src),
                    static_cast<char*>(dst),
                    static_cast<int>(src_size),
                    static_cast<int>(dst_capacity),
                    1);
                return (result > 0) ? static_cast<size_t>(result) : 0;
            }
            case CompressAlgo::ZSTD: {
                size_t result = ZSTD_compressCCtx(
                    zstd_cctx,
                    dst, dst_capacity,
                    src, src_size,
                    zstd_level);
                return ZSTD_isError(result) ? 0 : result;
            }
            case CompressAlgo::ZSTD_DICT: {
                ZSTD_CDict* cdict = shared_engine->getCDict(size_class);
                if (cdict) {
                    size_t result = ZSTD_compress_usingCDict(
                        zstd_cctx,
                        dst, dst_capacity,
                        src, src_size,
                        cdict);
                    return ZSTD_isError(result) ? 0 : result;
                }
                // Fallback to zstd deep level without dict
                size_t result = ZSTD_compressCCtx(
                    zstd_cctx,
                    dst, dst_capacity,
                    src, src_size,
                    kZstdDeepLevel);
                return ZSTD_isError(result) ? 0 : result;
            }
            default:
                return 0;
            }
        }
    };

    CompressWorker workers_[kMaxCompressorWorkers];

    // ── Dictionary training (coordinator thread) ──────────────────────────
    struct DictTrainState {
        char* sample_data = nullptr;
        size_t* sample_sizes = nullptr;
        std::atomic<uint16_t> num_samples{0};
        bool trained = false;
        bool allocated = false;
        Spinlock alloc_lock;
    };
    DictTrainState dict_train_[kNumClasses]{};

    // ── Dict experiment counters (try-both) ───────────────────────────────
    std::atomic<uint64_t> dict_win_count_{0};   // dict smaller than plain zstd
    std::atomic<uint64_t> dict_loss_count_{0};   // dict larger than plain zstd
    std::atomic<uint64_t> dict_tie_count_{0};    // equal size
    std::atomic<int64_t>  dict_total_delta_{0};  // sum of (plain_size - dict_size), positive = dict wins

    // ── Thread management ─────────────────────────────────────────────────
    pthread_t coord_thread_{};
    static constexpr int kMaxHelpers = kMaxCompressorWorkers > 1 ? kMaxCompressorWorkers - 1 : 1;
    pthread_t helper_threads_[kMaxHelpers]{};
    std::atomic<bool> running_{false};

    // Fork coordination. paused_ is set by the atfork prepare handler;
    // in_tick_ is set while the coordinator is mid-tick (holding page
    // locks etc.). prepare waits for in_tick_=0 before letting fork()
    // proceed, so the child never inherits a half-done tick.
    std::atomic<bool> paused_{false};
    std::atomic<bool> in_tick_{false};

public:
    // Pause the coordinator (called from a pthread_atfork prepare handler in
    // the parent before fork()). Wait briefly for any in-flight tick to
    // finish. We cap the wait so a stuck tick can never deadlock fork().
    void pauseForFork() {
        paused_.store(true, std::memory_order_release);
        // Coordinator wakes on its 10 ms tick boundary. Give it a few of
        // those (~50 ms total) to drain in_tick_ before we let fork proceed.
        for (int i = 0; i < 50 && in_tick_.load(std::memory_order_acquire); ++i)
            usleep(1000);
    }

    // Resume after fork() in the parent.
    void resumeAfterFork() {
        paused_.store(false, std::memory_order_release);
    }

    // Reset thread-management bookkeeping in a fork()'d child. The actual
    // pthreads themselves are already gone — Linux fork() only clones the
    // calling thread — but the parent's pthread_t handles are still in our
    // arrays and `running_`/`helpers_created_` still claim threads exist.
    // Without this, the singleton's startCompression() CAS fails (because
    // compression_started_ is true) and the child runs with no compressor.
    void resetForFork() {
        running_.store(false, std::memory_order_release);
        paused_.store(false, std::memory_order_release);
        in_tick_.store(false, std::memory_order_release);
        coord_thread_ = pthread_t{};
        for (int i = 0; i < kMaxHelpers; ++i) {
            helper_threads_[i] = pthread_t{};
            helper_done_gen_[i].store(0, std::memory_order_relaxed);
        }
        helpers_created_ = 0;
        active_workers_ = kCompressorWorkers;
        work_gen_.store(0, std::memory_order_release);
        current_phase_ = 0;
    }
private:

    // Work dispatch: coordinator increments work_gen_ to signal helpers.
    // Helpers compare against their last-seen gen to detect new work.
    std::atomic<uint64_t> work_gen_{0};
    int current_phase_ = 0;  // 1=access, 2=compress, 3=monitor
    std::atomic<uint64_t> helper_done_gen_[kMaxHelpers]{};

    // ── Adaptive worker scaling (Little's Law) ─────────────────────────
    //
    // By Little's Law, the average number of items in a stable queueing
    // system is L = λ · W, where λ is the arrival rate and W is the mean
    // service time.  For the compression queue:
    //
    //   λ  = pages becoming eligible for compression per tick
    //   μ  = pages one worker compresses per tick  (service rate = 1/W)
    //
    // To keep the queue drained (L → 0), total service rate must meet
    // arrival rate: N · μ ≥ λ.  Solving for the minimum worker count:
    //
    //   N_needed = ⌈λ / μ⌉
    //
    // Both λ and μ are measured each tick and smoothed with EMAs.
    // Workers are pre-allocated up to kMaxCompressorWorkers; helper threads
    // are lazily created on first scale-up and parked when not needed.
    int active_workers_{kCompressorWorkers};  // current active count (runtime)
    int helpers_created_ = 0;  // how many helper threads have been pthread_create'd

    // Per-worker counters for the current tick (reset each tick)
    std::atomic<uint32_t> worker_pages_eligible_[kMaxCompressorWorkers]{};
    std::atomic<uint32_t> worker_pages_compressed_[kMaxCompressorWorkers]{};

    // Exponential moving averages (fixed-point: multiply by 256 to avoid float)
    // EMA update: ema = ema + (sample - ema) / 4   (α = 1/4, ~4-tick response)
    uint32_t lambda_ema_ = 0;  // pages eligible per tick (× 256)
    uint32_t mu_ema_ = 256;    // pages compressed per worker per tick (× 256, init=1 to avoid /0)

    // ── Helper methods ────────────────────────────────────────────────────

    uint8_t lookupSizeClass(size_t page_idx) {
        if (!page_map_) return 0;
        void* addr = vm_->pageAddress(page_idx);
        Span* span = page_map_->get(reinterpret_cast<uintptr_t>(addr));
        if (!span) return 0;
        return span->size_class;
    }

    // Read both arena_id and size_class in a single page-map lookup.
    // Returns false on unmapped pages.
    bool lookupSpanInfo(size_t page_idx, uint8_t& arena_id, uint8_t& sc) {
        arena_id = 0; sc = 0;
        if (!page_map_) return false;
        void* addr = vm_->pageAddress(page_idx);
        Span* span = page_map_->get(reinterpret_cast<uintptr_t>(addr));
        if (!span) return false;
        arena_id = span->arena_id;
        sc = span->size_class;
        return true;
    }

    // Index into the per-(arena, size-class) ROI stats array.
    static inline size_t statsIndex(uint8_t arena_id, uint8_t sc) {
        return static_cast<size_t>(arena_id) * kNumClasses +
               static_cast<size_t>(sc);
    }

    bool sameSpan(size_t page_a, size_t page_b) {
        if (!page_map_) return false;
        Span* sa = page_map_->get(reinterpret_cast<uintptr_t>(vm_->pageAddress(page_a)));
        Span* sb = page_map_->get(reinterpret_cast<uintptr_t>(vm_->pageAddress(page_b)));
        return sa && sb && sa == sb;
    }

    // Non-temporal zero: avoids cache pollution when zeroing cold page data.
    // All object slots are 16-byte aligned with sizes that are multiples of 16.
    static void ntZeroMemory(void* dst, size_t size) {
#if defined(__clang__)
        auto* p = static_cast<uint64_t*>(dst);
        size_t n = size / 8;  // always even since size % 16 == 0
        for (size_t i = 0; i < n; i += 2) {
            __builtin_nontemporal_store(static_cast<uint64_t>(0), &p[i]);
            __builtin_nontemporal_store(static_cast<uint64_t>(0), &p[i + 1]);
        }
#elif defined(__x86_64__) || defined(_M_X64)
        auto* p = static_cast<uint64_t*>(dst);
        size_t n = size / 8;
        for (size_t i = 0; i < n; i += 2) {
            _mm_stream_si64(reinterpret_cast<long long*>(&p[i]), 0);
            _mm_stream_si64(reinterpret_cast<long long*>(&p[i + 1]), 0);
        }
        _mm_sfence();
#else
        std::memset(dst, 0, size);
#endif
    }

    // Zero freed slots in the scratch buffer before compression.
    // All freed slots are zeroed here (deferred from free() to avoid critical-path overhead).
    void zeroFreeSlots(void* page_buf, size_t page_idx) {
        if (!page_map_) return;
        void* page_addr = vm_->pageAddress(page_idx);
        Span* span = page_map_->get(reinterpret_cast<uintptr_t>(page_addr));
        if (!span || span->is_large || span->object_size == 0) return;

        uintptr_t span_base = reinterpret_cast<uintptr_t>(span->base);
        uintptr_t page_start = reinterpret_cast<uintptr_t>(page_addr);
        size_t obj_size = span->object_size;

        size_t page_off = page_start - span_base;
        size_t page_end_off = page_off + kPageSize;

        // First slot that could overlap this page
        size_t first_slot = page_off / obj_size;

        for (size_t slot = first_slot; slot < span->object_count; ++slot) {
            size_t slot_start = slot * obj_size;
            if (slot_start >= page_end_off) break;
            size_t slot_end = slot_start + obj_size;

            // Check if free (bitmap bit 1 = free)
            if (!(span->bitmap[slot / 64] & (1ULL << (slot % 64)))) continue;

            // Overlap with this page
            size_t zero_start = (slot_start > page_off) ? slot_start : page_off;
            size_t zero_end = (slot_end < page_end_off) ? slot_end : page_end_off;
            if (zero_start >= zero_end) continue;

            size_t buf_offset = zero_start - page_off;
            ntZeroMemory(static_cast<char*>(page_buf) + buf_offset, zero_end - zero_start);
        }
    }

    // ── Chunk bitmap management ───────────────────────────────────────────

    void rebuildChunkBitmap(size_t committed) {
        size_t nchunks = (committed + kChunkSize - 1) / kChunkSize;
        for (size_t c = 0; c < nchunks; ++c) {
            uint64_t mask = 0;
            size_t base = c * kChunkSize;
            size_t end = base + kChunkSize;
            if (end > committed) end = committed;
            for (size_t i = base; i < end; ++i) {
                if (states_->get(i) != PageState::EMPTY)
                    mask |= (1ULL << (i - base));
            }
            live_chunks_[c] = mask;
        }
    }

    // Iterate live pages in [start, end) and call fn(page_idx)
    template<typename Fn>
    void forEachLivePage(size_t start, size_t end, Fn&& fn) {
#ifdef SMASH_ABLATION_NO_CHUNK_BITMAP
        // Linear scan fallback (ablation: measure chunk bitmap contribution)
        for (size_t i = start; i < end; ++i) {
            if (states_->get(i) != PageState::EMPTY)
                fn(i);
        }
#else
        size_t start_chunk = start / kChunkSize;
        size_t end_chunk = (end + kChunkSize - 1) / kChunkSize;
        for (size_t c = start_chunk; c < end_chunk; ++c) {
            uint64_t mask = live_chunks_[c];
            if (mask == 0) continue;
            size_t base = c * kChunkSize;
            while (mask) {
                int bit = __builtin_ctzll(mask);
                size_t idx = base + bit;
                if (idx >= start && idx < end)
                    fn(idx);
                mask &= mask - 1;  // clear lowest set bit
            }
        }
#endif
    }

    // ── Phase implementations ─────────────────────────────────────────────

    // Phase 1: Process access bits and update cold counts
    void phase1Range(size_t start, size_t end) {
        forEachLivePage(start, end, [&](size_t i) {
            PageState st = states_->get(i);
            if (st == PageState::ACTIVE || st == PageState::ACTIVE_MONITORING) {
                if (accessed_[i].load(std::memory_order_relaxed)) {
                    cold_count_[i] = 0;
                    accessed_[i].store(false, std::memory_order_relaxed);
                } else {
                    if (cold_count_[i] < 255) cold_count_[i]++;
                    decayRecompressCount(i);
                }
            } else if (st == PageState::COMPRESSED) {
                // Keep counting for compressed pages so zstd upgrade can trigger
                if (cold_count_[i] < 255) cold_count_[i]++;
                decayRecompressCount(i);
            }
        });
    }

    // Slowly decay the per-page recompression count once a page settles into
    // a long cold streak. We trigger one decrement every kRcDecayColdTicks
    // ticks of inactivity (edge-detect on cold_count_), and a hard reset
    // when cold_count_ saturates — by that point the page has been quiet
    // for ~255 ticks and any prior thrash history is stale.
    inline void decayRecompressCount(size_t i) {
        uint8_t cc = cold_count_[i];
        if (cc == 255) {
            recompress_count_[i] = 0;
        } else if (cc >= kRcDecayColdTicks
                   && cc % kRcDecayColdTicks == 0
                   && recompress_count_[i] > 0) {
            recompress_count_[i]--;
        }
    }

    // Phase 2: Compress cold pages
    //
    // Two-level monitoring: Phase 3 sets pages to PROT_READ, which detects
    // writes but not reads.  A read-hot page (e.g., YCSB Workload B: 95%
    // reads) would appear cold and get compressed, only to be immediately
    // decompressed on the next read — wasteful churn.
    //
    // To avoid this, when a page first reaches the cold threshold we
    // escalate it from PROT_READ to PROT_NONE (deep monitoring) instead
    // of compressing.  Under PROT_NONE any access (read or write) triggers
    // the fault handler, which sets accessed_[]=true.  If the page
    // survives one full tick at PROT_NONE without any access, it is truly
    // cold and Phase 2 compresses it on the next tick.
    void phase2Range(int worker_id, size_t start, size_t end) {
        const ROIConfig& cfg = ROIConfig::instance();
        uint32_t floor = cfg.cold_ticks_floor;
        bool backoff_enabled = cfg.recompress_backoff;

        // No CPU-pressure multiplier and no per-tick compression budget.
        // The right signal for "don't compress this page yet" isn't
        // CPU saturation — it's "the program is still allocating into
        // this page." Span::allocate() resets cold_count_ for the page
        // it just handed out a chunk on, so a page being filled simply
        // never reaches the floor. Recompression-thrash is handled
        // independently by the per-page rc backoff below.
        forEachLivePage(start, end, [&](size_t i) {
            if (cold_count_[i] < floor) return;
            PageState st = states_->get(i);

            // Tiered recompression: COMPRESSED pages at the LZ4 tier
            // (initial fast tier) that have stayed cold long enough get
            // upgraded to zstd-9 for a better ratio.  cold_count_ keeps
            // incrementing for COMPRESSED pages in phase1Range, so this
            // threshold is reliable.
            if constexpr (kTieredRecompression) {
                if (st == PageState::COMPRESSED &&
                    cold_count_[i] >= cfg.very_cold_ticks &&
                    page_tier_ && page_tier_[i] == kTierFast) {
                    tier_upgrade_attempts_.fetch_add(1,
                        std::memory_order_relaxed);
                    // Pick ZSTD_DICT if a dict is trained for this size
                    // class, else plain ZSTD at deep level.
                    uint8_t sc = lookupSizeClass(i);
                    CompressAlgo target = CompressAlgo::ZSTD;
                    if (engine_ && sc < kNumClasses &&
                        engine_->hasDictionary(sc)) {
                        target = CompressAlgo::ZSTD_DICT;
                    }
                    if (recompressPage(i, workers_[worker_id], target,
                                       kZstdDeepLevel)) {
                        tier_upgrade_success_.fetch_add(1,
                            std::memory_order_relaxed);
                    }
                    // Regardless of upgrade outcome, this slot was visited
                    // — no further work this tick for this page.
                }
                if (st == PageState::COMPRESSED || st == PageState::COMPRESSING)
                    return;
            } else if (st != PageState::ACTIVE && st != PageState::ACTIVE_MONITORING) {
                return;
            }
            if (st != PageState::ACTIVE && st != PageState::ACTIVE_MONITORING)
                return;

            // Recompression-thrash gate. Pages that have been faulted back
            // from COMPRESSED before, or that live in a (arena, size_class)
            // bucket whose pages are doing the same, must stay idle longer
            // before becoming eligible again. Effective floor doubles per
            // accumulated recompress event, capped at kMaxBackoffShift.
            //
            // Looked up via the span so we also pick up the new-page bias
            // from the per-bucket EMA — fresh pages from a known-thrashy
            // call site inherit the wait without needing their own history.
            uint32_t eff_floor = floor;
            if (backoff_enabled) {
                uint8_t rc = recompress_count_[i];
                uint8_t bucket_bias = 0;
                if (page_map_) {
                    void* page_addr = vm_->pageAddress(i);
                    Span* sp = page_map_->get(reinterpret_cast<uintptr_t>(page_addr));
                    if (sp && !sp->is_large && sp->size_class < kNumClasses) {
                        size_t bidx = static_cast<size_t>(sp->arena_id) * kNumClasses
                                    + sp->size_class;
                        if (bidx < kBucketTableLen) {
                            // Bucket bias: a single recompress event in this
                            // (arena, size_class) bucket gives bias=1 to fresh
                            // pages in the same bucket — they get a longer
                            // initial cold window without needing to thrash
                            // themselves first. Threshold=256 ≈ "one full rc
                            // sample" with α=1/4 EMA.
                            uint16_t ema = bucket_rc_ema_x256_[bidx].load(
                                std::memory_order_relaxed);
                            if (ema >= kBucketRcBiasThreshold_x256) bucket_bias = 1;
                        }
                    }
                }
                uint32_t shift32 = static_cast<uint32_t>(rc) + bucket_bias;
                if (shift32 > kMaxBackoffShift) shift32 = kMaxBackoffShift;
                uint64_t wide_floor = static_cast<uint64_t>(floor) << shift32;
                if (wide_floor > kMaxEffectiveFloorTicks)
                    wide_floor = kMaxEffectiveFloorTicks;
                eff_floor = static_cast<uint32_t>(wide_floor);
            }

            if (cold_count_[i] == floor && st == PageState::ACTIVE_MONITORING) {
                // Always escalate to deep monitoring at the BASE floor —
                // this just changes protection, not compression. It gives
                // the compressor more accurate access info for thrashy
                // pages (which we won't compress anyway until eff_floor).
                escalateToDeepMonitoring(i);
            } else if (cold_count_[i] > floor && cold_count_[i] >= eff_floor) {
                // Page is eligible for compression — count it
                worker_pages_eligible_[worker_id].fetch_add(1, std::memory_order_relaxed);
                if (compressPage(i, workers_[worker_id])) {
                    worker_pages_compressed_[worker_id].fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // Escalate a PROT_READ-monitored page to PROT_NONE (deep monitoring).
    // Any access (read or write) will now trigger the fault handler.
    void escalateToDeepMonitoring(size_t page_idx) {
        void* page_addr = vm_->pageAddress(page_idx);
        vm::protectPages(page_addr, kPageSize, false, false);  // PROT_NONE
    }

    // Phase 3: Set up access monitoring for remaining active pages.
    //
    // Syscalls touching a transiently PROT_READ page get EFAULT instead of
    // a SIGSEGV; the wrapper's retryWithDecompress loop catches that, walks
    // the buffer pages (which DOES go through the fault handler), and
    // retries. The bounded retry budget (8) outlasts a compressor tick.
    void phase3Range(size_t start, size_t end) {
        forEachLivePage(start, end, [&](size_t i) {
            // CAS: only transition ACTIVE → ACTIVE_MONITORING.
            // Avoids TOCTOU race where releaseCompressedPages() sets
            // a page to EMPTY between our read and write — without CAS,
            // we would overwrite EMPTY with ACTIVE_MONITORING, leaking
            // the page. (Identified via TLA+ model checking.)
            if (states_->transition(i, PageState::ACTIVE,
                                       PageState::ACTIVE_MONITORING)) {
                vm::protectPages(vm_->pageAddress(i), kPageSize, true, false);
            }
        });
    }

    // ── Compress one page using worker's contexts ─────────────────────────

    bool compressPage(size_t page_idx, CompressWorker& worker) {
        locks_->lock(page_idx);

        // Verify still eligible
        PageState st = states_->get(page_idx);
        if (st != PageState::ACTIVE && st != PageState::ACTIVE_MONITORING) {
            locks_->unlock(page_idx);
            return false;
        }

        // Mark as compressing
        states_->set(page_idx, PageState::COMPRESSING);

        // ROI-based compression decision (replaces fixed shouldSkip + selectAlgorithm).
        // Stats are per (arena_id, size_class) so the ROI model sees the
        // per-origin compression ratio, consistent with the arena design.
        uint8_t arena_id = 0, sc = 0;
        bool have_span = lookupSpanInfo(page_idx, arena_id, sc);
        size_t stats_idx = statsIndex(arena_id, sc);
        uint8_t stats_count = (have_span && sc < kNumClasses)
            ? worker.sc_stats[stats_idx].count : 0;
        uint16_t stats_sum = (have_span && sc < kNumClasses)
            ? worker.sc_stats[stats_idx].sum : 0;

        if (!CompressionROI::shouldCompress(cold_count_[page_idx],
                                             stats_count, stats_sum)) {
            states_->set(page_idx, PageState::ACTIVE);
            locks_->unlock(page_idx);
            return false;
        }

        // Tier selection: either ROI cost/benefit model or UCB1-Tuned bandit.
        const auto& cfg = ROIConfig::instance();
        const AlgoProfile* profile = nullptr;
        int chosen_tier = -1;
        if (cfg.use_ucb && cfg.num_profiles > 1) {
            // Build per-arm view from this bucket's stats and the per-profile
            // min_cold_ticks gate.  UCB sees the action space directly; the
            // ROI model's calibrated cost estimates are unused.
            uint32_t pulls[4] = {0,0,0,0};
            double mean[4] = {0,0,0,0};
            double m2[4]   = {0,0,0,0};
            bool eligible[4] = {false,false,false,false};
            auto& s = worker.sc_stats[stats_idx];
            int n = cfg.num_profiles;
            if (n > SizeClassStats::kTiers) n = SizeClassStats::kTiers;
            for (int i = 0; i < n; ++i) {
                pulls[i] = s.arm_pulls[i];
                mean[i]  = s.arm_mean[i];
                m2[i]    = s.arm_m2[i];
                eligible[i] = (cold_count_[page_idx] >= cfg.profiles[i].min_cold_ticks);
            }

            // Force-deep override: every Nth selection in this bucket, if
            // the deep arm is eligible and exists, force-pick it to keep the
            // variance estimate of the deep tier live on heavy-tailed reward
            // distributions.  Cycling per-bucket avoids tick-aligned spikes.
            uint32_t sel_counter = ++s.selections;
            int deep_idx = (n > 1) ? 1 : -1;
            if (cfg.ucb_force_deep_every > 0 && deep_idx >= 0 &&
                eligible[deep_idx] &&
                (sel_counter % cfg.ucb_force_deep_every) == 0) {
                chosen_tier = deep_idx;
            } else if (cfg.ucb_warmstart && have_span) {
                // Warm-start: pass the per-arena aggregate as a prior so
                // under-explored bucket arms borrow strength from the arena
                // rather than starting from a uniform posterior.
                uint32_t pp[4] = {0,0,0,0};
                double   pm[4] = {0,0,0,0};
                double   pmm[4] = {0,0,0,0};
                const auto& a = worker.arena_arm[arena_id];
                int an = n < ArenaArmStats::kTiers ? n : ArenaArmStats::kTiers;
                for (int i = 0; i < an; ++i) {
                    pp[i] = a.pulls[i];
                    pm[i] = a.mean[i];
                    pmm[i] = a.m2[i];
                }
                chosen_tier = CompressionROI::selectTierUCB(
                    pulls, mean, m2, eligible, n,
                    cfg.ucb_min_pulls, cfg.ucb_variant,
                    pp, pm, pmm);
            } else {
                chosen_tier = CompressionROI::selectTierUCB(
                    pulls, mean, m2, eligible, n,
                    cfg.ucb_min_pulls, cfg.ucb_variant);
            }
            if (chosen_tier >= 0) profile = &cfg.profiles[chosen_tier];
        } else {
            // Pass the bucket's observed per-tier compression costs into the
            // ROI model so it uses real workload data instead of synthetic
            // calibration when available.
            uint32_t observed_costs_us[2] = {0, 0};
            if (have_span && sc < kNumClasses) {
                observed_costs_us[0] = worker.sc_stats[stats_idx].observedCostUs(0);
                observed_costs_us[1] = worker.sc_stats[stats_idx].observedCostUs(1);
            }
            profile = CompressionROI::selectProfile(
                cold_count_[page_idx], stats_count, stats_sum,
                observed_costs_us);
            if (profile) {
                // Map back to a tier index (0=fast, 1=deep) so reward
                // attribution lines up with the UCB path even when UCB is off.
                chosen_tier = (profile == &cfg.profiles[0]) ? 0 : 1;
            }
        }
        if (!profile) {
            states_->set(page_idx, PageState::ACTIVE);
            locks_->unlock(page_idx);
            return false;
        }
        CompressAlgo algo = profile->algo;
        int zstd_level = profile->zstd_level;
        bool is_fast_tier = (zstd_level != 0 && zstd_level != kZstdDeepLevel)
                          || (algo == CompressAlgo::LZ4);
        // Prefer dictionary for deep-tier zstd if trained.
        if (algo == CompressAlgo::ZSTD && zstd_level == kZstdDeepLevel &&
            engine_ && engine_->hasDictionary(sc)) {
            algo = CompressAlgo::ZSTD_DICT;
        }

        // Make page read-only to get a consistent snapshot
        void* page_addr = vm_->pageAddress(page_idx);
        vm::protectPages(page_addr, kPageSize, true, false);  // PROT_READ

        // Copy page data into worker's scratch buffer
        __builtin_memcpy(worker.page_buf, page_addr, kPageSize);

        // Zero freed slots in scratch buffer before compression
#ifndef SMASH_ABLATION_NO_ZERO_DEFERRED
        zeroFreeSlots(worker.page_buf, page_idx);
#endif

        // Release physical backing while page is still accessible.
        // On macOS, MADV_FREE_REUSABLE requires pages to be readable;
        // on Linux, MADV_DONTNEED works regardless of protection.
        vm::decommitPages(page_addr, kPageSize);

        // Make page inaccessible
        vm::protectPages(page_addr, kPageSize, false, false);  // PROT_NONE

        // Check if we were preempted by a fault
        // TLA+ model proves this is unreachable: compressor holds the lock
        // during COMPRESSING, so no other thread can change the state.
        if (states_->get(page_idx) != PageState::COMPRESSING) {
            // Verified unreachable via TLA+ model checking and
            // 576 benchmark runs with __builtin_trap() (zero crashes).
            // Retained as defensive fallback.
            locks_->unlock(page_idx);
            return false;
        }

        // Collect sample for dictionary training (page data in worker's buf)
        collectDictSample(page_idx, sc, worker.page_buf);

        size_t max_comp = CompressEngine::maxCompressedSizeAny(kPageSize);
        size_t comp_size = 0;
        auto comp_t0 = std::chrono::steady_clock::now();

        // Try-both experiment (opt-in): when dict is selected, also try plain
        // ZSTD and keep the smaller result. Doubles compression CPU for
        // very-cold pages, so only enable for experiments.
#ifdef SMASH_DICT_TRY_BOTH
        if (algo == CompressAlgo::ZSTD_DICT && worker.compress_buf2) {
            size_t dict_size = worker.compress(worker.page_buf, worker.compress_buf,
                                               kPageSize, max_comp, CompressAlgo::ZSTD_DICT,
                                               sc, zstd_level, engine_);
            size_t plain_size = worker.compress(worker.page_buf, worker.compress_buf2,
                                                kPageSize, max_comp, CompressAlgo::ZSTD,
                                                sc, zstd_level, engine_);
            if (dict_size > 0 && plain_size > 0) {
                int64_t delta = static_cast<int64_t>(plain_size) - static_cast<int64_t>(dict_size);
                dict_total_delta_.fetch_add(delta, std::memory_order_relaxed);
                if (dict_size < plain_size) {
                    dict_win_count_.fetch_add(1, std::memory_order_relaxed);
                    comp_size = dict_size;
                } else if (dict_size > plain_size) {
                    dict_loss_count_.fetch_add(1, std::memory_order_relaxed);
                    algo = CompressAlgo::ZSTD;
                    comp_size = plain_size;
                    void* tmp = worker.compress_buf;
                    worker.compress_buf = worker.compress_buf2;
                    worker.compress_buf2 = tmp;
                } else {
                    dict_tie_count_.fetch_add(1, std::memory_order_relaxed);
                    comp_size = dict_size;
                }
            } else {
                comp_size = dict_size ? dict_size : plain_size;
                if (!dict_size && plain_size) {
                    algo = CompressAlgo::ZSTD;
                    void* tmp = worker.compress_buf;
                    worker.compress_buf = worker.compress_buf2;
                    worker.compress_buf2 = tmp;
                }
            }
        } else
#endif
        {
            comp_size = worker.compress(worker.page_buf, worker.compress_buf,
                                        kPageSize, max_comp, algo, sc,
                                        zstd_level, engine_);
        }

        double min_ratio = ROIConfig::instance().min_compress_ratio;

        // If fast tier failed the ratio gate, fall back to deep compression.
        // zstd-9 often compresses 30-50% better than zstd-1/LZ4 due to
        // better match finding and larger search window.
        if (is_fast_tier &&
            (comp_size == 0 || comp_size > static_cast<size_t>(kPageSize * min_ratio))) {
            algo = CompressAlgo::ZSTD;
            zstd_level = kZstdDeepLevel;
            is_fast_tier = false;
            comp_size = worker.compress(worker.page_buf, worker.compress_buf,
                                        kPageSize, max_comp, algo, sc,
                                        zstd_level, engine_);
        }

        auto comp_t1 = std::chrono::steady_clock::now();
        uint32_t comp_elapsed_us = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                comp_t1 - comp_t0).count());

        if (comp_size == 0 || comp_size > static_cast<size_t>(kPageSize * min_ratio)) {
            // Not worth compressing; record poor ratio and cost for the
            // tier we actually ran, then restore the page.
            if (have_span && sc < kNumClasses) {
                worker.sc_stats[stats_idx].record(
                    comp_size ? comp_size : kPageSize, kPageSize);
                int tier = is_fast_tier ? 0 : 1;
                worker.sc_stats[stats_idx].recordCost(tier, comp_elapsed_us);
                // UCB reward: attribute zero reward to the *originally chosen*
                // arm (its decision led here even after the fast→deep
                // fallback path).  Failed-to-compress = zero bytes saved.
                if (cfg.use_ucb && chosen_tier >= 0) {
                    worker.sc_stats[stats_idx].recordReward(chosen_tier, 0.0);
                    if (cfg.ucb_warmstart && arena_id < kNumArenas)
                        worker.arena_arm[arena_id].recordReward(chosen_tier, 0.0);
                }
            }
            vm::protectPages(page_addr, kPageSize, true, true);
            __builtin_memcpy(page_addr, worker.page_buf, kPageSize);
            states_->set(page_idx, PageState::ACTIVE);
            locks_->unlock(page_idx);
            return false;
        }

        // Store compressed data (sharded by page_idx)
        size_t alloc_size = 0;
        void* stored = store_->store(worker.compress_buf, comp_size, &alloc_size, page_idx);
        if (!stored) {
            vm::protectPages(page_addr, kPageSize, true, true);
            __builtin_memcpy(page_addr, worker.page_buf, kPageSize);
            states_->set(page_idx, PageState::ACTIVE);
            locks_->unlock(page_idx);
            return false;
        }

        // Record successful compression ratio and observed cost in the
        // per-(arena, sc) bucket so future ROI decisions use real data.
        if (have_span && sc < kNumClasses) {
            worker.sc_stats[stats_idx].record(comp_size, kPageSize);
            int tier = is_fast_tier ? 0 : 1;
            worker.sc_stats[stats_idx].recordCost(tier, comp_elapsed_us);
            // UCB reward: bytes_saved / compress_us, attributed to the
            // originally chosen arm.  comp_elapsed_us covers the (possibly
            // tier-fallback) work that the choice actually triggered, which
            // is the cost the bandit should learn to avoid.
            if (cfg.use_ucb && chosen_tier >= 0) {
                double r = CompressionROI::ucbReward(
                    comp_size, kPageSize, comp_elapsed_us);
                worker.sc_stats[stats_idx].recordReward(chosen_tier, r);
                if (cfg.ucb_warmstart && arena_id < kNumArenas)
                    worker.arena_arm[arena_id].recordReward(chosen_tier, r);
            }
        }

        // Record compressed page info (with algo in top 2 bits)
        compressed_[page_idx].set(stored, comp_size, alloc_size, algo);
        if (page_tier_) {
            page_tier_[page_idx] = is_fast_tier ? kTierFast : kTierDeep;
        }

        states_->set(page_idx, PageState::COMPRESSED);
        locks_->unlock(page_idx);

        // A3 feedback: notify heap that a page from (span->arena_id, sc)
        // successfully compressed. The hook accumulates evidence that the
        // originating slab is cold-biased and eventually flips routing.
        if (compressed_fn_ && page_map_) {
            Span* sp = page_map_->get(reinterpret_cast<uintptr_t>(page_addr));
            if (sp && !sp->is_large && sp->size_class < kNumClasses) {
                compressed_fn_(page_idx, sp->arena_id, sp->size_class,
                               compressed_ctx_);
            }
        }
        return true;
    }

    // Tiered recompression: take a COMPRESSED page and replace its blob
    // with one produced by a (presumably stronger) algorithm.  Used for
    // LZ4 → zstd-9 upgrades once a page has stayed cold long enough that
    // the higher ratio of zstd-9 amortizes against its decomp cost.
    //
    // Locking contract matches compressPage: hold the per-page lock from
    // before state transitions through to after the CompressedPageInfo
    // publish.  Concurrent fault handlers either acquire the lock first
    // (and we abort because state is no longer COMPRESSED) or wait
    // (and see the freshly-published new blob).
    //
    // Returns true on a successful tier upgrade.
    bool recompressPage(size_t page_idx, CompressWorker& worker,
                        CompressAlgo target_algo, int target_zstd_level) {
        locks_->lock(page_idx);

        if (states_->get(page_idx) != PageState::COMPRESSED) {
            // Faulted in by app between phase2 decision and our lock.
            locks_->unlock(page_idx);
            return false;
        }

        // Snapshot current blob info while still under lock.
        CompressedPageInfo old_info = compressed_[page_idx];
        CompressAlgo old_algo = old_info.algorithm();
        size_t old_comp_size = old_info.compressedSize();
        size_t old_alloc_size = old_info.alloc_size;
        void* old_data = old_info.data;

        // No-op if already at target tier.
        if (old_algo == target_algo && target_algo != CompressAlgo::ZSTD_DICT) {
            locks_->unlock(page_idx);
            return false;
        }

        uint8_t sc = lookupSizeClass(page_idx);
        uint8_t arena_id = 0;
        bool have_span = lookupSpanInfo(page_idx, arena_id, sc);
        size_t stats_idx = statsIndex(arena_id, sc);

        // We own the blob now; mark state COMPRESSING so concurrent
        // fault paths know the blob may be mid-flight (they wait on
        // the lock we still hold).
        states_->set(page_idx, PageState::COMPRESSING);

        // Acquire a fault slot to borrow a DCtx for decompression.  The
        // shared engine_->zstd_dctx_ isn't thread-safe across workers;
        // fault slots have per-slot DCtx that we can reuse here.
        int slot = -1;
        while ((slot = acquireFaultSlot()) < 0) {
#if defined(__x86_64__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            asm volatile("yield");
#endif
        }

        size_t decomp_size = engine_->decompressWithDCtx(
            fault_slots_[slot].dctx,
            old_data, worker.page_buf,
            old_comp_size, kPageSize,
            old_algo, sc);
        releaseFaultSlot(slot);

        if (decomp_size != kPageSize) {
            // Bad blob? Should be unreachable given prior successful compress.
            states_->set(page_idx, PageState::COMPRESSED);
            locks_->unlock(page_idx);
            return false;
        }

        // Recompress with target algo.  Reuse compress_buf as output.
        auto t0 = std::chrono::steady_clock::now();
        size_t max_comp = CompressEngine::maxCompressedSizeAny(kPageSize);
        size_t new_comp_size = worker.compress(
            worker.page_buf, worker.compress_buf,
            kPageSize, max_comp,
            target_algo, sc, target_zstd_level, engine_);
        auto t1 = std::chrono::steady_clock::now();
        uint32_t elapsed_us = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());

        // Reject if compression failed or doesn't deliver meaningful savings
        // over the existing blob.  Threshold: new blob must be ≤ 80% of old.
        // For LZ4 → zstd-9 on real workloads, expected new/old ratio is
        // 0.5 (json) to 0.8 (kv), so the gate fires only on marginal cases.
        size_t savings_gate = (old_comp_size * 4) / 5;
        if (new_comp_size == 0 || new_comp_size > savings_gate) {
            states_->set(page_idx, PageState::COMPRESSED);
            locks_->unlock(page_idx);
            // Stats: record the failed upgrade attempt's cost against the
            // target tier so the ROI model learns this bucket doesn't reward
            // deep-tier upgrades.
            if (have_span && sc < kNumClasses) {
                worker.sc_stats[stats_idx].recordCost(1, elapsed_us);
            }
            return false;
        }

        size_t new_alloc_size = 0;
        void* new_data = store_->store(worker.compress_buf, new_comp_size,
                                       &new_alloc_size, page_idx);
        if (!new_data) {
            states_->set(page_idx, PageState::COMPRESSED);
            locks_->unlock(page_idx);
            return false;
        }

        // Publish new blob.  Fault handler holds the same lock so this
        // transition is invisible to it; once we release the lock, the
        // fault handler reads compressed_[page_idx] under its own lock
        // acquisition and sees the new blob.
        compressed_[page_idx].set(new_data, new_comp_size, new_alloc_size,
                                  target_algo);
        if (page_tier_) page_tier_[page_idx] = kTierDeep;

        states_->set(page_idx, PageState::COMPRESSED);
        locks_->unlock(page_idx);

        // Safe to release old blob now: lock was held while we overwrote
        // the per-page CompressedPageInfo, so no concurrent fault path
        // can still hold a reference to old_data.
        store_->release(old_data, old_alloc_size, page_idx);

        // Stats: target-tier success record.  Tier index 1 = deep in the
        // current 2-tier scheme.
        if (have_span && sc < kNumClasses) {
            worker.sc_stats[stats_idx].record(new_comp_size, kPageSize);
            worker.sc_stats[stats_idx].recordCost(1, elapsed_us);
        }

        return true;
    }


    // ── Dictionary training ───────────────────────────────────────────────

    void collectDictSample(size_t /*page_idx*/, uint8_t sc, void* page_buf) {
        if (sc >= kNumClasses) return;
        auto& dt = dict_train_[sc];
        if (dt.trained) return;
        if (trainedDictCount() >= kMaxDictClasses) return;

        // Lazy-allocate sample buffers (double-checked locking)
        if (!dt.allocated) {
            LockGuard guard(dt.alloc_lock);
            if (!dt.allocated) {
                size_t buf_size = static_cast<size_t>(kDictTrainSamples) * kPageSize;
                dt.sample_data = static_cast<char*>(
                    BootstrapAlloc::instance().allocate(buf_size, kPageSize));
                dt.sample_sizes = bootstrapArray<size_t>(kDictTrainSamples);
                if (!dt.sample_data || !dt.sample_sizes) {
                    dt.trained = true;  // Prevent retries
                    return;
                }
                dt.allocated = true;
            }
        }

        // Atomically claim a sample slot
        uint16_t slot = dt.num_samples.fetch_add(1, std::memory_order_acq_rel);
        if (slot >= kDictTrainSamples) return;  // already full

        size_t offset = static_cast<size_t>(slot) * kPageSize;
        __builtin_memcpy(dt.sample_data + offset, page_buf, kPageSize);
        dt.sample_sizes[slot] = kPageSize;
    }

    int trainedDictCount() const {
        int n = 0;
        for (int sc = 0; sc < kNumClasses; ++sc)
            if (engine_->hasDictionary(sc)) ++n;
        return n;
    }

    void trainDictionaries() {
        if (trainedDictCount() >= kMaxDictClasses) return;

        for (int sc = 0; sc < kNumClasses; ++sc) {
            auto& dt = dict_train_[sc];
            if (dt.trained) continue;
            uint16_t samples = dt.num_samples.load(std::memory_order_acquire);
            if (samples < kDictTrainSamples) continue;
            dt.trained = engine_->trainDictionary(
                sc, dt.sample_data, dt.sample_sizes, samples);
            if (!dt.trained) dt.trained = true;
            if (trainedDictCount() >= kMaxDictClasses) return;
        }
    }

    // ── Prefetch adjacent compressed pages ────────────────────────────────

    void prefetchAdjacent(size_t page_idx) {
        size_t committed = vm_->committedPages();

        for (int delta = -kPrefetchWindow; delta <= kPrefetchWindow; ++delta) {
            if (delta == 0) continue;

            size_t adj = page_idx + static_cast<size_t>(delta);
            if (delta < 0 && static_cast<size_t>(-delta) > page_idx) continue;
            if (adj >= committed) continue;

            // Only prefetch pages in the same span
            if (!sameSpan(page_idx, adj)) continue;

            // Non-blocking lock to avoid deadlock
            if (!locks_->tryLock(adj)) continue;

            if (states_->get(adj) == PageState::COMPRESSED && compressed_[adj].data) {
                void* adj_addr = vm_->pageAddress(adj);
                CompressAlgo algo = compressed_[adj].algorithm();
                uint8_t sc = lookupSizeClass(adj);

                // Try to acquire a fault slot. If none available, skip this prefetch
                // (it's just an optimization - the page will be decompressed on demand).
                int slot = acquireFaultSlot();
                if (slot < 0) {
                    locks_->unlock(adj);
                    continue;
                }

                engine_->decompressWithDCtx(
                    fault_slots_[slot].dctx,
                    compressed_[adj].data, fault_slots_[slot].buf,
                    compressed_[adj].compressedSize(), kPageSize,
                    algo, sc);
                vm::commitPages(adj_addr, kPageSize);
                __builtin_memcpy(adj_addr, fault_slots_[slot].buf, kPageSize);
                releaseFaultSlot(slot);

                // Release compressed blob (sharded by page index)
                store_->release(compressed_[adj].data, compressed_[adj].alloc_size, adj);
                compressed_[adj] = {};

                // Restore to active
                states_->set(adj, PageState::ACTIVE);
                cold_count_[adj] = 0;
            }

            locks_->unlock(adj);
        }
    }

    // ── stdio buffer warming ──────────────────────────────────────────────

    // macOS only: DYLD interposition cannot intercept intra-libSystem calls
    // (getc_unlocked, __srget). Those paths read from FILE._bf without going
    // through our wrapper, so EFAULT-retry can't help. Re-warm stdin/stdout/
    // stderr buffer pages each tick so the kernel never sees a protected page
    // for them. On Linux, LD_PRELOAD intercepts intra-libc calls, so the
    // EFAULT-retry path covers them; this is a no-op there.
    void warmStdioBuffers() {
#ifdef __APPLE__
        FILE* streams[] = { stdin, stdout, stderr };
        for (FILE* f : streams) {
            if (!f || !f->_bf._base || f->_bf._size <= 0) continue;
            smash::vm::warmPages(f->_bf._base, f->_bf._size, vm_);
            smash::vm::warmPages(f, sizeof(FILE), vm_);
        }
#endif
    }

    // ── Parallel dispatch ─────────────────────────────────────────────────

    void dispatch(int phase) {
        // If helpers are not running (e.g., compressTick() called directly
        // in tests), fall back to single-threaded execution.
        int nw = active_workers_;
        bool helpers_active = running_.load(std::memory_order_relaxed) && nw > 1;
        if (helpers_active) {
            current_phase_ = phase;
            work_gen_.fetch_add(1, std::memory_order_release);

            // Coordinator does worker 0's range
            executePhase(phase, 0);

            // Wait for active helpers only (helpers with id >= nw-1 are parked)
            uint64_t target = work_gen_.load(std::memory_order_relaxed);
            for (int i = 0; i < nw - 1; ++i) {
                while (helper_done_gen_[i].load(std::memory_order_acquire) < target) {
                    // spin
                }
            }
        } else {
            // Single-threaded: execute all work on worker 0
            executePhase(phase, 0);
        }
    }

    void executePhase(int phase, int worker_id) {
        auto& w = workers_[worker_id];
        if (phase == 1) phase1Range(w.range_start, w.range_end);
        else if (phase == 2) phase2Range(worker_id, w.range_start, w.range_end);
        else if (phase == 3) phase3Range(w.range_start, w.range_end);
    }

    // ── Tick ──────────────────────────────────────────────────────────────

    void tick() {
        if (pre_tick_fn_) pre_tick_fn_();
        if (fault_handler_) fault_handler_->ensureInstalled();
        // SMASH_DEFER_PHASES_MS=NNN: skip Phase 2 (compress) and Phase 3
        // (monitor PROT_READ) for the first NNN ms after start(). Useful
        // for workloads that establish IPC channels at startup with
        // buffers in smash-managed pages — once those buffers are no
        // longer hot, normal compressor operation resumes. Phase 1
        // (access tracking bookkeeping) still runs.
        static const int defer_ms = []{
            const char* v = std::getenv("SMASH_DEFER_PHASES_MS");
            return v ? atoi(v) : 0;
        }();
        static const auto start_time = std::chrono::steady_clock::now();
        bool defer_phases = false;
        if (defer_ms > 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - start_time).count();
            defer_phases = (elapsed_ms < defer_ms);
        }
        // Re-claim SIGUSR2 each tick so Firefox / other runtimes that
        // also install a SIGUSR2 handler can't permanently displace ours.
        // Cheap: one sigaction-read + one branch.
        {
            struct sigaction current{};
            sigaction(SIGUSR2, nullptr, &current);
            if (current.sa_handler != &CompressorThread::sigusr1Handler) {
                struct sigaction sa{};
                sa.sa_handler = &CompressorThread::sigusr1Handler;
                sa.sa_flags = SA_RESTART;
                sigemptyset(&sa.sa_mask);
                sigaction(SIGUSR2, &sa, nullptr);
            }
        }

        // Re-warm stdio buffers each tick (macOS-only; no-op on Linux).
        // Without this, intra-libSystem getc_unlocked / __srget paths can
        // hit a protected stdin/stdout/stderr buffer and EFAULT inside libc
        // before our wrapper sees the call.
        warmStdioBuffers();

        // SMASH_DEBUG=1: print a stats line every 5 ticks (~5s at the
        // 1000ms compressor cadence — kCompressIntervalMs). Mirrors the
        // SIGUSR2 stats format so log scrapers can use one parser.
        if (s_debug_enabled_) {
            static int debug_tick_counter = 0;
            if (++debug_tick_counter >= 5) {
                debug_tick_counter = 0;
                sigusr1Handler(0);
            }
        }

        size_t committed = vm_->committedPages();
        if (committed == 0) return;

        // Rebuild chunk bitmap
        rebuildChunkBitmap(committed);

        // Reset per-worker tick counters
        int nw = active_workers_;
        for (int w = 0; w < nw; ++w) {
            worker_pages_eligible_[w].store(0, std::memory_order_relaxed);
            worker_pages_compressed_[w].store(0, std::memory_order_relaxed);
        }

        // Partition page range across active workers.
        // If helpers are not running, worker 0 gets the entire range.
        bool helpers_active = running_.load(std::memory_order_relaxed) && nw > 1;
        if (helpers_active) {
            size_t per_worker = committed / nw;
            for (int w = 0; w < nw; ++w) {
                workers_[w].range_start = w * per_worker;
                workers_[w].range_end = (w == nw - 1)
                    ? committed : (w + 1) * per_worker;
            }
        } else {
            workers_[0].range_start = 0;
            workers_[0].range_end = committed;
        }

        dispatch(1);  // Phase 1: access tracking

        if (!defer_phases) dispatch(2);  // Phase 2: compression
        // Phase 3 mprotects ACTIVE pages to PROT_READ for write-fault tracking.
        // This breaks any syscall / Mach trap that writes into a smash-managed
        // buffer it doesn't know to pin (e.g. mach_msg from CFRunLoop).
        // SMASH_NO_MONITOR=1 disables Phase 3 at runtime, trading off cold
        // detection accuracy for compatibility with such codepaths. Looked
        // up once at startup to keep the tick loop branch-free.
        static const bool no_monitor = []{
            const char* v = std::getenv("SMASH_NO_MONITOR");
            return v && v[0] == '1';
        }();
        if (!no_monitor && !defer_phases) dispatch(3);  // Phase 3: monitoring

        // ── Adaptive worker scaling ──────────────────────────────────────
        // Measure this tick's workload and throughput, update EMAs, and
        // compute the number of workers needed for next tick.
        adaptWorkerCount(nw);

        if constexpr (kMeasureCohorts) tallyCohorts(committed);

        trainDictionaries();
    }

    // Adapt active worker count using Little's Law (see class comment above).
    // CPU-pressure cap on compressor workers. Returns the maximum number of
    // workers we should run given current system load and our own process's
    // thread count, or 0 to mean "no cap."
    //
    // The compressor is background work: every worker that runs competes
    // with the application for cores. The right answer depends on what the
    // application is doing — a single-threaded app on an 8-core box can
    // afford many compressor workers; a 16-threaded app on a 4-core box
    // cannot afford any.
    //
    // Method: read the live thread count for *our process* (/proc/self/stat
    // num_threads field, or task-dir count on macOS), subtract our known
    // compressor workers to get an estimate of app threads, and reserve
    // enough cores for them. Then add a safety floor based on system-wide
    // 1-minute load average to back off when *other* processes are also
    // saturating the box.
    //
    //   app_threads = max(1, process_threads - (helpers_created_ + 1))
    //   sys_pressure = max(0, ceil(loadavg) - app_threads)  // outside contention
    //   cap = max(1, nproc - app_threads - sys_pressure)
    //
    // Cheap, cached: re-evaluated at most every kCpuPressureSampleTicks
    // ticks. Behind SMASH_CPU_PRESSURE_CAP=0 to allow ablation.
    int cpuPressureWorkerCap() {
        static const bool enabled = []{
            const char* v = std::getenv("SMASH_CPU_PRESSURE_CAP");
            return !(v && v[0] == '0');
        }();
        if (!enabled) return 0;

        static const long nproc_cached = []{
            long n = sysconf(_SC_NPROCESSORS_ONLN);
            return n > 0 ? n : 1;
        }();

        constexpr int kCpuPressureSampleTicks = 4;
        if (cpu_pressure_sample_age_ < kCpuPressureSampleTicks) {
            cpu_pressure_sample_age_++;
            return cpu_pressure_cached_cap_;
        }
        cpu_pressure_sample_age_ = 0;

        // Live thread count of our own process. /proc/self/stat field 20 is
        // num_threads, but stat parsing is finicky (the comm field can
        // contain spaces); easier to count entries in /proc/self/task.
        // On macOS, /proc isn't available → fall back to process-info.
        int proc_threads = countOwnThreads();
        if (proc_threads <= 0) {
            cpu_pressure_cached_cap_ = 0;  // can't read → no cap
            return 0;
        }

        // App threads = total minus our own compressor workers.
        int our_workers = helpers_created_ + 1;  // helpers + coordinator
        int app_threads = proc_threads - our_workers;
        if (app_threads < 1) app_threads = 1;

        // System-wide pressure not attributable to our own app.
        double loads[3] = {0.0, 0.0, 0.0};
        int sys_pressure = 0;
        int load_int = 0;
        if (getloadavg(loads, 1) >= 1) {
            load_int = static_cast<int>(loads[0] + 0.5);
            sys_pressure = load_int - app_threads;
            if (sys_pressure < 0) sys_pressure = 0;
        }

        int cap = static_cast<int>(nproc_cached) - app_threads - sys_pressure;
        if (cap < 1) cap = 1;
        cpu_pressure_cached_cap_ = cap;
        return cap;
    }

    // /proc/self/task/ entry count (Linux) or task_threads() (macOS).
    // Returns -1 on error.
    int countOwnThreads() {
#if defined(__linux__)
        DIR* d = opendir("/proc/self/task");
        if (!d) return -1;
        int count = 0;
        while (struct dirent* e = readdir(d)) {
            if (e->d_name[0] != '.') count++;
        }
        closedir(d);
        return count > 0 ? count : -1;
#elif defined(__APPLE__)
        thread_array_t threads;
        mach_msg_type_number_t n = 0;
        if (task_threads(mach_task_self(), &threads, &n) != KERN_SUCCESS)
            return -1;
        // Free the port rights we just received.
        for (mach_msg_type_number_t i = 0; i < n; ++i)
            mach_port_deallocate(mach_task_self(), threads[i]);
        vm_deallocate(mach_task_self(),
                      reinterpret_cast<vm_address_t>(threads),
                      n * sizeof(thread_t));
        return static_cast<int>(n);
#else
        return -1;
#endif
    }

    int cpu_pressure_cached_cap_ = 0;
    int cpu_pressure_sample_age_ = 1000;  // force resample on first call

    // Measures λ (cold arrival rate) and μ (per-worker service rate) each
    // tick, smooths with EMA (α = 1/4), and sets N = ⌈λ_ema / μ_ema⌉.
    void adaptWorkerCount(int nw) {
        // Sum per-worker counters
        uint32_t total_eligible = 0;
        uint32_t total_compressed = 0;
        for (int w = 0; w < nw; ++w) {
            total_eligible += worker_pages_eligible_[w].load(std::memory_order_relaxed);
            total_compressed += worker_pages_compressed_[w].load(std::memory_order_relaxed);
        }

        // Per-worker throughput (avoid division by zero)
        uint32_t mu_sample = nw > 0 ? (total_compressed + nw - 1) / nw : 0;

        // Update EMAs: ema += (sample - ema) / 4  (in fixed-point ×256)
        uint32_t lambda_sample_fp = total_eligible << 8;
        uint32_t mu_sample_fp = mu_sample << 8;

        lambda_ema_ += (static_cast<int32_t>(lambda_sample_fp) - static_cast<int32_t>(lambda_ema_)) / 4;
        // Only update μ when we have compression work (avoid decaying μ to 0
        // during idle periods, which would cause spurious scale-up)
        if (total_compressed > 0) {
            mu_ema_ += (static_cast<int32_t>(mu_sample_fp) - static_cast<int32_t>(mu_ema_)) / 4;
            if (mu_ema_ == 0) mu_ema_ = 1;  // floor to prevent division by zero
        }

        // N_needed = ⌈λ / μ⌉ (both in same fixed-point scale, so they cancel)
        int n_needed;
        if (lambda_ema_ == 0) {
            n_needed = 1;  // no work → 1 worker is enough
        } else {
            n_needed = static_cast<int>((lambda_ema_ + mu_ema_ - 1) / mu_ema_);
        }

        // Clamp to valid range
        if (n_needed < 1) n_needed = 1;
        if (n_needed > kMaxCompressorWorkers) n_needed = kMaxCompressorWorkers;

        // CPU-pressure cap. The compressor's worker threads compete with
        // application threads for cores; on a saturated machine, scaling up
        // adds context-switch latency to the very fault handlers we depend on
        // for forward progress. Cap workers at (nproc − 1) when 1-minute load
        // average ≥ nproc. This leaves at least one core for the application
        // and prevents the worst-case scenario where every core is running
        // compressor work and the app times out waiting on a fault.
        int cap = cpuPressureWorkerCap();
        if (cap > 0 && n_needed > cap) n_needed = cap;

        // Ensure helper threads exist for the new count
        // (lazily create threads on first scale-up, never destroy them)
        while (helpers_created_ < n_needed - 1) {
            int helper_id = helpers_created_;
            auto* ha = static_cast<HelperArg*>(
                BootstrapAlloc::instance().allocate(sizeof(HelperArg), alignof(HelperArg)));
            ha->self = this;
            ha->id = helper_id;
            pthread_create(&helper_threads_[helper_id], nullptr, helperEntry, ha);
            helpers_created_++;
        }

        active_workers_ = n_needed;
    }

    void tallyCohorts(size_t committed) {
        if (!cohort_pages_ || cohort_pages_len_ == 0) return;
        size_t lim = committed < cohort_pages_len_ ? committed : cohort_pages_len_;
        size_t active_total = 0, active_mixed_tid = 0, active_mixed_ra = 0;
        size_t comp_total = 0, comp_mixed_tid = 0, comp_mixed_ra = 0;
        size_t all_total = 0, all_mixed_tid = 0, all_mixed_ra = 0;
        for (size_t i = 0; i < lim; ++i) {
            auto& cp = cohort_pages_[i];
            if (cp.first_tid == 0) continue;
            all_total++;
            if (cp.mixed_tid) all_mixed_tid++;
            if (cp.mixed_ra) all_mixed_ra++;
            PageState st = states_->get(i);
            if (st == PageState::ACTIVE || st == PageState::ACTIVE_MONITORING) {
                active_total++;
                if (cp.mixed_tid) active_mixed_tid++;
                if (cp.mixed_ra) active_mixed_ra++;
            } else if (st == PageState::COMPRESSED) {
                comp_total++;
                if (cp.mixed_tid) comp_mixed_tid++;
                if (cp.mixed_ra) comp_mixed_ra++;
            }
        }
        auto pct = [](size_t n, size_t d) -> double {
            return d > 0 ? 100.0 * n / d : 0.0;
        };
        fprintf(stderr,
            "[cohort] committed=%zu stamped=%zu  "
            "all=%zu mixed_tid=%zu(%.1f%%) mixed_ra=%zu(%.1f%%)  "
            "active=%zu mixed_tid=%zu(%.1f%%) mixed_ra=%zu(%.1f%%)  "
            "compressed=%zu mixed_tid=%zu(%.1f%%) mixed_ra=%zu(%.1f%%)\n",
            committed, all_total,
            all_total, all_mixed_tid, pct(all_mixed_tid, all_total),
            all_mixed_ra, pct(all_mixed_ra, all_total),
            active_total, active_mixed_tid, pct(active_mixed_tid, active_total),
            active_mixed_ra, pct(active_mixed_ra, active_total),
            comp_total, comp_mixed_tid, pct(comp_mixed_tid, comp_total),
            comp_mixed_ra, pct(comp_mixed_ra, comp_total));
    }

    // ── Thread entry points ───────────────────────────────────────────────

    static void* coordEntry(void* arg) {
        auto* self = static_cast<CompressorThread*>(arg);
        while (self->running_.load(std::memory_order_relaxed)) {
            usleep(kCompressIntervalMs * 1000);
            if (!self->running_.load(std::memory_order_relaxed)) break;
            // Skip the tick if a fork is in progress. The atfork prepare
            // handler set paused_ and is waiting for in_tick_ to be 0;
            // entering tick() now would race the impending fork().
            if (self->paused_.load(std::memory_order_acquire)) continue;
            self->in_tick_.store(true, std::memory_order_release);
            self->tick();
            self->in_tick_.store(false, std::memory_order_release);
        }
        return nullptr;
    }

    struct HelperArg {
        CompressorThread* self;
        int id;  // 0-based helper index (worker index = id + 1)
    };

    static void* helperEntry(void* arg) {
        auto* ha = static_cast<HelperArg*>(arg);
        auto* self = ha->self;
        int helper_id = ha->id;
        int worker_id = helper_id + 1;
        uint64_t last_gen = 0;

        while (self->running_.load(std::memory_order_relaxed)) {
            uint64_t gen = self->work_gen_.load(std::memory_order_acquire);
            if (gen == last_gen) {
                usleep(100);  // brief sleep while waiting for work
                continue;
            }
            last_gen = gen;

            // Only execute if this worker is active; otherwise just ACK
            // so the coordinator's dispatch() doesn't block on parked helpers.
            if (worker_id < self->active_workers_) {
                self->executePhase(self->current_phase_, worker_id);
            }
            self->helper_done_gen_[helper_id].store(gen, std::memory_order_release);
        }
        return nullptr;
    }

public:
    // Pointer to the per-page cold-tick counter array, valid after init().
    // Slab::allocate exposes this to Span so freshly-allocated chunks reset
    // the cold counter for their containing page — pages still receiving
    // allocations should never be eligible for compression.
    uint8_t* coldCounts() const { return cold_count_; }

    void init(VmRegion* vm, PageStateTable* states, PageLockTable* locks,
              CompressStore* store, CompressEngine* engine,
              PageMap* page_map = nullptr,
              vm::FaultHandler* fault_handler = nullptr) {
        vm_ = vm;
        states_ = states;
        locks_ = locks;
        store_ = store;
        engine_ = engine;
        page_map_ = page_map;
        fault_handler_ = fault_handler;
        pre_tick_fn_ = nullptr;

        size_t max_pages = vm->totalPages();
        compressed_ = bootstrapArray<CompressedPageInfo>(max_pages);
        accessed_ = bootstrapArray<std::atomic<bool>>(max_pages);
        cold_count_ = bootstrapArray<uint8_t>(max_pages);
        recompress_count_ = bootstrapArray<uint8_t>(max_pages);
        page_tier_ = bootstrapArray<uint8_t>(max_pages);

        // Per-bucket recompression EMA table (kNumArenas × kNumClasses entries).
        bucket_rc_ema_x256_ = bootstrapArray<std::atomic<uint16_t>>(kBucketTableLen);
        bucket_rc_count_    = bootstrapArray<std::atomic<uint8_t>>(kBucketTableLen);

        // Chunk bitmap
        num_chunks_ = (max_pages + kChunkSize - 1) / kChunkSize;
        live_chunks_ = bootstrapArray<uint64_t>(num_chunks_);

        // Pre-allocate per-worker state
        size_t max_comp = CompressEngine::maxCompressedSizeAny(kPageSize);
        ZSTD_customMem custom_mem = {
            [](void*, size_t sz) -> void* {
                return BootstrapAlloc::instance().allocate(sz, 16);
            },
            [](void*, void*) {},
            nullptr
        };

        // Pre-allocate state for all possible workers (adaptive scaling may
        // activate up to kMaxCompressorWorkers at runtime).
        for (int w = 0; w < kMaxCompressorWorkers; ++w) {
            auto& worker = workers_[w];
            worker.page_buf = BootstrapAlloc::instance().allocate(kPageSize, kPageSize);
            worker.compress_buf = BootstrapAlloc::instance().allocate(max_comp, 16);
            worker.compress_buf2 = BootstrapAlloc::instance().allocate(max_comp, 16);
            worker.lz4_state = BootstrapAlloc::instance().allocate(
                static_cast<size_t>(LZ4_sizeofState()), 16);
            worker.zstd_cctx = ZSTD_createCCtx_advanced(custom_mem);
        }

        // Pre-allocate fault handler slots with per-slot DCtx
        for (int i = 0; i < kFaultSlotCount; ++i) {
            fault_slots_[i].buf = BootstrapAlloc::instance().allocate(kPageSize, kPageSize);
            fault_slots_[i].dctx = ZSTD_createDCtx_advanced(custom_mem);
        }

        // Initialize ROI model (auto-calibrate throughput, read env vars)
        ROIConfig::instance().init(engine_);
    }

    void setPreTickCallback(PreTickFn fn) { pre_tick_fn_ = fn; }

    // A3: feedback hook invoked once per successful page compression
    // with the originating span's arena_id and size_class.
    void setCompressedCallback(CompressedFn fn, void* ctx) {
        compressed_fn_ = fn;
        compressed_ctx_ = ctx;
    }

    void setDecompressedCallback(DecompressedFn fn, void* ctx) {
        decompressed_fn_ = fn;
        decompressed_ctx_ = ctx;
    }

    void setCohortData(void* pages, size_t len) {
        cohort_pages_ = static_cast<CohortPage*>(pages);
        cohort_pages_len_ = len;
    }

    void start() {
        running_.store(true, std::memory_order_release);

        // SIGUSR2 stats dump: walk page-state table, write a one-line summary
        // to stderr. Lets us observe whether smash is actually compressing
        // anything during a Firefox run without rebuilding. Use SIGUSR2 (not
        // SIGUSR1) because Firefox installs a SIGUSR1 handler of its own.
        s_stats_instance_ = this;
        struct sigaction sa{};
        sa.sa_handler = &CompressorThread::sigusr1Handler;
        sa.sa_flags = SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR2, &sa, nullptr);

        // SMASH_STATS=1: also emit a stats line on every normal process
        // exit. atexit handlers are inherited across fork(), so each child
        // of a multi-process app (Firefox parent + content + GPU + …)
        // prints its own line as it shuts down. _exit() and SIGKILL skip
        // atexit by design.
        const char* stats_env = std::getenv("SMASH_STATS");
        if (stats_env && stats_env[0] == '1') {
            std::atexit([]() {
                if (s_stats_instance_) sigusr1Handler(0);
            });
        }

        // SMASH_DEBUG=1: emit a banner now (compressor came up) and a
        // stats line every Nth tick during the run. Distinct from
        // SMASH_STATS=1 (atexit-only) — SMASH_DEBUG is for watching
        // activity live without having to chase PIDs and send SIGUSR2.
        const char* dbg_env = std::getenv("SMASH_DEBUG");
        s_debug_enabled_ = dbg_env && dbg_env[0] == '1';
        if (s_debug_enabled_) {
            char ts[32] = {};
            vm::formatTimestamp(ts, sizeof(ts));
            char buf[200];
            int n = snprintf(buf, sizeof(buf),
                "[smash debug] [%s] compressor start pid=%d workers=%d\n",
                ts, (int)getpid(), kCompressorWorkers);
            if (n > 0) (void)!write(2, buf, (size_t)n);
        }

        // Start initial helper threads (adaptive scaling may create more later)
        int initial_helpers = kCompressorWorkers > 1 ? kCompressorWorkers - 1 : 0;
        for (int i = 0; i < initial_helpers; ++i) {
            auto* ha = static_cast<HelperArg*>(
                BootstrapAlloc::instance().allocate(sizeof(HelperArg), alignof(HelperArg)));
            ha->self = this;
            ha->id = i;
            pthread_create(&helper_threads_[i], nullptr, helperEntry, ha);
        }
        helpers_created_ = initial_helpers;

        // Start coordinator thread
        pthread_create(&coord_thread_, nullptr, coordEntry, this);
    }

    static inline CompressorThread* s_stats_instance_ = nullptr;
    static inline bool s_debug_enabled_ = false;

    static void sigusr1Handler(int) {
        auto* self = s_stats_instance_;
        if (!self || !self->states_ || !self->vm_) return;
        size_t empty = 0, active = 0, monitor = 0, compressing = 0,
               compressed = 0, total = self->vm_->committedPages();
        for (size_t i = 0; i < total; ++i) {
            switch (self->states_->get(i)) {
            case PageState::EMPTY: ++empty; break;
            case PageState::ACTIVE: ++active; break;
            case PageState::ACTIVE_MONITORING: ++monitor; break;
            case PageState::COMPRESSING: ++compressing; break;
            case PageState::COMPRESSED: ++compressed; break;
            }
        }
        char ts[32] = {};
        vm::formatTimestamp(ts, sizeof(ts));
        uint64_t tier_attempts = self->tier_upgrade_attempts_.load(
            std::memory_order_relaxed);
        uint64_t tier_success = self->tier_upgrade_success_.load(
            std::memory_order_relaxed);
        char buf[384];
        int n = snprintf(buf, sizeof(buf),
            "[smash stats] [%s] pid=%d committed=%zu  active=%zu  monitor=%zu"
            "  compressing=%zu  compressed=%zu  empty=%zu"
            "  tier_up=%llu/%llu\n",
            ts, (int)getpid(), total, active, monitor, compressing, compressed,
            empty,
            (unsigned long long)tier_success,
            (unsigned long long)tier_attempts);
        // Cast to void to silence -Wunused-result on glibc (write is
        // marked __wur there). We're inside a signal handler — there's
        // no useful recovery if write() short-returns.
        if (n > 0) (void)!write(2, buf, (size_t)n);
    }

    void stop() {
        if (!running_.load(std::memory_order_relaxed)) return;
        running_.store(false, std::memory_order_release);
        pthread_join(coord_thread_, nullptr);
        for (int i = 0; i < helpers_created_; ++i)
            pthread_join(helper_threads_[i], nullptr);
    }

    // Manually trigger one compression tick (for testing/manual control)
    void compressTick() { tick(); }

    // Dict experiment counters (try-both)
    uint64_t dictWinCount() const { return dict_win_count_.load(std::memory_order_relaxed); }
    uint64_t dictLossCount() const { return dict_loss_count_.load(std::memory_order_relaxed); }
    uint64_t dictTieCount() const { return dict_tie_count_.load(std::memory_order_relaxed); }
    int64_t dictTotalDelta() const { return dict_total_delta_.load(std::memory_order_relaxed); }

    int dictsTrainedCount() const {
        int count = 0;
        for (int sc = 0; sc < kNumClasses; ++sc)
            if (dict_train_[sc].trained) ++count;
        return count;
    }

    // Called by the fault handler when a protected page is accessed.
    // Returns true if the fault was handled (page restored).
    bool handleFault(uintptr_t addr) {
        if (!vm_->contains(addr)) return false;

        size_t page_idx = vm_->pageIndex(addr);
        locks_->lock(page_idx);

        PageState st = states_->get(page_idx);

        switch (st) {
        case PageState::COMPRESSED: {
            void* page_addr = vm_->pageAddress(page_idx);
            CompressAlgo algo = compressed_[page_idx].algorithm();
            uint8_t sc = lookupSizeClass(page_idx);

            // Spin-wait for a fault slot - cannot skip since thread is faulted.
            // With 128 slots this should rarely spin, but under extreme load
            // it ensures we always use thread-safe per-slot DCtx.
            int slot;
            while ((slot = acquireFaultSlot()) < 0) {
#if defined(__x86_64__)
                __builtin_ia32_pause();
#elif defined(__aarch64__)
                asm volatile("yield");
#endif
            }

            // Decompress using per-slot DCtx (no data race)
            engine_->decompressWithDCtx(
                fault_slots_[slot].dctx,
                compressed_[page_idx].data, fault_slots_[slot].buf,
                compressed_[page_idx].compressedSize(), kPageSize,
                algo, sc);

            vm::commitPages(page_addr, kPageSize);
            __builtin_memcpy(page_addr, fault_slots_[slot].buf, kPageSize);
            releaseFaultSlot(slot);

            // Release compressed blob (sharded store)
            store_->release(compressed_[page_idx].data,
                           compressed_[page_idx].alloc_size, page_idx);
            compressed_[page_idx] = {};
            if (page_tier_) page_tier_[page_idx] = kTierNone;

            states_->set(page_idx, PageState::ACTIVE);
            cold_count_[page_idx] = 0;
            locks_->unlock(page_idx);

            // Recompression-thrash signal. The act of faulting back from
            // COMPRESSED is direct evidence the page was hotter than we
            // thought; bump the per-page count and feed the per-bucket EMA
            // so phase2 backs off on this page (and on fresh pages from the
            // same allocation site) the next round.
            uint8_t prev_rc = recompress_count_[page_idx];
            if (prev_rc < 255) recompress_count_[page_idx] = prev_rc + 1;

            // Adaptive cap: decompression = re-warm evidence.  Notify the
            // heap so it can bias cap sizing for this (arena, sc) toward
            // "hot" (no cap / larger cap).
            //
            // Same lookup gives us (arena, size_class) for the bucket EMA
            // update — do both in one Span fetch.
            if (page_map_) {
                Span* sp = page_map_->get(reinterpret_cast<uintptr_t>(page_addr));
                if (sp && !sp->is_large && sp->size_class < kNumClasses) {
                    if (decompressed_fn_) {
                        decompressed_fn_(page_idx, sp->arena_id, sp->size_class,
                                         decompressed_ctx_);
                    }
                    size_t bidx = static_cast<size_t>(sp->arena_id) * kNumClasses
                                + sp->size_class;
                    if (bidx < kBucketTableLen) {
                        // EMA α=1/4, ×256 fixed-point. Relaxed atomics; tearing
                        // on the EMA is acceptable.
                        uint16_t old = bucket_rc_ema_x256_[bidx].load(std::memory_order_relaxed);
                        uint16_t sample_x256 = static_cast<uint16_t>(prev_rc + 1) * 256;
                        uint8_t cnt = bucket_rc_count_[bidx].load(std::memory_order_relaxed);
                        uint16_t neu = (cnt == 0)
                            ? sample_x256
                            : static_cast<uint16_t>(static_cast<int32_t>(old)
                                + (static_cast<int32_t>(sample_x256) - static_cast<int32_t>(old)) / 4);
                        bucket_rc_ema_x256_[bidx].store(neu, std::memory_order_relaxed);
                        if (cnt < 64)
                            bucket_rc_count_[bidx].store(cnt + 1, std::memory_order_relaxed);
                    }
                }
            }

            // Prefetch adjacent compressed pages
            prefetchAdjacent(page_idx);

            return true;
        }

        case PageState::COMPRESSING: {
            // TLA+ model proves this is unreachable: fault handler acquires
            // the page lock, but compressor holds it during COMPRESSING.
            // Verified unreachable via TLA+ model checking and
            // 576 benchmark runs with __builtin_trap() (zero crashes).
            // Retained as defensive fallback.
            void* page_addr = vm_->pageAddress(page_idx);
            vm::commitPages(page_addr, kPageSize);
            states_->set(page_idx, PageState::ACTIVE);
            cold_count_[page_idx] = 0;
            locks_->unlock(page_idx);
            return true;
        }

        case PageState::ACTIVE_MONITORING: {
            void* page_addr = vm_->pageAddress(page_idx);
            vm::protectPages(page_addr, kPageSize, true, true);
            accessed_[page_idx].store(true, std::memory_order_relaxed);
            states_->set(page_idx, PageState::ACTIVE);
            locks_->unlock(page_idx);
            return true;
        }

        case PageState::ACTIVE: {
            // Race: Phase 3's batched mprotect(PROT_READ) can overwrite a
            // per-page PROT_RW restoration done by a concurrent fault handler.
            void* page_addr = vm_->pageAddress(page_idx);
            vm::protectPages(page_addr, kPageSize, true, true);
            locks_->unlock(page_idx);
            return true;
        }

        default:
            locks_->unlock(page_idx);
            return false;
        }
    }

    // Release any compressed data for a range of pages (called during span release)
    void releaseCompressedPages(size_t start_page, size_t num_pages) {
        for (size_t i = start_page; i < start_page + num_pages; ++i) {
            locks_->lock(i);
            if (states_->get(i) == PageState::COMPRESSED && compressed_[i].data) {
                store_->release(compressed_[i].data, compressed_[i].alloc_size, i);
                compressed_[i] = {};
            }
            states_->set(i, PageState::EMPTY);
            cold_count_[i] = 0;
            recompress_count_[i] = 0;
            accessed_[i].store(false, std::memory_order_relaxed);
            locks_->unlock(i);
        }
    }
};

} // namespace smash
