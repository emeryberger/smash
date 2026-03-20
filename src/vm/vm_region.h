// smash/src/vm/vm_region.h - Single large virtual memory reservation
//
// All slab data pages are allocated from this contiguous region.
// Benefits: O(1) bounds check, page state tracking, reassignable pages.
//
// Supports two modes (selected at runtime via SMASH_MODE env var):
// - Full mode: contiguous VM reservation, pages allocated via bump pointer
// - Compress-only mode: tracks arbitrary page addresses via hash map,
//   allowing compression of pages owned by any allocator
#pragma once

#include "smash/config.h"
#include "platform_mem.h"
#include "page_state.h"
#include "../core/bootstrap_alloc.h"
#include "../util/spinlock.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace smash {

class VmRegion {
    // Mode determined at init time
    bool tracking_mode_ = false;

    // ── Full mode: contiguous allocation ────────────────────────────────────
    char* base_ = nullptr;
    size_t total_pages_ = 0;
    std::atomic<size_t> next_page_{0};  // bump pointer (normal) or next index (tracking)

    struct FreeRun {
        size_t page_index;
        size_t page_count;
        FreeRun* next;
    };
    FreeRun* free_list_ = nullptr;
    FreeRun* free_pool_ = nullptr;
    Spinlock free_lock_;

    FreeRun* newFreeRun() {
        if (free_pool_) {
            FreeRun* r = free_pool_;
            free_pool_ = r->next;
            return r;
        }
        return static_cast<FreeRun*>(
            BootstrapAlloc::instance().allocate(sizeof(FreeRun), alignof(FreeRun)));
    }

    void recycleFreeRun(FreeRun* r) {
        r->next = free_pool_;
        free_pool_ = r;
    }

    // ── Tracking mode: hash map for arbitrary page addresses ────────────────
    static constexpr size_t kTrackMaxPages = 128 * 1024;  // 128K pages (~2GB on 16K pages)
    static constexpr size_t kTrackHashCap  = 256 * 1024;  // 2x headroom
    static constexpr size_t kTrackHashMask = kTrackHashCap - 1;

    struct TrackEntry {
        std::atomic<uintptr_t> key{0};  // page addr >> kPageShift; 0 = empty
        std::atomic<size_t> idx{0};
    };

    TrackEntry* track_hash_ = nullptr;
    uintptr_t* track_reverse_ = nullptr;  // idx → page_addr (page-aligned)

    size_t lookupIdx(uintptr_t addr) const {
        if (!tracking_mode_ || !track_hash_) return 0;
        uintptr_t key = addr >> kPageShift;
        if (key == 0) return 0;
        size_t slot = static_cast<size_t>(key * 0x9E3779B97F4A7C15ULL) >> (64 - 18);
        for (size_t i = 0; i < kTrackHashCap; ++i) {
            size_t s = (slot + i) & kTrackHashMask;
            uintptr_t existing = track_hash_[s].key.load(std::memory_order_relaxed);
            if (existing == key)
                return track_hash_[s].idx.load(std::memory_order_acquire);
            if (existing == 0)
                return 0;
        }
        return 0;
    }

public:
    bool init(size_t region_size) {
        tracking_mode_ = isCompressOnlyMode();

        if (!tracking_mode_) {
            // Full mode: reserve contiguous VM region
            total_pages_ = region_size / kPageSize;
            base_ = static_cast<char*>(vm::reservePages(region_size));
            return base_ != nullptr;
        } else {
            // Compress-only mode: initialize tracking hash table
            (void)region_size;
            total_pages_ = kTrackMaxPages;
            next_page_.store(1, std::memory_order_relaxed);  // index 0 reserved

            track_hash_ = static_cast<TrackEntry*>(
                BootstrapAlloc::instance().allocate(
                    kTrackHashCap * sizeof(TrackEntry), 64));
            track_reverse_ = static_cast<uintptr_t*>(
                BootstrapAlloc::instance().allocate(
                    kTrackMaxPages * sizeof(uintptr_t), 8));
            if (!track_hash_ || !track_reverse_) return false;

            // Zero-initialize hash table (0 = empty sentinel)
            __builtin_memset(track_hash_, 0, kTrackHashCap * sizeof(TrackEntry));
            __builtin_memset(track_reverse_, 0, kTrackMaxPages * sizeof(uintptr_t));
            return true;
        }
    }

