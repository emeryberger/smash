// smash/src/compress/compressor_thread.h - Background scan-and-compress loop
//
// Integrates cold page detection (access tracking) and multi-algorithm
// compression (LZ4/zstd/zstd+dict). Supports adaptive algorithm selection
// based on cold duration, per-size-class dictionary training, prefetching
// of adjacent pages on fault, and batch decommit.
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

#include <atomic>
#include <pthread.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unistd.h>

namespace smash {

class CompressorThread {
    VmRegion* vm_ = nullptr;
    PageStateTable* states_ = nullptr;
    PageLockTable* locks_ = nullptr;
    CompressStore* store_ = nullptr;
    CompressEngine* engine_ = nullptr;
    PageMap* page_map_ = nullptr;

    // Per-page metadata (allocated from bootstrap, indexed by VmRegion page index)
    CompressedPageInfo* compressed_ = nullptr;  // compressed page data
    std::atomic<bool>* accessed_ = nullptr;     // set by fault handler on write
    uint8_t* cold_count_ = nullptr;             // consecutive ticks without access

    // Compression scratch buffer (one page, pre-allocated)
    void* page_buf_ = nullptr;      // for reading page data before compression
    void* compress_buf_ = nullptr;  // for compressed output

    // Dictionary training state per size class
    struct DictTrainState {
        char* sample_data;          // Concatenated page samples (bootstrap)
        size_t* sample_sizes;       // Per-sample size (bootstrap)
        uint16_t num_samples;
        bool trained;
        bool allocated;             // Whether sample buffers are allocated
    };
    DictTrainState dict_train_[kNumClasses]{};

    pthread_t thread_{};
    std::atomic<bool> running_{false};

    static void* threadEntry(void* arg) {
        static_cast<CompressorThread*>(arg)->run();
        return nullptr;
    }

    void run() {
        while (running_.load(std::memory_order_relaxed)) {
            usleep(kCompressIntervalMs * 1000);
            if (!running_.load(std::memory_order_relaxed)) break;
            tick();
        }
    }

    // Look up size class for a page via PageMap
    uint8_t lookupSizeClass(size_t page_idx) {
        if (!page_map_) return 0;
        void* addr = vm_->pageAddress(page_idx);
        Span* span = page_map_->get(reinterpret_cast<uintptr_t>(addr));
        if (!span) return 0;
        return span->size_class;
    }

    // Check if two page indices belong to the same span
    bool sameSpan(size_t page_a, size_t page_b) {
        if (!page_map_) return false;
        Span* sa = page_map_->get(reinterpret_cast<uintptr_t>(vm_->pageAddress(page_a)));
        Span* sb = page_map_->get(reinterpret_cast<uintptr_t>(vm_->pageAddress(page_b)));
        return sa && sb && sa == sb;
    }

    // Select compression algorithm based on cold duration and dictionary availability
    CompressAlgo selectAlgorithm(size_t page_idx) {
        uint8_t cold = cold_count_[page_idx];
        if (cold >= kVeryColdTicks) {
            uint8_t sc = lookupSizeClass(page_idx);
            if (engine_->hasDictionary(sc))
                return CompressAlgo::ZSTD_DICT;
            return CompressAlgo::ZSTD;  // deep level handled by engine
        }
        if (cold >= kColdTicks) {
            return CompressAlgo::LZ4;
        }
        return CompressAlgo::LZ4;
    }

    // Get effective zstd level for a page
    int effectiveZstdLevel(size_t page_idx) {
        return (cold_count_[page_idx] >= kVeryColdTicks)
            ? kZstdDeepLevel : kZstdNormalLevel;
    }

    // Collect a page sample for dictionary training
    void collectDictSample(size_t page_idx, uint8_t size_class) {
        if (size_class >= kNumClasses) return;

        auto& dt = dict_train_[size_class];
        if (dt.trained) return;

        // Lazy-allocate sample buffers
        if (!dt.allocated) {
            size_t buf_size = static_cast<size_t>(kDictTrainSamples) * kPageSize;
            dt.sample_data = static_cast<char*>(
                BootstrapAlloc::instance().allocate(buf_size, kPageSize));
            dt.sample_sizes = bootstrapArray<size_t>(kDictTrainSamples);
            dt.allocated = true;
            if (!dt.sample_data || !dt.sample_sizes) {
                dt.trained = true;  // Mark trained to prevent retries
                return;
            }
        }

        if (dt.num_samples >= kDictTrainSamples) return;

        // Copy page data into sample buffer
        size_t offset = static_cast<size_t>(dt.num_samples) * kPageSize;
        __builtin_memcpy(dt.sample_data + offset, page_buf_, kPageSize);
        dt.sample_sizes[dt.num_samples] = kPageSize;
        dt.num_samples++;

        // Trigger training when enough samples collected
        if (dt.num_samples >= kDictTrainSamples) {
            dt.trained = engine_->trainDictionary(
                size_class, dt.sample_data, dt.sample_sizes, dt.num_samples);
            if (!dt.trained) {
                dt.trained = true;  // Mark to prevent retries on failure
            }
        }
    }

