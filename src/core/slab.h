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
    // Per-slab per-page slot cap (0 = no cap).  Set by SmashHeap during init
    // based on cold vs hot sub-arena identity (C1).
    uint32_t max_slots_per_page_ = 0;
    // Adaptive cap callback.  When set, overrides max_slots_per_page_ per
    // new-span allocation; lets SmashHeap revise the cap as it accumulates
    // compression/decompression feedback for this (arena, sc) bucket.
    using CapFn = uint32_t(*)(void* ctx, uint8_t arena, uint8_t sc);
    CapFn cap_fn_ = nullptr;
    void* cap_ctx_ = nullptr;
    Spinlock lock_;
    IntrusiveList<Span> partial_;
    IntrusiveList<Span> full_;
    IntrusiveList<Span> empty_;
    PageMap* page_map_;   // set during init, shared across all slabs
    VmRegion* vm_region_ = nullptr;
    PageStateTable* page_states_ = nullptr;
    // Per-page cold-tick counter array owned by CompressorThread.  Plumbed
    // into Span so the bitmap walk can clear the cold counter on the page
    // it just handed out a chunk on — pages still receiving allocations
    // should not be eligible for compression yet.
    uint8_t* cold_counts_ = nullptr;
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
        span->init(mem, info.pages, size_class_, arena_id_, currentCap());
        // Plumb the page-state lookup so Span::allocate() can avoid handing
        // out chunks on COMPRESSED pages (would fault on first user access),
        // and the cold-count array so allocate() can reset coldness on the
        // page it just handed a chunk out on.
        if (vm_region_ && page_states_) {
            span->page_states = page_states_;
            span->first_page_vm_idx = static_cast<uint32_t>(
                vm_region_->pageIndex(reinterpret_cast<uintptr_t>(mem)));
            span->cold_counts = cold_counts_;
        }
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

    // Decommit pages of an empty span via MADV_DONTNEED/MADV_FREE, releasing
    // physical memory immediately without waiting for the compressor to
    // discover, zero, and compress them.  The span stays in the empty list
    // for reuse; recommitEmptySpan() restores page state on reuse.
    void decommitEmptySpan(Span* span) {
        if (release_hook_ && vm_region_) {
            size_t idx = vm_region_->pageIndex(reinterpret_cast<uintptr_t>(span->base));
            release_hook_(idx, span->page_count, release_ctx_);
        }
        size_t bytes = span->page_count * kPageSize;
        vm::decommitPages(span->base, bytes);
        if (page_states_ && vm_region_) {
            size_t idx = vm_region_->pageIndex(reinterpret_cast<uintptr_t>(span->base));
            for (uint32_t p = 0; p < span->page_count; ++p)
                page_states_->set(idx + p, PageState::EMPTY);
        }
    }

    // Restore page state when reusing a decommitted empty span.
    // Physical pages are zero-filled by the kernel on first access
    // (MADV_DONTNEED on Linux, MADV_FREE on macOS).
    void recommitEmptySpan(Span* span) {
        if (page_states_ && vm_region_) {
            size_t idx = vm_region_->pageIndex(reinterpret_cast<uintptr_t>(span->base));
            for (uint32_t p = 0; p < span->page_count; ++p)
                page_states_->set(idx + p, PageState::ACTIVE);
        }
    }

public:
    void init(uint8_t sc, PageMap* pm,
              VmRegion* vr = nullptr, PageStateTable* ps = nullptr,
              void (*hook)(size_t, size_t, void*) = nullptr,
              void* hook_ctx = nullptr,
              uint8_t arena_id = 0,
              uint32_t max_slots_per_page = 0,
              uint8_t* cold_counts = nullptr) {
        size_class_ = sc;
        arena_id_ = arena_id;
        max_slots_per_page_ = max_slots_per_page;
        page_map_ = pm;
        vm_region_ = vr;
        page_states_ = ps;
        cold_counts_ = cold_counts;
        release_hook_ = hook;
        release_ctx_ = hook_ctx;
    }

    // Current per-page cap for new spans (or for widening existing ones).
    // Uses cap_fn_ if installed, else the static max_slots_per_page_.
    uint32_t currentCap() const {
        return cap_fn_ ? cap_fn_(cap_ctx_, arena_id_, size_class_)
                       : max_slots_per_page_;
    }

    // Widen a partial span's bitmap if the bucket's cap has relaxed since
    // this span was initialized.  Lets a bucket mis-classified as cold
    // recover once decomp evidence reveals it's actually hot, without
    // forcing us to discard spans or pre-allocate fresh ones.
    void maybeWiden(Span* span) {
        if (!cap_fn_ || span->current_cap_per_page == 0) return;
        uint32_t cur_cap = currentCap();
        if (cur_cap == 0 || cur_cap > span->current_cap_per_page) {
            span->widenCap(cur_cap);
        }
    }

    // Allocate one object from this size class. Caller must hold no locks.
    void* allocate() {
        LockGuard guard(lock_);

        // Try partial spans first
        Span* span = partial_.front();
        if (span) {
            maybeWiden(span);
            void* ptr = span->allocate();
            if (span->full()) {
                partial_.remove(span);
                full_.pushFront(span);
            }
            return ptr;
        }

        // Try reusing an empty span (pages were decommitted)
        span = empty_.popFront();
        if (!span) {
            // Allocate a fresh span
            span = allocateNewSpan();
            if (!span) return nullptr;
        } else {
            recommitEmptySpan(span);
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
            // Transition: partial → empty.  Immediately decommit pages
            // via MADV_DONTNEED to release physical memory without waiting
            // for the compressor to zero and compress them.
            partial_.remove(span);
            decommitEmptySpan(span);
            empty_.pushFront(span);
        }
    }

    // Batch allocate: fill an array with up to `count` pointers.
    // Returns actual number allocated.
    //
    // When kPageLocalBatch is true, the inner loop stops as soon as the
    // next allocated object would lie on a different VM page than the
    // batch's first object.  This keeps each thread-cache refill confined
    // to one page, so objects allocated in a single burst share a page
    // and (under Pareto-skew access) tend to cool together.
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
                } else {
                    recommitEmptySpan(span);
                }
                partial_.pushFront(span);
            }
            maybeWiden(span);

            uintptr_t first_page = 0;
            bool first_in_batch = (allocated == 0);
            while (allocated < count) {
                void* ptr = span->allocate();
                if (!ptr) break;
                if (kPageLocalBatch) {
                    uintptr_t p = reinterpret_cast<uintptr_t>(ptr) & ~(kPageSize - 1);
                    if (first_in_batch) {
                        first_page = p;
                        first_in_batch = false;
                    } else if (p != first_page) {
                        // Crossed a page boundary.  Return the object and stop
                        // — next refill will anchor on a fresh page.
                        span->deallocate(ptr);
                        return allocated;
                    }
                }
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
                decommitEmptySpan(span);
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

    void setCapFn(CapFn fn, void* ctx) {
        cap_fn_ = fn;
        cap_ctx_ = ctx;
    }

    void lockSlab() { lock_.lock(); }
    void unlockSlab() { lock_.unlock(); }
};

} // namespace smash
