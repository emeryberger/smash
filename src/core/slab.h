// smash/src/core/slab.h - Per-size-class span manager
//
// Maintains lists of partial/full/empty spans for one size class.
// Thread cache drains/refills go through here.
#pragma once

#include "smash/config.h"
#include "span.h"
#include "page_map.h"
#include "size_classes.h"
#include "../util/spinlock.h"
#include "../util/intrusive_list.h"
#include "../vm/platform_mem.h"
#include "../vm/vm_region.h"
#include "../vm/page_state.h"

#include <cstddef>
#include <cstdint>

namespace smash {

class Slab {
    uint8_t size_class_;
    uint8_t arena_id_ = 0;
    Spinlock lock_;
    IntrusiveList<Span> partial_;
    IntrusiveList<Span> full_;
    IntrusiveList<Span> empty_;
    PageMap* page_map_;   // set during init, shared across all slabs
    VmRegion* vm_region_ = nullptr;
    PageStateTable* page_states_ = nullptr;
    void (*release_hook_)(size_t, size_t, void*) = nullptr;
    void* release_ctx_ = nullptr;

    Span* allocateNewSpan() {
        const auto& info = kSizeClasses[size_class_];
        size_t span_bytes = info.pages * kPageSize;
        void* mem;

        if (vm_region_) {
            mem = vm_region_->allocatePages(info.pages);
            if (!mem) return nullptr;
            if (page_states_) {
                size_t idx = vm_region_->pageIndex(reinterpret_cast<uintptr_t>(mem));
                for (uint32_t p = 0; p < info.pages; ++p)
                    page_states_->set(idx + p, PageState::ACTIVE);
            }
        } else {
            mem = vm::mapPages(span_bytes);
            if (!mem) return nullptr;
        }

        Span* span = newSpanDescriptor();
        span->init(mem, info.pages, size_class_, arena_id_);
        page_map_->setRange(reinterpret_cast<uintptr_t>(mem), info.pages, span);
        return span;
    }

    void releaseSpan(Span* span) {
        page_map_->clearRange(reinterpret_cast<uintptr_t>(span->base), span->page_count);
        if (vm_region_) {
            if (release_hook_) {
                size_t idx = vm_region_->pageIndex(reinterpret_cast<uintptr_t>(span->base));
                release_hook_(idx, span->page_count, release_ctx_);
            }
            vm_region_->releasePages(span->base, span->page_count);
        } else {
            vm::unmapPages(span->base, span->page_count * kPageSize);
        }
    }

public:
    void init(uint8_t sc, PageMap* pm,
              VmRegion* vr = nullptr, PageStateTable* ps = nullptr,
              void (*hook)(size_t, size_t, void*) = nullptr,
              void* hook_ctx = nullptr,
              uint8_t arena_id = 0) {
        size_class_ = sc;
        arena_id_ = arena_id;
        page_map_ = pm;
        vm_region_ = vr;
        page_states_ = ps;
        release_hook_ = hook;
        release_ctx_ = hook_ctx;
    }

    // Allocate one object from this size class. Caller must hold no locks.
    void* allocate() {
        LockGuard guard(lock_);

        // Try partial spans first
        Span* span = partial_.front();
        if (span) {
            void* ptr = span->allocate();
            if (span->full()) {
                partial_.remove(span);
                full_.pushFront(span);
            }
            return ptr;
        }

        // Try reusing an empty span
        span = empty_.popFront();
        if (!span) {
            // Allocate a fresh span
            span = allocateNewSpan();
            if (!span) return nullptr;
        }

        void* ptr = span->allocate();
        if (span->full()) {
            full_.pushFront(span);
        } else {
            partial_.pushFront(span);
        }
        return ptr;
    }

    // Free one object back to its span. Caller must hold no locks.
    void deallocate(Span* span, void* ptr) {
        LockGuard guard(lock_);

        bool was_full = span->full();
        span->deallocate(ptr);

        if (was_full) {
            // Transition: full → partial
            full_.remove(span);
            partial_.pushFront(span);
        } else if (span->empty()) {
            // Transition: partial → empty
            partial_.remove(span);
            empty_.pushFront(span);
        }
    }

    // Batch allocate: fill an array with up to `count` pointers.
    // Returns actual number allocated.
    size_t allocateBatch(void** out, size_t count) {
        LockGuard guard(lock_);
        size_t allocated = 0;

        while (allocated < count) {
            Span* span = partial_.front();
            if (!span) {
                span = empty_.popFront();
                if (!span) {
                    span = allocateNewSpan();
                    if (!span) break;
                }
                partial_.pushFront(span);
            }

            while (allocated < count) {
                void* ptr = span->allocate();
                if (!ptr) break;
                out[allocated++] = ptr;
            }

            if (span->full()) {
                partial_.remove(span);
                full_.pushFront(span);
            }
        }
        return allocated;
    }

    // Batch deallocate: return `count` pointers to their spans.
    void deallocateBatch(void** ptrs, size_t count) {
        LockGuard guard(lock_);
        for (size_t i = 0; i < count; ++i) {
            // Look up the span for each pointer
            Span* span = page_map_->get(reinterpret_cast<uintptr_t>(ptrs[i]));
            if (!span) continue;

            bool was_full = span->full();
            span->deallocate(ptrs[i]);

            if (was_full) {
                full_.remove(span);
                partial_.pushFront(span);
            } else if (span->empty()) {
                partial_.remove(span);
                empty_.pushFront(span);
            }
        }
    }

    // Return empty spans' pages to OS
    void scavenge() {
        LockGuard guard(lock_);
        while (Span* span = empty_.popFront()) {
            releaseSpan(span);
        }
    }

    void lockSlab() { lock_.lock(); }
    void unlockSlab() { lock_.unlock(); }
};

} // namespace smash