    void tick() {
        size_t committed = vm_->committedPages();

        // Phase 1: Process access bits and update cold counts
        for (size_t i = 0; i < committed; ++i) {
            PageState st = states_->get(i);
            if (st == PageState::ACTIVE || st == PageState::ACTIVE_MONITORING) {
                if (accessed_[i].load(std::memory_order_relaxed)) {
                    cold_count_[i] = 0;
                    accessed_[i].store(false, std::memory_order_relaxed);
                } else {
                    if (cold_count_[i] < 255) cold_count_[i]++;
                }
            }
        }

        // Phase 2: Compress cold pages (batch decommit)
        // Track which pages were compressed so we can batch decommit
        // We use a simple scan approach: compress without decommit, then batch decommit
        for (size_t i = 0; i < committed; ++i) {
            if (cold_count_[i] < kColdTicks) continue;
            PageState st = states_->get(i);
            if (st != PageState::ACTIVE && st != PageState::ACTIVE_MONITORING)
                continue;

            compressPageNoDecommit(i);
        }

        // Phase 2b: Batch decommit all newly compressed pages
        batchDecommit(committed);

        // Phase 3: Set up access monitoring for remaining active pages
        size_t run_start = 0;
        bool in_run = false;
        for (size_t i = 0; i < committed; ++i) {
            PageState st = states_->get(i);
            if (st == PageState::ACTIVE) {
                if (!in_run) { run_start = i; in_run = true; }
                states_->set(i, PageState::ACTIVE_MONITORING);
            } else {
                if (in_run) {
                    size_t count = i - run_start;
                    vm::protectPages(
                        vm_->pageAddress(run_start),
                        count * kPageSize, true, false);  // PROT_READ
                    in_run = false;
                }
            }
        }
        if (in_run) {
            size_t count = committed - run_start;
            vm::protectPages(
                vm_->pageAddress(run_start),
                count * kPageSize, true, false);
        }
    }

    // Compress a page without decommitting. Returns true if compressed.
    bool compressPageNoDecommit(size_t page_idx) {
        locks_->lock(page_idx);

        // Verify still eligible
        PageState st = states_->get(page_idx);
        if (st != PageState::ACTIVE && st != PageState::ACTIVE_MONITORING) {
            locks_->unlock(page_idx);
            return false;
        }

        // Mark as compressing
        states_->set(page_idx, PageState::COMPRESSING);

        // Make page read-only to get a consistent snapshot
        void* page_addr = vm_->pageAddress(page_idx);
        vm::protectPages(page_addr, kPageSize, true, false);  // PROT_READ

        // Copy page data
        __builtin_memcpy(page_buf_, page_addr, kPageSize);

        // Make page inaccessible
        vm::protectPages(page_addr, kPageSize, false, false);  // PROT_NONE

        // Check if we were preempted by a fault
        if (states_->get(page_idx) != PageState::COMPRESSING) {
            locks_->unlock(page_idx);
            return false;
        }

        // Select algorithm and compress
        CompressAlgo algo = selectAlgorithm(page_idx);
        uint8_t sc = lookupSizeClass(page_idx);

        // Collect sample for dictionary training (page data is in page_buf_)
        collectDictSample(page_idx, sc);

        size_t max_comp = CompressEngine::maxCompressedSizeAny(kPageSize);
        size_t comp_size = engine_->compress(page_buf_, compress_buf_, kPageSize, max_comp,
                                             algo, sc);

        if (comp_size == 0 || comp_size > static_cast<size_t>(kPageSize * kMinCompressRatio)) {
            // Not worth compressing; restore page
            vm::protectPages(page_addr, kPageSize, true, true);
            __builtin_memcpy(page_addr, page_buf_, kPageSize);
            states_->set(page_idx, PageState::ACTIVE);
            // Don't reset cold_count — let it continue climbing for escalation
            locks_->unlock(page_idx);
            return false;
        }

        // Store compressed data
        size_t alloc_size = 0;
        void* stored = store_->store(compress_buf_, comp_size, &alloc_size);
        if (!stored) {
            vm::protectPages(page_addr, kPageSize, true, true);
            __builtin_memcpy(page_addr, page_buf_, kPageSize);
            states_->set(page_idx, PageState::ACTIVE);
            locks_->unlock(page_idx);
            return false;
        }

        // Record compressed page info (with algo in top 2 bits)
        compressed_[page_idx].set(stored, comp_size, alloc_size, algo);

        // Mark COMPRESSED but don't decommit yet (batch decommit later)
        states_->set(page_idx, PageState::COMPRESSED);
        locks_->unlock(page_idx);
        return true;
    }

