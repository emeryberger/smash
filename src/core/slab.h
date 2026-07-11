// smash/src/core/slab.h - Per-size-class span manager
//
// Maintains lists of partial/full/empty spans for one size class.
// Thread cache drains/refills go through here.
#pragma once

#include "smash/config.h"
#include "span.h"
#include "page_map.h"
#include "size_classes.h"
#include "nursery_stats.h"
#include "../util/spinlock.h"
#include "../util/intrusive_list.h"
#include "../vm/platform_mem.h"
#include "../vm/vm_region.h"
#include "../vm/page_state.h"

#include <cstddef>
#include <cstdint>
#include <atomic>

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
    // Nursery instrumentation arrays (SMASH_NURSERY_STATS), owned by the
    // compressor, plumbed via setNurseryArrays(). Null when the gate is off.
    uint32_t* nursery_birth_epochs_ = nullptr;
    uint8_t*  nursery_cold_seen_ = nullptr;
    void (*release_hook_)(size_t, size_t, void*) = nullptr;
    void* release_ctx_ = nullptr;

    // ── Lock-free deferred free (Approach A) ────────────────────────────────
    // When a freeing thread finds lock_ contended, instead of blocking it pushes
    // its already-resolved free batch onto this MPSC Treiber stack and returns.
    // The next thread to acquire lock_ (a malloc refill, or a drain that wins
    // tryLock) drains and applies the whole stack first. Nodes live in bootstrap
    // memory (NOT threaded through user objects — that would put metadata on
    // data pages and break compression) and carry the span resolved AT FREE TIME
    // (page ranges recycle across size classes, so re-resolving later is wrong).
    // Modeled on VmRegion's decommit Treiber stack.
    struct PendingNode {
        void*  ptrs[kThreadCacheMaxPerClass];
        Span*  spans[kThreadCacheMaxPerClass];
        uint32_t count;
        PendingNode* next;
    };
    std::atomic<PendingNode*> pending_head_{nullptr};
    std::atomic<uint32_t> pending_depth_{0};   // node count, for back-pressure
    // Bootstrap node free-pool (recycled; steady-state allocation-free).
    Spinlock pending_pool_lock_;
    PendingNode* pending_pool_ = nullptr;

    // Above this many queued nodes, the next freer force-locks and drains rather
    // than pushing — bounds memory if a lock owner stalls.
    static constexpr uint32_t kPendingNodeCap = 64;

    PendingNode* acquirePendingNode() {
        {
            LockGuard g(pending_pool_lock_);
            if (pending_pool_) {
                PendingNode* n = pending_pool_;
                pending_pool_ = n->next;
                return n;
            }
        }
        return static_cast<PendingNode*>(
            BootstrapAlloc::instance().allocate(sizeof(PendingNode), alignof(PendingNode)));
    }

    void recyclePendingNode(PendingNode* n) {
        LockGuard g(pending_pool_lock_);
        n->next = pending_pool_;
        pending_pool_ = n;
    }

    // Push a resolved batch onto the pending stack (lock-free, MP producers).
    void pushPending(void** ptrs, Span** spans, size_t count) {
        PendingNode* n = acquirePendingNode();
        __builtin_memcpy(n->ptrs, ptrs, count * sizeof(void*));
        __builtin_memcpy(n->spans, spans, count * sizeof(Span*));
        n->count = static_cast<uint32_t>(count);
        PendingNode* head = pending_head_.load(std::memory_order_relaxed);
        do {
            n->next = head;
        } while (!pending_head_.compare_exchange_weak(
            head, n, std::memory_order_release, std::memory_order_relaxed));
        pending_depth_.fetch_add(1, std::memory_order_relaxed);
    }

    // Apply all queued deferred frees. CALLER MUST HOLD lock_ (single consumer).
    // Fast-exits on an empty stack with one relaxed load.
    void drainPending() SMASH_REQUIRES(lock_) {
        if (pending_head_.load(std::memory_order_relaxed) == nullptr) [[likely]] return;
        PendingNode* n = pending_head_.exchange(nullptr, std::memory_order_acquire);
        while (n) {
            PendingNode* next = n->next;
            for (uint32_t i = 0; i < n->count; ++i)
                if (n->spans[i]) deallocateOne(n->ptrs[i], n->spans[i]);
            pending_depth_.fetch_sub(1, std::memory_order_relaxed);
            recyclePendingNode(n);
            n = next;
        }
    }

    // Return one object to its span, updating the partial/full/empty lists.
    // Caller holds lock_. Shared by deallocateBatch (resolves spans itself)
    // and deallocateBatchResolved (spans pre-resolved by the caller).
    void deallocateOne(void* ptr, Span* span) {
        const bool was_full = span->full();
        span->deallocate(ptr);

        if (was_full) {
            full_.remove(span);
            partial_.pushFront(span);
        } else if (span->empty()) {
            // Q1: whole-span turnover. same_tid is approximate — a free applied
            // via the deferred (contended) path runs on a drain thread, not the
            // original freer, so those are misattributed; the uncontended path
            // (the common case) attributes correctly.
            nurseryOnTurnover(nurseryTid() == span->alloc_tid);
            partial_.remove(span);
            decommitEmptySpan(span);
            empty_.pushFront(span);
        }
    }

    Span* allocateNewSpan() {
        const auto& info = kSizeClasses[size_class_];
        size_t span_bytes = info.pages * kPageSize;
        void* mem;

        if (vm_region_) {
            mem = vm_region_->allocatePages(info.pages);
            if (!mem) return nullptr;
            if (page_states_) {
                size_t idx = vm_region_->pageIndex(reinterpret_cast<uintptr_t>(mem));
                page_states_->setRange(idx, info.pages, PageState::ACTIVE);
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
            // Page ranges recycle across spans: clear any stale cold streak
            // so the fresh span's pages classify as warm, not cooling.
            resetColdCounts(span);
        }
        nurseryStampBirth(span);
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
            page_states_->setRange(idx, span->page_count, PageState::EMPTY);
        }
    }

    // Clear the cold streak on every page of a span so it classifies as
    // warm. Needed when a span's pages (re)enter service — the cold-count
    // array is indexed by VM page and keeps whatever streak the previous
    // occupant left behind.
    void resetColdCounts(Span* span) {
        if (!cold_counts_ || !vm_region_) return;
        size_t idx = vm_region_->pageIndex(reinterpret_cast<uintptr_t>(span->base));
        __builtin_memset(cold_counts_ + idx, 0, span->page_count);
    }

    // Nursery instrumentation: stamp this span's birth (elapsed-allocation
    // epoch + allocating thread) when it (re)enters service, and reset the
    // per-page age/cold-seen arrays so the next cold transition dates from
    // this life. No-op when the gate is off.
    void nurseryStampBirth(Span* span) {
        if (!nurseryStatsOn()) return;
        span->birth_epoch = static_cast<uint32_t>(nurseryEpoch());
        span->alloc_tid = nurseryTid();
        nurseryOnSpanBirth();
        if (nursery_birth_epochs_ && vm_region_) {
            size_t idx = vm_region_->pageIndex(reinterpret_cast<uintptr_t>(span->base));
            for (uint32_t p = 0; p < span->page_count; ++p) {
                nursery_birth_epochs_[idx + p] = span->birth_epoch;
                if (nursery_cold_seen_) nursery_cold_seen_[idx + p] = 0;
            }
        }
    }

    // Restore page state when reusing a decommitted empty span.
    // Physical pages are zero-filled by the kernel on first access
    // (MADV_DONTNEED on Linux, MADV_FREE on macOS).
    void recommitEmptySpan(Span* span) {
        if (page_states_ && vm_region_) {
            size_t idx = vm_region_->pageIndex(reinterpret_cast<uintptr_t>(span->base));
            page_states_->setRange(idx, span->page_count, PageState::ACTIVE);
        }
        // The span sat idle in the empty list — its pages carry a stale cold
        // streak that would classify them as cooling and defeat the redirect
        // that chose this span precisely because it has nothing to protect.
        resetColdCounts(span);
        // Re-entering service counts as a fresh birth for age/turnover.
        nurseryStampBirth(span);
    }

