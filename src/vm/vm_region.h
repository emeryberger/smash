// smash/src/vm/vm_region.h - Single large virtual memory reservation
//
// All slab data pages are allocated from this contiguous region.
// Benefits: O(1) bounds check, page state tracking, reassignable pages.
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
    char* base_ = nullptr;
    size_t total_pages_ = 0;
    std::atomic<size_t> next_page_{0};  // bump pointer

    // Free page runs for reuse
    struct FreeRun {
        size_t page_index;
        size_t page_count;
        FreeRun* next;
    };
    FreeRun* free_list_ = nullptr;
    FreeRun* free_pool_ = nullptr;  // recycled FreeRun structs
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

public:
    bool init(size_t region_size) {
        total_pages_ = region_size / kPageSize;
        base_ = static_cast<char*>(vm::reservePages(region_size));
        return base_ != nullptr;
    }

    // Allocate num_pages contiguous pages. Returns page-aligned address.
    // Pages are committed (PROT_READ|PROT_WRITE) on return.
    void* allocatePages(size_t num_pages) {
        // Try free list first
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

        // Bump allocate
        size_t start = next_page_.fetch_add(num_pages, std::memory_order_relaxed);
        if (start + num_pages > total_pages_) {
            next_page_.fetch_sub(num_pages, std::memory_order_relaxed);
            return nullptr;
        }
        void* addr = base_ + start * kPageSize;
        vm::commitPages(addr, num_pages * kPageSize);
        return addr;
    }

    // Release pages back to the region (decommit physical backing).
    void releasePages(void* addr, size_t num_pages) {
        vm::protectPages(addr, num_pages * kPageSize, false, false); // PROT_NONE
        vm::decommitPages(addr, num_pages * kPageSize);

        size_t page_idx = pageIndex(reinterpret_cast<uintptr_t>(addr));
        FreeRun* run = newFreeRun();
        run->page_index = page_idx;
        run->page_count = num_pages;

        LockGuard guard(free_lock_);
        run->next = free_list_;
        free_list_ = run;
    }

    bool contains(uintptr_t addr) const {
        auto a = addr;
        auto b = reinterpret_cast<uintptr_t>(base_);
        return a >= b && a < b + total_pages_ * kPageSize;
    }

    size_t pageIndex(uintptr_t addr) const {
        return (addr - reinterpret_cast<uintptr_t>(base_)) / kPageSize;
    }

    void* pageAddress(size_t index) const {
        return base_ + index * kPageSize;
    }

    char* base() const { return base_; }
    size_t totalPages() const { return total_pages_; }
    size_t committedPages() const { return next_page_.load(std::memory_order_relaxed); }
};

} // namespace smash