    // Batch decommit: find contiguous COMPRESSED runs and issue one decommit per run
    void batchDecommit(size_t committed) {
        size_t run_start = 0;
        bool in_run = false;

        for (size_t i = 0; i < committed; ++i) {
            if (states_->get(i) == PageState::COMPRESSED && compressed_[i].data != nullptr) {
                // Check if this page still has physical backing that needs decommit
                // We only decommit pages that were just compressed in this tick
                // (they're COMPRESSED but the physical page is still mapped to PROT_NONE)
                if (!in_run) { run_start = i; in_run = true; }
            } else {
                if (in_run) {
                    size_t count = i - run_start;
                    vm::decommitPages(vm_->pageAddress(run_start), count * kPageSize);
                    in_run = false;
                }
            }
        }
        if (in_run) {
            size_t count = committed - run_start;
            vm::decommitPages(vm_->pageAddress(run_start), count * kPageSize);
        }
    }

    // Prefetch adjacent compressed pages around a faulted page
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

                // Recommit the page
                vm::commitPages(adj_addr, kPageSize);

                // Decompress
                CompressAlgo algo = compressed_[adj].algorithm();
                uint8_t sc = lookupSizeClass(adj);
                engine_->decompress(
                    compressed_[adj].data, adj_addr,
                    compressed_[adj].compressedSize(), kPageSize,
                    algo, sc);

                // Release compressed blob
                store_->release(compressed_[adj].data, compressed_[adj].alloc_size);
                compressed_[adj] = {};

                // Restore to active
                states_->set(adj, PageState::ACTIVE);
                cold_count_[adj] = 0;
            }

            locks_->unlock(adj);
        }
    }

public:
    void init(VmRegion* vm, PageStateTable* states, PageLockTable* locks,
              CompressStore* store, CompressEngine* engine,
              PageMap* page_map = nullptr) {
        vm_ = vm;
        states_ = states;
        locks_ = locks;
        store_ = store;
        engine_ = engine;
        page_map_ = page_map;

        size_t max_pages = vm->totalPages();
        compressed_ = bootstrapArray<CompressedPageInfo>(max_pages);
        accessed_ = bootstrapArray<std::atomic<bool>>(max_pages);
        cold_count_ = bootstrapArray<uint8_t>(max_pages);

        // Pre-allocate scratch buffers (large enough for any algorithm)
        size_t max_comp = CompressEngine::maxCompressedSizeAny(kPageSize);
        page_buf_ = BootstrapAlloc::instance().allocate(kPageSize, kPageSize);
        compress_buf_ = BootstrapAlloc::instance().allocate(max_comp, 16);
    }

    void start() {
        running_.store(true, std::memory_order_release);
        pthread_create(&thread_, nullptr, threadEntry, this);
    }

    void stop() {
        if (!running_.load(std::memory_order_relaxed)) return;
        running_.store(false, std::memory_order_release);
        pthread_join(thread_, nullptr);
    }

    // Manually trigger one compression tick (for testing/manual control)
    void compressTick() { tick(); }

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

            // Recommit the page
            vm::commitPages(page_addr, kPageSize);

            // Decompress using the correct algorithm
            CompressAlgo algo = compressed_[page_idx].algorithm();
            uint8_t sc = lookupSizeClass(page_idx);
            engine_->decompress(
                compressed_[page_idx].data, page_addr,
                compressed_[page_idx].compressedSize(), kPageSize,
                algo, sc);

            // Release compressed blob
            store_->release(compressed_[page_idx].data,
                           compressed_[page_idx].alloc_size);
            compressed_[page_idx] = {};

            // Restore to active
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
                store_->release(compressed_[i].data, compressed_[i].alloc_size);
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