    bool isTrackingMode() const { return tracking_mode_; }

    // ── Allocation (full mode only) ─────────────────────────────────────────
    void* allocatePages(size_t num_pages) {
        if (tracking_mode_) return nullptr;

        {
            LockGuard guard(free_lock_);
            FreeRun** prev = &free_list_;
            FreeRun* run = free_list_;
            while (run) {
                if (run->page_count >= num_pages) {
                    size_t page_idx = run->page_index;
                    if (run->page_count == num_pages) {
                        *prev = run->next;
                        recycleFreeRun(run);
                    } else {
                        run->page_index += num_pages;
                        run->page_count -= num_pages;
                    }
                    void* addr = base_ + page_idx * kPageSize;
                    vm::commitPages(addr, num_pages * kPageSize);
                    return addr;
                }
                prev = &run->next;
                run = run->next;
            }
        }
        size_t start = next_page_.fetch_add(num_pages, std::memory_order_relaxed);
        if (start + num_pages > total_pages_) {
            next_page_.fetch_sub(num_pages, std::memory_order_relaxed);
            return nullptr;
        }
        void* addr = base_ + start * kPageSize;
        vm::commitPages(addr, num_pages * kPageSize);
        return addr;
    }

    void releasePages(void* addr, size_t num_pages) {
        if (tracking_mode_) return;

        vm::protectPages(addr, num_pages * kPageSize, false, false);
        vm::decommitPages(addr, num_pages * kPageSize);
        size_t page_idx = pageIndex(reinterpret_cast<uintptr_t>(addr));
        FreeRun* run = newFreeRun();
        run->page_index = page_idx;
        run->page_count = num_pages;
        LockGuard guard(free_lock_);
        run->next = free_list_;
        free_list_ = run;
    }

    // ── Page tracking (compress-only mode) ──────────────────────────────────
    // Track a page address, assigning it a compact index.
    // Returns the index (>0) on success, 0 on failure (table full or wrong mode).
    size_t trackPage(uintptr_t page_addr) {
        if (!tracking_mode_ || !track_hash_) return 0;

        uintptr_t key = page_addr >> kPageShift;
        if (key == 0) return 0;
        size_t slot = static_cast<size_t>(key * 0x9E3779B97F4A7C15ULL) >> (64 - 18);
        for (size_t i = 0; i < kTrackHashCap; ++i) {
            size_t s = (slot + i) & kTrackHashMask;
            uintptr_t existing = track_hash_[s].key.load(std::memory_order_relaxed);
            if (existing == key)
                return track_hash_[s].idx.load(std::memory_order_acquire);
            if (existing == 0) {
                uintptr_t expected = 0;
                if (track_hash_[s].key.compare_exchange_strong(
                        expected, key, std::memory_order_acq_rel)) {
                    size_t idx = next_page_.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= total_pages_) return 0;
                    track_reverse_[idx] = page_addr;
                    track_hash_[s].idx.store(idx, std::memory_order_release);
                    return idx;
                }
                // Another thread claimed this slot; recheck
                if (track_hash_[s].key.load(std::memory_order_relaxed) == key)
                    return track_hash_[s].idx.load(std::memory_order_acquire);
            }
        }
        return 0;
    }

    bool contains(uintptr_t addr) const {
        if (!tracking_mode_) {
            auto a = addr;
            auto b = reinterpret_cast<uintptr_t>(base_);
            return a >= b && a < b + total_pages_ * kPageSize;
        } else {
            return lookupIdx(addr) != 0;
        }
    }

    size_t pageIndex(uintptr_t addr) const {
        if (!tracking_mode_) {
            return (addr - reinterpret_cast<uintptr_t>(base_)) / kPageSize;
        } else {
            return lookupIdx(addr);
        }
    }

    void* pageAddress(size_t index) const {
        if (!tracking_mode_) {
            return base_ + index * kPageSize;
        } else {
            if (index == 0 || index >= total_pages_) return nullptr;
            return reinterpret_cast<void*>(track_reverse_[index]);
        }
    }

    char* base() const { return base_; }
    size_t totalPages() const { return total_pages_; }
    size_t committedPages() const { return next_page_.load(std::memory_order_relaxed); }
};

} // namespace smash
