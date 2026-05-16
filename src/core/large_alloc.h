// smash/src/core/large_alloc.h - Large allocations (> kMaxSmallSize)
//
// When a VmRegion is available, large allocations are placed there so the
// compressor thread can track and compress their pages.  Falls back to
// direct mmap for oversized alignments or when the VmRegion is full.
#pragma once

#include "smash/config.h"
#include "span.h"
#include "page_map.h"
#include "../util/spinlock.h"
#include "../util/bitops.h"
#include "../vm/platform_mem.h"
#include "../vm/vm_region.h"
#include "../vm/page_state.h"

#include <cstddef>
#include <cstdint>

namespace smash {

class LargeAlloc {
    Spinlock lock_;
    PageMap* page_map_;
    VmRegion* vm_region_ = nullptr;
    PageStateTable* page_states_ = nullptr;
    void (*release_hook_)(size_t, size_t, void*) = nullptr;
    void* release_ctx_ = nullptr;

public:
    void init(PageMap* pm,
              VmRegion* vr = nullptr, PageStateTable* ps = nullptr,
              void (*hook)(size_t, size_t, void*) = nullptr,
              void* hook_ctx = nullptr) {
        page_map_ = pm;
        vm_region_ = vr;
        page_states_ = ps;
        release_hook_ = hook;
        release_ctx_ = hook_ctx;
    }

    void* allocate(size_t size, size_t alignment) {
        if (alignment < kPageSize) alignment = kPageSize;

        // Round size up to page boundary
        size_t alloc_size = roundUp(size, kPageSize);
        uint32_t num_pages = static_cast<uint32_t>(alloc_size / kPageSize);

        void* mem = nullptr;

        // Route large-enough allocations through VmRegion for compression
        if (vm_region_ && alignment <= kPageSize && alloc_size >= kLargeAllocVmThreshold) {
            mem = vm_region_->allocatePages(num_pages);
            if (mem && page_states_) {
                size_t idx = vm_region_->pageIndex(reinterpret_cast<uintptr_t>(mem));
                for (uint32_t p = 0; p < num_pages; ++p)
                    page_states_->set(idx + p, PageState::ACTIVE);
            }
        }

        // Fallback to direct mmap
        if (!mem) {
            if (alignment <= kPageSize) {
                mem = vm::mapPages(alloc_size);
            } else {
                // Over-allocate by alignment to guarantee aligned region
                size_t map_size = alloc_size + alignment;
                mem = vm::mapPages(map_size);
                if (!mem) return nullptr;

                auto addr = reinterpret_cast<uintptr_t>(mem);
                auto aligned_addr = roundUp(addr, alignment);
                size_t prefix = aligned_addr - addr;
                size_t suffix = map_size - prefix - alloc_size;

                if (prefix > 0) vm::unmapPages(mem, prefix);
                if (suffix > 0) vm::unmapPages(reinterpret_cast<void*>(aligned_addr + alloc_size), suffix);

                mem = reinterpret_cast<void*>(aligned_addr);
            }
        }

        if (!mem) return nullptr;

        Span* span = newSpanDescriptor();
        span->initLarge(mem, size, num_pages);

        // O(1) registration: only register the first page. The Span contains
        // page_count so we can compute the full range from just the base.
        // PageMap::set uses atomic stores internally - no external lock needed.
        page_map_->set(reinterpret_cast<uintptr_t>(mem), span);
        return mem;
    }

    void deallocate(Span* span) {
        void* base = span->base;
        size_t num_pages = span->page_count;

        // O(1) clear: only clear the first page (matches O(1) registration).
        // Must clear BEFORE releasing pages to prevent race with reallocation.
        page_map_->set(reinterpret_cast<uintptr_t>(base), nullptr);

        if (vm_region_ && vm_region_->contains(reinterpret_cast<uintptr_t>(base))) {
            if (release_hook_) {
                size_t idx = vm_region_->pageIndex(reinterpret_cast<uintptr_t>(base));
                release_hook_(idx, num_pages, release_ctx_);
            }
            vm_region_->releasePages(base, num_pages);
        } else {
            vm::unmapPages(base, num_pages * kPageSize);
        }
    }

    size_t getSize(Span* span) const {
        return span->large_size;
    }

    void lockAlloc() { lock_.lock(); }
    void unlockAlloc() { lock_.unlock(); }
};

} // namespace smash
