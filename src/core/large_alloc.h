// smash/src/core/large_alloc.h - mmap-backed large allocations (> kMaxSmallSize)
#pragma once

#include "smash/config.h"
#include "span.h"
#include "page_map.h"
#include "../util/spinlock.h"
#include "../util/bitops.h"
#include "../vm/platform_mem.h"

#include <cstddef>
#include <cstdint>

namespace smash {

class LargeAlloc {
    Spinlock lock_;
    PageMap* page_map_;

public:
    void init(PageMap* pm) {
        page_map_ = pm;
    }

    void* allocate(size_t size, size_t alignment) {
        if (alignment < kPageSize) alignment = kPageSize;

        // Round size up to page boundary
        size_t alloc_size = roundUp(size, kPageSize);

        // For large alignments, over-allocate and trim
        void* mem;
        size_t map_size;
        if (alignment <= kPageSize) {
            map_size = alloc_size;
            mem = vm::mapPages(map_size);
        } else {
            // Over-allocate by alignment to guarantee we can find an aligned region
            map_size = alloc_size + alignment;
            mem = vm::mapPages(map_size);
            if (!mem) return nullptr;

            auto addr = reinterpret_cast<uintptr_t>(mem);
            auto aligned_addr = roundUp(addr, alignment);
            size_t prefix = aligned_addr - addr;
            size_t suffix = map_size - prefix - alloc_size;

            // Unmap the prefix and suffix
            if (prefix > 0) vm::unmapPages(mem, prefix);
            if (suffix > 0) vm::unmapPages(reinterpret_cast<void*>(aligned_addr + alloc_size), suffix);

            mem = reinterpret_cast<void*>(aligned_addr);
            map_size = alloc_size;
        }

        if (!mem) return nullptr;

        uint32_t num_pages = static_cast<uint32_t>(alloc_size / kPageSize);

        Span* span = newSpanDescriptor();
        span->initLarge(mem, size, num_pages);

        LockGuard guard(lock_);
        page_map_->setRange(reinterpret_cast<uintptr_t>(mem), num_pages, span);
        return mem;
    }

    void deallocate(Span* span) {
        void* base = span->base;
        size_t map_size = static_cast<size_t>(span->page_count) * kPageSize;

        {
            LockGuard guard(lock_);
            page_map_->clearRange(reinterpret_cast<uintptr_t>(base), span->page_count);
        }

        vm::unmapPages(base, map_size);
    }

    size_t getSize(Span* span) const {
        return span->large_size;
    }

    void lockAlloc() { lock_.lock(); }
    void unlockAlloc() { lock_.unlock(); }
};

} // namespace smash