public:
    // Plumb the compressor-owned nursery instrumentation arrays. Null-safe;
    // called once during init when SMASH_NURSERY_STATS is on.
    void setNurseryArrays(uint32_t* birth_epochs, uint8_t* cold_seen) {
        nursery_birth_epochs_ = birth_epochs;
        nursery_cold_seen_ = cold_seen;
    }

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
        drainPending();  // apply any deferred cross-thread frees first

        // Try partial spans first
        Span* span = partial_.front();
        if (span) {
            maybeWiden(span);
            Span::AllocFallback fb;
            void* ptr = span->allocateWarm(&fb);
            if (!ptr && fb.any()) {
                // The span's only free slots are on cooling/compressed pages.
                // Prefer a decommitted empty span: committing one of its
                // pages costs the same 4 KB the cooling page would keep
                // resident, and lets the cooling page finish cooling and
                // compress. Never allocate a NEW span for this — if the
                // empty list is dry, consume the fallback (no page-count
                // growth either way).
                if (Span* es = empty_.popFront()) {
                    // Rotate the warmless span to the back so the next
                    // allocation doesn't rescan it while it cools.
                    partial_.remove(span);
                    partial_.pushBack(span);
                    recommitEmptySpan(es);
                    partial_.pushFront(es);
                    span = es;
                    ptr = span->allocate();
                } else {
                    ptr = span->consumeFallback(fb);
                }
            }
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
            // Transition: partial → empty.  Move to empty list; the
            // compressor's zeroFreeSlots + compress cycle handles
            // reclamation. Immediate madvise here was a syscall per
            // span that destroyed throughput under batch-free workloads.
            partial_.remove(span);
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
        drainPending();  // apply any deferred cross-thread frees first
        // Advance the coarse elapsed-allocation clock once per refill batch.
        nurseryBumpEpoch();
        size_t allocated = 0;
        // One empty-span redirect per refill: when the front partial span
        // has only cooling/compressed free slots, switch to a decommitted
        // empty span instead of backfilling (same physical cost, and the
        // cooling pages get to compress). Bounded to one redirect so a
        // refill can't churn the empty list; after that, fallbacks are
        // consumed normally. Never allocates a NEW span for this.
        bool redirected = false;

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
                Span::AllocFallback fb;
                void* ptr = span->allocateWarm(&fb);
                if (!ptr && fb.any()) {
                    if (!redirected && empty_.front()) {
                        // Rotate the warmless span to the back so it cools
                        // undisturbed; the outer loop picks up the empty
                        // span we just promoted.
                        redirected = true;
                        partial_.remove(span);
                        partial_.pushBack(span);
                        Span* es = empty_.popFront();
                        recommitEmptySpan(es);
                        partial_.pushFront(es);
                        break;  // outer loop re-reads partial_.front()
                    }
                    ptr = span->consumeFallback(fb);
                }
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
            deallocateOne(ptrs[i], span);
        }
    }

    // Same as deallocateBatch but with spans already resolved by the caller
    // (ThreadCache::drain resolves each span once, then groups by arena). Skips
    // the per-pointer PageMap radix walk the plain deallocateBatch would repeat.
    void deallocateBatchResolved(void** ptrs, Span** spans, size_t count) {
        LockGuard guard(lock_);
        drainPending();
        for (size_t i = 0; i < count; ++i) {
            if (!spans[i]) continue;
            deallocateOne(ptrs[i], spans[i]);
        }
    }

    // Lock-free-friendly batch free (Approach A). If the slab lock is
    // uncontended, take it, drain any pending deferred frees, and apply this
    // batch directly. If contended, push the resolved batch onto the pending
    // stack and return without blocking — a future lock holder applies it. To
    // bound memory if a holder stalls, force-lock once the queue is deep.
    void deallocateBatchDeferred(void** ptrs, Span** spans, size_t count)
            SMASH_NO_TS_ANALYSIS {
        if (count == 0) return;
        if (pending_depth_.load(std::memory_order_relaxed) < kPendingNodeCap) {
            if (lock_.tryLock()) {
                drainPending();
                for (size_t i = 0; i < count; ++i)
                    if (spans[i]) deallocateOne(ptrs[i], spans[i]);
                lock_.unlock();
                return;
            }
            // Contended → defer without blocking.
            pushPending(ptrs, spans, count);
            return;
        }
        // Back-pressure: queue too deep, block to drain it and apply directly.
        LockGuard guard(lock_);
        drainPending();
        for (size_t i = 0; i < count; ++i)
            if (spans[i]) deallocateOne(ptrs[i], spans[i]);
    }

    // Return empty spans' pages to OS
    void scavenge() {
        LockGuard guard(lock_);
        drainPending();
        while (Span* span = empty_.popFront()) {
            releaseSpan(span);
        }
    }

    void setCapFn(CapFn fn, void* ctx) {
        cap_fn_ = fn;
        cap_ctx_ = ctx;
    }

    void lockSlab() SMASH_ACQUIRE(lock_) { lock_.lock(); }
    void unlockSlab() SMASH_RELEASE(lock_) { lock_.unlock(); }
};

} // namespace smash
