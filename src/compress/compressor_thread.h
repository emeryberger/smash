// smash/src/compress/compressor_thread.h - Background scan-and-compress loop
//
// Integrates cold page detection (access tracking) and multi-algorithm
// compression (LZ4/zstd/zstd+dict). Supports adaptive algorithm selection
// based on cold duration, per-size-class dictionary training, prefetching
// of adjacent pages on fault, and batch decommit.
//
// Parallelism: kCompressorWorkers threads process disjoint page ranges.
// Chunk bitmap skips EMPTY pages. Per-fault-slot DCtx avoids decompress races.
// Sharded CompressStore eliminates single lock bottleneck.
#pragma once

#include "smash/config.h"
#include "compress_store.h"
#include "compress_engine.h"
#include "../vm/vm_region.h"
#include "../vm/page_state.h"
#include "../vm/platform_mem.h"
#include "../core/bootstrap_alloc.h"
#include "../core/page_map.h"
#include "../util/spinlock.h"
#include "../vm/fault_handler.h"
#include "../vm/syscall_compat.h"

#include <atomic>
#include <pthread.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unistd.h>

#include <lz4.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

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

    // Per-page metadata (allocated from bootstrap, indexed by VmRegion page index)
    CompressedPageInfo* compressed_ = nullptr;
    std::atomic<bool>* accessed_ = nullptr;
    uint8_t* cold_count_ = nullptr;

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
    static constexpr int kFaultSlotCount = 32;
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

    // ── Per-size-class sliding window stats ───────────────────────────────
    struct SizeClassStats {
        static constexpr int kWindow = 64;
        uint8_t ratios[kWindow]{};
        uint8_t head = 0, count = 0;
        uint16_t sum = 0;

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

        bool shouldSkip() const {
#ifdef SMASH_ABLATION_NO_SKIP_STATS
            return false;
#else
            return count >= kWindow / 2 &&
                   sum < static_cast<uint16_t>(count) * 13;  // 13/255 ≈ 5%
#endif
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
        SizeClassStats sc_stats[kNumClasses]{};
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

    CompressWorker workers_[kCompressorWorkers];

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
    pthread_t helper_threads_[kCompressorWorkers > 1 ? kCompressorWorkers - 1 : 1]{};
    std::atomic<bool> running_{false};
    bool stdio_pinned_ = false;

    // Work dispatch: coordinator increments work_gen_ to signal helpers.
    // Helpers compare against their last-seen gen to detect new work.
    std::atomic<uint64_t> work_gen_{0};
    int current_phase_ = 0;  // 1=access, 2=compress, 3=monitor
    std::atomic<uint64_t> helper_done_gen_[kCompressorWorkers > 1 ? kCompressorWorkers - 1 : 1]{};

    // ── Helper methods ────────────────────────────────────────────────────

    uint8_t lookupSizeClass(size_t page_idx) {
        if (!page_map_) return 0;
        void* addr = vm_->pageAddress(page_idx);
        Span* span = page_map_->get(reinterpret_cast<uintptr_t>(addr));
        if (!span) return 0;
        return span->size_class;
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
        auto* p = static_cast<uint64_t*>(dst);
        size_t n = size / 8;  // always even since size % 16 == 0
        for (size_t i = 0; i < n; i += 2) {
            __builtin_nontemporal_store(static_cast<uint64_t>(0), &p[i]);
            __builtin_nontemporal_store(static_cast<uint64_t>(0), &p[i + 1]);
        }
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

    CompressAlgo selectAlgorithm(size_t page_idx) {
        uint8_t cold = cold_count_[page_idx];
        if (cold >= kVeryColdTicks) {
            uint8_t sc = lookupSizeClass(page_idx);
            if (engine_->hasDictionary(sc))
                return CompressAlgo::ZSTD_DICT;
            return CompressAlgo::ZSTD;
        }
        return CompressAlgo::LZ4;
    }

    int effectiveZstdLevel(size_t page_idx) {
        return (cold_count_[page_idx] >= kVeryColdTicks)
            ? kZstdDeepLevel : kZstdNormalLevel;
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
                }
            }
        });
    }

    // Phase 2: Compress cold pages
    void phase2Range(int worker_id, size_t start, size_t end) {
        forEachLivePage(start, end, [&](size_t i) {
            if (cold_count_[i] < kColdTicks) return;
            PageState st = states_->get(i);
            if (st != PageState::ACTIVE && st != PageState::ACTIVE_MONITORING)
                return;
            compressPage(i, workers_[worker_id]);
        });
    }

    // Phase 3: Set up access monitoring for remaining active pages
    void phase3Range(size_t start, size_t end) {
        size_t run_start = 0;
        size_t last_in_run = 0;
        bool in_run = false;

        forEachLivePage(start, end, [&](size_t i) {
            PageState st = states_->get(i);
            bool pinned = vm::g_page_pins &&
                vm::g_page_pins[i].load(std::memory_order_relaxed) > 0;
            if (st == PageState::ACTIVE && !pinned) {
                if (!in_run) {
                    run_start = i;
                    in_run = true;
                } else if (i != last_in_run + 1 ||
                           reinterpret_cast<uintptr_t>(vm_->pageAddress(i)) !=
                           reinterpret_cast<uintptr_t>(vm_->pageAddress(last_in_run)) + kPageSize) {
                    // Gap or non-contiguous addresses — flush previous run
                    vm::protectPages(vm_->pageAddress(run_start),
                                    (last_in_run - run_start + 1) * kPageSize, true, false);
                    run_start = i;
                }
                last_in_run = i;
                states_->set(i, PageState::ACTIVE_MONITORING);
            } else {
                if (in_run) {
                    vm::protectPages(vm_->pageAddress(run_start),
                                    (last_in_run - run_start + 1) * kPageSize, true, false);
                    in_run = false;
                }
            }
        });
        if (in_run) {
            vm::protectPages(vm_->pageAddress(run_start),
                            (last_in_run - run_start + 1) * kPageSize, true, false);
        }
    }

    // ── Compress one page using worker's contexts ─────────────────────────

    bool compressPage(size_t page_idx, CompressWorker& worker) {
        // Skip pinned pages (in use by a blocking syscall)
        if (vm::g_page_pins && vm::g_page_pins[page_idx].load(std::memory_order_relaxed) > 0)
            return false;

        locks_->lock(page_idx);

        // Verify still eligible
        PageState st = states_->get(page_idx);
        if (st != PageState::ACTIVE && st != PageState::ACTIVE_MONITORING) {
            locks_->unlock(page_idx);
            return false;
        }

        // Mark as compressing
        states_->set(page_idx, PageState::COMPRESSING);

        // Adaptive skip: if recent attempts for this size class have poor ratios
        CompressAlgo algo = selectAlgorithm(page_idx);
        uint8_t sc = lookupSizeClass(page_idx);
        if (algo == CompressAlgo::LZ4 && sc < kNumClasses) {
            if (worker.sc_stats[sc].shouldSkip()) {
                states_->set(page_idx, PageState::ACTIVE);
                locks_->unlock(page_idx);
                return false;
            }
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

        // Make page inaccessible
        vm::protectPages(page_addr, kPageSize, false, false);  // PROT_NONE

        // Check if we were preempted by a fault
        if (states_->get(page_idx) != PageState::COMPRESSING) {
            locks_->unlock(page_idx);
            return false;
        }

        // Collect sample for dictionary training (page data in worker's buf)
        collectDictSample(page_idx, sc, worker.page_buf);

        int zstd_level = effectiveZstdLevel(page_idx);
        size_t max_comp = CompressEngine::maxCompressedSizeAny(kPageSize);
        size_t comp_size = 0;

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
                    comp_size = dict_size;  // dict wins, result in compress_buf
                } else if (dict_size > plain_size) {
                    dict_loss_count_.fetch_add(1, std::memory_order_relaxed);
                    algo = CompressAlgo::ZSTD;
                    comp_size = plain_size;
                    // plain wins — swap buffers so compress_buf has the better result
                    void* tmp = worker.compress_buf;
                    worker.compress_buf = worker.compress_buf2;
                    worker.compress_buf2 = tmp;
                } else {
                    dict_tie_count_.fetch_add(1, std::memory_order_relaxed);
                    comp_size = dict_size;  // tie, use dict (already in compress_buf)
                }
            } else {
                // One or both failed; use whichever succeeded
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

        if (comp_size == 0 || comp_size > static_cast<size_t>(kPageSize * kMinCompressRatio)) {
            // Not worth compressing; record poor ratio and restore page
            if (sc < kNumClasses) worker.sc_stats[sc].record(comp_size ? comp_size : kPageSize, kPageSize);
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

        // Record successful compression ratio
        if (sc < kNumClasses) worker.sc_stats[sc].record(comp_size, kPageSize);

        // Record compressed page info (with algo in top 2 bits)
        compressed_[page_idx].set(stored, comp_size, alloc_size, algo);

        // Decommit physical backing while holding lock
        vm::decommitPages(page_addr, kPageSize);

        states_->set(page_idx, PageState::COMPRESSED);
        locks_->unlock(page_idx);
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

                int slot = acquireFaultSlot();
                if (slot >= 0) {
                    engine_->decompressWithDCtx(
                        fault_slots_[slot].dctx,
                        compressed_[adj].data, fault_slots_[slot].buf,
                        compressed_[adj].compressedSize(), kPageSize,
                        algo, sc);
                    vm::commitPages(adj_addr, kPageSize);
                    __builtin_memcpy(adj_addr, fault_slots_[slot].buf, kPageSize);
                    releaseFaultSlot(slot);
                } else {
                    // All slots in use — fall back to direct decompress.
                    // This races with other DCtx users but is the last resort.
                    vm::commitPages(adj_addr, kPageSize);
                    engine_->decompress(
                        compressed_[adj].data, adj_addr,
                        compressed_[adj].compressedSize(), kPageSize,
                        algo, sc);
                }

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

    // ── stdio buffer pinning ──────────────────────────────────────────────

    void pinStdioBuffers() {
        if (!vm::g_page_pins) return;
        FILE* streams[] = { stdin, stdout, stderr };
        for (FILE* f : streams) {
            if (!f || !f->_bf._base || f->_bf._size <= 0) continue;
            smash::vm::warmPages(f->_bf._base, f->_bf._size, vm_);
            smash::vm::pinPages(f->_bf._base, f->_bf._size, vm_);
            smash::vm::warmPages(f, sizeof(FILE), vm_);
            smash::vm::pinPages(f, sizeof(FILE), vm_);
        }
    }

    // ── Parallel dispatch ─────────────────────────────────────────────────

    void dispatch(int phase) {
        // If helpers are not running (e.g., compressTick() called directly
        // in tests), fall back to single-threaded execution.
        bool helpers_active = running_.load(std::memory_order_relaxed) &&
                              kCompressorWorkers > 1;
        if (helpers_active) {
            current_phase_ = phase;
            work_gen_.fetch_add(1, std::memory_order_release);

            // Coordinator does worker 0's range
            executePhase(phase, 0);

            // Wait for all helpers
            uint64_t target = work_gen_.load(std::memory_order_relaxed);
            for (int i = 0; i < kCompressorWorkers - 1; ++i) {
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

        // Pin stdio buffers once (they don't move after first use)
        if (!stdio_pinned_) {
            pinStdioBuffers();
            stdio_pinned_ = true;
        }

        size_t committed = vm_->committedPages();
        if (committed == 0) return;

        // Rebuild chunk bitmap
        rebuildChunkBitmap(committed);

        // Partition page range across workers.
        // If helpers are not running, worker 0 gets the entire range.
        bool helpers_active = running_.load(std::memory_order_relaxed) &&
                              kCompressorWorkers > 1;
        if (helpers_active) {
            size_t per_worker = committed / kCompressorWorkers;
            for (int w = 0; w < kCompressorWorkers; ++w) {
                workers_[w].range_start = w * per_worker;
                workers_[w].range_end = (w == kCompressorWorkers - 1)
                    ? committed : (w + 1) * per_worker;
            }
        } else {
            workers_[0].range_start = 0;
            workers_[0].range_end = committed;
        }

        dispatch(1);  // Phase 1: access tracking
        dispatch(2);  // Phase 2: compression
        dispatch(3);  // Phase 3: monitoring

        trainDictionaries();
    }

    // ── Thread entry points ───────────────────────────────────────────────

    static void* coordEntry(void* arg) {
        auto* self = static_cast<CompressorThread*>(arg);
        while (self->running_.load(std::memory_order_relaxed)) {
            usleep(kCompressIntervalMs * 1000);
            if (!self->running_.load(std::memory_order_relaxed)) break;
            self->tick();
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

            self->executePhase(self->current_phase_, worker_id);
            self->helper_done_gen_[helper_id].store(gen, std::memory_order_release);
        }
        return nullptr;
    }

public:
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

        for (int w = 0; w < kCompressorWorkers; ++w) {
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
    }

    void setPreTickCallback(PreTickFn fn) { pre_tick_fn_ = fn; }

    void start() {
        running_.store(true, std::memory_order_release);

        // Start helper threads
        if constexpr (kCompressorWorkers > 1) {
            for (int i = 0; i < kCompressorWorkers - 1; ++i) {
                auto* ha = static_cast<HelperArg*>(
                    BootstrapAlloc::instance().allocate(sizeof(HelperArg), alignof(HelperArg)));
                ha->self = this;
                ha->id = i;
                pthread_create(&helper_threads_[i], nullptr, helperEntry, ha);
            }
        }

        // Start coordinator thread
        pthread_create(&coord_thread_, nullptr, coordEntry, this);
    }

    void stop() {
        if (!running_.load(std::memory_order_relaxed)) return;
        running_.store(false, std::memory_order_release);
        pthread_join(coord_thread_, nullptr);
        if constexpr (kCompressorWorkers > 1) {
            for (int i = 0; i < kCompressorWorkers - 1; ++i)
                pthread_join(helper_threads_[i], nullptr);
        }
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

            int slot = acquireFaultSlot();
            if (slot >= 0) {
                // Decompress using per-slot DCtx (no data race)
                engine_->decompressWithDCtx(
                    fault_slots_[slot].dctx,
                    compressed_[page_idx].data, fault_slots_[slot].buf,
                    compressed_[page_idx].compressedSize(), kPageSize,
                    algo, sc);

                vm::commitPages(page_addr, kPageSize);
                __builtin_memcpy(page_addr, fault_slots_[slot].buf, kPageSize);
                releaseFaultSlot(slot);
            } else {
                // All slots in use — fall back to direct decompress
                vm::commitPages(page_addr, kPageSize);
                engine_->decompress(
                    compressed_[page_idx].data, page_addr,
                    compressed_[page_idx].compressedSize(), kPageSize,
                    algo, sc);
            }

            // Release compressed blob (sharded store)
            store_->release(compressed_[page_idx].data,
                           compressed_[page_idx].alloc_size, page_idx);
            compressed_[page_idx] = {};

            states_->set(page_idx, PageState::ACTIVE);
            cold_count_[page_idx] = 0;
            locks_->unlock(page_idx);

            // Prefetch adjacent compressed pages
            prefetchAdjacent(page_idx);

            return true;
        }

        case PageState::COMPRESSING: {
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
            accessed_[i].store(false, std::memory_order_relaxed);
            locks_->unlock(i);
        }
    }
};

} // namespace smash
