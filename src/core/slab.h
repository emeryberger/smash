// smash/src/core/slab.h - Per-size-class span manager
//
// Maintains lists of partial/full/empty spans for one size class.
// Thread cache drains/refills go through here.
#pragma once

#include "smash/config.h"
#include <cstdlib>
#include <unistd.h>
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
#include <cstdlib>
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
    // SMASH_AGE_ORDER: spans that filled up once and have since had frees.
    // Never allocated from; a span leaves when it becomes empty.
    IntrusiveList<Span> aged_;
    std::atomic<uint32_t> resident_empty_count_{0};  // resident spans in empty_
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

    // ── Magazine depot (lock-free transfer cache, à la tcmalloc) ─────────────
    // A Treiber stack of FULL magazines (reusing PendingNode as the magazine).
    // On free, a resolved batch is pushed here as a magazine. On refill, a
    // magazine is popped and its pointers handed straight back to the thread
    // cache — WITHOUT taking lock_, touching the bitmap, or updating span lists.
    // Objects circulate thread → depot → thread; the span bitmap keeps them
    // marked "allocated" the whole time (correct: they never logically returned
    // to the span, so the compressor won't zero/compress their slots, and
    // allocated_count stays accurate). This closes the alloc/free churn loop
    // lock-free — the slab lock_ is only hit when the depot is empty (genuine
    // growth) or on the eventual drain to reclaim spans. ABA-safe via a tagged
    // head (counter in the high 16 bits of the pointer word is not portable, so
    // we use a separate generation via a 128-bit-free scheme: the magazine node
    // is never freed back to a shared pool while linked, and pop re-validates).
    // Gated by SMASH_MAGAZINE (default off) for clean A/B.
    //
    // SINGLE global depot, guarded by a dedicated tiny lock whose critical
    // section is an O(1) list swing (NOT the bitmap walk / span-list churn that
    // the main slab lock_ does). Objects circulate thread → depot → thread and
    // never touch lock_/bitmap on the common path.
    //
    // NOTE (2026-07-18): a per-CPU-SHARDED depot was prototyped and measured —
    // it LOST to this single depot (96t: sharded 24.0 vs single 27.9 MOPS).
    // Sharding by running CPU removed the (already tiny) mag_lock_ contention but
    // scattered freed magazines across 64 stacks, dropping the refill hit rate;
    // wider work-stealing (16 probes) made it worse still. The single mag_lock_
    // critical section (~5 instructions) was never the bottleneck, so trading
    // hit-rate for lock-sharding is a net loss here. Single depot kept.
    PendingNode* mag_head_ = nullptr;
    uint32_t mag_count_ = 0;
    Spinlock mag_lock_;
    static constexpr uint32_t kMaxMagazines = 64;   // depot cap: bounds the held
        // (marked-allocated, not-yet-returned) set to ~64·256·8 = 128 KB. An
        // UNCAPPED depot livelocked at 32t — held objects + thread caches held
        // the whole live set, exhausting spans. Cap is load-bearing.

    // Push a resolved batch as a full magazine (free path). false → depot full
    // (caller falls back to the real free path so memory stays reclaimable).
    bool magazinePush(void** ptrs, Span** spans, size_t count) {
        PendingNode* n = acquirePendingNode();
        __builtin_memcpy(n->ptrs, ptrs, count * sizeof(void*));
        __builtin_memcpy(n->spans, spans, count * sizeof(Span*));
        n->count = static_cast<uint32_t>(count);
        {
            LockGuard g(mag_lock_);
            if (mag_count_ >= kMaxMagazines) {
                recyclePendingNode(n);
                return false;
            }
            n->next = mag_head_;
            mag_head_ = n;
            ++mag_count_;
        }
        return true;
    }

    // Pop a full magazine (refill path). Returns its count, or 0 if empty.
    size_t magazinePop(void** out_ptrs) {
        PendingNode* n = nullptr;
        {
            LockGuard g(mag_lock_);
            n = mag_head_;
            if (!n) return 0;
            mag_head_ = n->next;
            --mag_count_;
        }
        size_t count = n->count;
        __builtin_memcpy(out_ptrs, n->ptrs, count * sizeof(void*));
        recyclePendingNode(n);
        return count;
    }

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
        if (densityEnabled()) return deallocateOneDensity(ptr, span);
        const bool was_full = span->full();
        const bool reuse = ageOrderEnabled() && span->aged && hotHoleReuse(span, ptr);
        span->deallocate(ptr);

        if (was_full) {
            full_.remove(span);
            if (ageOrderEnabled() && !reuse) {
                span->aged = true;
                if (!span->empty()) { aged_.pushFront(span); return; }
                span->aged = false;
                retireEmptySpan(span);
                empty_.pushFront(span);
                return;
            }
            if (ageOrderEnabled()) span->aged = false;
            partial_.pushFront(span);
        } else if (span->empty()) {
            if (ageOrderEnabled() && span->aged) { aged_.remove(span); span->aged = false; }
            else partial_.remove(span);
            retireEmptySpan(span);
            empty_.pushFront(span);
        } else if (reuse) {
            // SMASH_AGE_ORDER=2: a hole on a warm page of an aged span. The
            // page is resident anyway, so let new objects fill it: back to
            // partial_ (allocateWarm still skips its cooling/compressed pages).
            aged_.remove(span);
            span->aged = false;
            partial_.pushFront(span);
        }
    }

    // A span just became empty. Its pages leave compressor tracking now
    // (release hook, EMPTY state), but the madvise that drops them is
    // deferred: churn that empties and refills spans (free a batch, allocate
    // a batch) otherwise paid a syscall + TLB shootdown per span plus a zero
    // page fault per page on reuse. A resident empty span is reused as-is, or
    // decommitted by purgeResidentEmpty() once it has sat empty for
    // kResidentEmptyTicks compressor ticks. Process-wide resident empties are
    // capped at residentEmptyMaxBytes(); past that, decommit immediately.
    static constexpr uint32_t kResidentEmptyTicks = 2;
    // Default cap: 1/32 of physical memory, clamped to [64 MiB, 1 GiB]. A
    // batch workload that frees and re-allocates a working set larger than
    // the cap pays decommit + refault on the excess every round (measured on
    // a 260 MB set: 32 MiB cap 2.5 s, 512 MiB 0.75 s, glibc 2.3 s), while
    // the retained spans are reused, so peak RSS is unchanged. The cost is a
    // transient: freed spans of one class stay resident for up to
    // kResidentEmptyTicks ticks while another class grows.
    // SMASH_EMPTY_RESIDENT_MB=N overrides it (0 = decommit at once).
    static size_t residentEmptyMaxBytesInit() {
        if (const char* e = std::getenv("SMASH_EMPTY_RESIDENT_MB"))
            return static_cast<size_t>(std::atol(e)) << 20;
        long pages = sysconf(_SC_PHYS_PAGES);
        long psz = sysconf(_SC_PAGESIZE);
        size_t ram = (pages > 0 && psz > 0) ? static_cast<size_t>(pages) * static_cast<size_t>(psz)
                                            : (size_t{2} << 30);
        size_t cap = ram / 32;
        if (cap < (size_t{64} << 20)) cap = size_t{64} << 20;
        if (cap > (size_t{1} << 30)) cap = size_t{1} << 30;
        return cap;
    }
    static size_t residentEmptyMaxBytes() {
        static std::atomic<size_t> cached{~size_t{0}};
        size_t v = cached.load(std::memory_order_relaxed);
        if (v == ~size_t{0}) [[unlikely]] {
            v = residentEmptyMaxBytesInit();
            cached.store(v, std::memory_order_relaxed);
        }
        return v;
    }
    static inline std::atomic<size_t> resident_empty_bytes_{0};
    static inline std::atomic<uint32_t> slab_tick_{0};

    void retireEmptySpan(Span* span) {
        const size_t bytes = span->page_count * kPageSize;
        if (resident_empty_bytes_.load(std::memory_order_relaxed) + bytes > residentEmptyMaxBytes()) {
            decommitEmptySpan(span);
            return;
        }
        if (release_hook_ && vm_region_) {
            size_t idx = vm_region_->pageIndex(reinterpret_cast<uintptr_t>(span->base));
            release_hook_(idx, span->page_count, release_ctx_);
        }
        if (page_states_ && vm_region_) {
            size_t idx = vm_region_->pageIndex(reinterpret_cast<uintptr_t>(span->base));
            page_states_->setRange(idx, span->page_count, PageState::EMPTY);
        }
        span->resident_empty = true;
        span->empty_tick = slab_tick_.load(std::memory_order_relaxed);
        resident_empty_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        resident_empty_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void clearResident(Span* span) {
        if (!span->resident_empty) return;
        span->resident_empty = false;
        resident_empty_bytes_.fetch_sub(span->page_count * kPageSize, std::memory_order_relaxed);
        resident_empty_count_.fetch_sub(1, std::memory_order_relaxed);
    }

public:
    static void advanceTick() { slab_tick_.fetch_add(1, std::memory_order_relaxed); }
    static size_t residentEmptyBytes() { return resident_empty_bytes_.load(std::memory_order_relaxed); }

    // Compressor tick: decommit empty spans that have stayed empty for
    // kResidentEmptyTicks ticks. Cheap when nothing is resident.
    void purgeResidentEmpty() {
        if (resident_empty_count_.load(std::memory_order_relaxed) == 0) return;
        const uint32_t now = slab_tick_.load(std::memory_order_relaxed);
        LockGuard guard(lock_);
        for (Span* sp = empty_.front(); sp; sp = sp->list_next) {
            if (sp->resident_empty && now - sp->empty_tick >= kResidentEmptyTicks) {
                clearResident(sp);
                vm::decommitPages(sp->base, sp->page_count * kPageSize);
                vm::uffdZeroFill(sp->base, sp->page_count * kPageSize);
            }
        }
    }
private:

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
        page_map_->setRange(reinterpret_cast<uintptr_t>(mem), info.pages, span);
        return span;
    }

    void releaseSpan(Span* span) {
        clearResident(span);
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
        vm::uffdZeroFill(span->base, bytes);
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

    // Restore page state when reusing a decommitted empty span.
    // Physical pages are zero-filled by the kernel on first access
    // (MADV_DONTNEED on Linux, MADV_FREE on macOS).
    void recommitEmptySpan(Span* span) {
        clearResident(span);
        if (page_states_ && vm_region_) {
            size_t idx = vm_region_->pageIndex(reinterpret_cast<uintptr_t>(span->base));
            page_states_->setRange(idx, span->page_count, PageState::ACTIVE);
        }
        // The span sat idle in the empty list — its pages carry a stale cold
        // streak that would classify them as cooling and defeat the redirect
        // that chose this span precisely because it has nothing to protect.
        resetColdCounts(span);
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
        drainPending();  // apply any deferred cross-thread frees first

        // Try partial spans first
        Span* span = partial_.front();
        if (span) {
            maybeWiden(span);
            Span::AllocFallback fb;
            void* ptr = span->allocateWarm(&fb);
            if (!ptr && fb.any() && tapEnabled() &&
                span->fallbackPageLivePct(fb) >= tap_sparse_pct_) {
                // TAP: never fill a cold, non-sparse hole; use an empty or
                // new span instead (no partial scan on this rare path).
                Span* ns = empty_.popFront();
                if (ns) recommitEmptySpan(ns);
                else ns = allocateNewSpan();
                if (ns) {
                    partial_.remove(span);
                    partial_.pushBack(span);
                    void* p = ns->allocate();
                    if (ns->full()) pushFull(ns); else partial_.pushFront(ns);
                    return p;
                }
            }
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
            if (!ptr && densityEnabled() && !span->full()) {
                parkSpan(span);   // only dense-page holes; use another span
                return allocate();
            }
            if (span->full()) {
                partial_.remove(span);
                pushFull(span);
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
            pushFull(span);
        } else {
            partial_.pushFront(span);
        }
        return ptr;
    }

    // Free one object back to its span. Caller must hold no locks.
    void deallocate(Span* span, void* ptr) {
        LockGuard guard(lock_);

        bool was_full = span->full();
        const bool reuse = ageOrderEnabled() && span->aged && hotHoleReuse(span, ptr);
        span->deallocate(ptr);

        if (was_full) {
            // Transition: full → partial (or → aged under SMASH_AGE_ORDER)
            full_.remove(span);
            if (ageOrderEnabled() && !reuse) {
                span->aged = true;
                if (!span->empty()) { aged_.pushFront(span); return; }
                span->aged = false;
                empty_.pushFront(span);
                return;
            }
            if (ageOrderEnabled()) span->aged = false;
            partial_.pushFront(span);
        } else if (ageOrderEnabled() && span->empty() && span->aged) {
            aged_.remove(span);
            span->aged = false;
            empty_.pushFront(span);
        } else if (reuse) {
            aged_.remove(span);
            if (ageOrderEnabled()) span->aged = false;
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
        size_t allocated = 0;
        // One empty-span redirect per refill: when the front partial span
        // has only cooling/compressed free slots, switch to a decommitted
        // empty span instead of backfilling (same physical cost, and the
        // cooling pages get to compress). Bounded to one redirect so a
        // refill can't churn the empty list; after that, fallbacks are
        // consumed normally. Never allocates a NEW span for this.
        bool redirected = false;
        int tap_scans = 0;
        bool parked_now = false;

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
                if (!ptr && fb.any() && tapEnabled() &&
                    span->fallbackPageLivePct(fb) >= tap_sparse_pct_) {
                    // TAP: don't fill a hole on a cold, non-sparse page.
                    // Rotate this span back and try others, then an empty
                    // span, then a new one (outer loop re-reads front()).
                    partial_.remove(span);
                    partial_.pushBack(span);
                    if (++tap_scans <= kTapScan && partial_.front() != span) break;
                    Span* ns = empty_.popFront();
                    if (ns) recommitEmptySpan(ns);
                    else ns = allocateNewSpan();
                    if (!ns) { ptr = span->consumeFallback(fb); }  // out of memory: fall back
                    else { partial_.pushFront(ns); tap_scans = 0; break; }
                }
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
                if (!ptr && !fb.any() && densityEnabled() && !span->full()) {
                    parkSpan(span);   // only dense-page holes left
                    parked_now = true;
                    break;
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

            if (parked_now) { parked_now = false; continue; }
            if (span->full()) {
                partial_.remove(span);
                pushFull(span);
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

    // SMASH_AGE_ORDER=1 (default off): age-ordered placement, after
    // Archipelago. A span that has filled once never has its freed slots
    // handed out again until every object in it is gone, so new objects go
    // only to the allocation frontier (a never-filled span) or to empty
    // spans, and objects written at the same time share pages. Old spans
    // are left with holes; the compressor zeroes free slots, so when such a
    // span cools its holes compress to almost nothing. Frees into aged spans
    // also bypass the thread cache (SmashHeap::free), which would otherwise
    // hand an old slot straight to the next allocation of that size — e.g.
    // a Redis overwrite placing the new value where the old one was.
    //
    // SMASH_AGE_ORDER=2 adds hot-hole reuse: a hole on a WARM page of an aged
    // span (not cooling, not compressed — Span::classifySlotPage) is reused
    // normally, since that page is resident whether or not the hole is
    // filled. Only holes on cooling/cold pages stay empty, to compress away.
    // Captures the Mesh-like win for hot sparse pages without remapping.
    //
    // Plain flags (no function-local static guard on the free/drain paths),
    // set once by initAgeOrder() at heap init, before any slab is used.
    static inline bool age_order_ = false;
    static inline bool age_hot_reuse_ = false;
    // SMASH_AGE_ORDER=3: temperature-aware placement (TAP). No aged spans.
    // (1) a free on a cooling/compressed page bypasses the thread cache
    //     (SmashHeap::free), so the slot is not handed straight back out —
    //     the path by which a rewrite plants a hot object on a cold page;
    // (2) when a span's only free slots are on cooling/compressed pages,
    //     allocation tries up to kTapScan other partial spans, then an empty
    //     span, then a NEW span, instead of filling the cold hole;
    // (3) except when that page is under SMASH_AGE_SPARSE_PCT (default 25)
    //     percent live: re-heating a nearly empty page costs little and
    //     refilling it bounds growth.
    // Warm pages behave exactly as without the flag, so churn reuses slots.
    static inline bool tap_ = false;
    static inline int tap_sparse_pct_ = 25;
    static constexpr int kTapScan = 4;
    // Default: TAP (3). SMASH_AGE_ORDER=0 turns placement policy off.
    static void initAgeOrder() {
        const char* v = std::getenv("SMASH_AGE_ORDER");
        if (!v || !v[0]) v = "3";
        age_order_ = v && (v[0] == '1' || v[0] == '2');
        age_hot_reuse_ = v && v[0] == '2';
        tap_ = v && v[0] == '3';
        if (const char* p = std::getenv("SMASH_AGE_SPARSE_PCT")) tap_sparse_pct_ = std::atoi(p);
        density_ = v && v[0] == '4';
        int pct = 50;
        if (const char* p = std::getenv("SMASH_AGE_DENSE_PCT")) pct = std::atoi(p);
        if (pct < 1) pct = 1;
        if (pct > 100) pct = 100;
        Span::dense_bytes_ = density_ ? static_cast<uint32_t>(kPageSize * pct / 100) : 0;
    }
    // SMASH_AGE_ORDER=4: density-based age order. A span is aged once it has
    // been full (pushFull); in an aged span a free slot is reusable only if
    // its page is below SMASH_AGE_DENSE_PCT (default 50) percent live
    // (Span::slotOnDensePage), whatever the page's temperature. Frees that
    // leave a dense page bypass the thread cache. An aged span with no
    // reusable slot is parked on aged_ (off the allocation list) until a free
    // makes one of its pages sparse. Growth is bounded by ~1/threshold; the
    // holes are zeroed by the compressor, so cold dense pages compress them
    // away. Unlike TAP, an application write to an old object (Redis's LRU
    // update on lookup) cannot make its page eligible again.
    static inline bool density_ = false;
    static bool densityEnabled() { return density_; }
    // Free routing: true if, after freeing `ptr`, its page stays dense.
    static bool densityKeepHole(const Span* span, const void* ptr) {
        if (!span->aged) return false;
        const size_t off = static_cast<size_t>(
            static_cast<const char*>(ptr) - static_cast<const char*>(span->base));
        const size_t live = span->pageLiveBytes(off / kPageSize * kPageSize);
        const size_t obj = span->object_size < kPageSize ? span->object_size : kPageSize;
        return live >= obj && live - obj >= Span::dense_bytes_;
    }
    static bool tapEnabled() { return tap_; }
    // TAP free routing: true if `ptr`'s page is not warm (cooling/compressed).
    static bool tapColdSlot(const Span* span, const void* ptr) {
        const size_t off = static_cast<size_t>(
            static_cast<const char*>(ptr) - static_cast<const char*>(span->base));
        return span->classifySlotPage(off, getCoolingCloseTicks()) !=
               Span::SlotPageClass::WARM;
    }
    // These checks used to sit behind a compile-time gate
    // (SMASH_PLACEMENT_EXPERIMENTS) on a measured 4-16% Larson cost. Re-measured
    // 2026-11-13 (x86 c7a, 1/4/8 threads, 5 reps): larson and
    // linux-scalability within +-2%; threadtest is multimodal in every build,
    // master included. They are plain runtime flags now.
    static bool ageOrderEnabled() { return age_order_; }
    // True if a free of `ptr` (in an aged span) should go the normal way.
    static bool hotHoleReuse(const Span* span, const void* ptr) {
        if (!age_hot_reuse_) return false;
        const size_t off = static_cast<size_t>(
            static_cast<const char*>(ptr) - static_cast<const char*>(span->base));
        return span->classifySlotPage(off, getCoolingCloseTicks()) ==
               Span::SlotPageClass::WARM;
    }

    // SMASH_AGE_ORDER=4 list maintenance (caller holds lock_). span->parked
    // means "on aged_": aged, not full, no reusable slot.
    void deallocateOneDensity(void* ptr, Span* span) {
        const bool was_full = span->full();
        const size_t off = static_cast<size_t>(
            static_cast<char*>(ptr) - static_cast<char*>(span->base));
        span->deallocate(ptr);
        if (span->empty()) {
            if (was_full) full_.remove(span);
            else if (span->parked) aged_.remove(span);
            else partial_.remove(span);
            span->parked = false;
            span->aged = false;
            retireEmptySpan(span);
            empty_.pushFront(span);
            return;
        }
        const bool sparse = !span->aged ||
            span->pageLiveBytes(off / kPageSize * kPageSize) < Span::dense_bytes_;
        if (was_full) {
            full_.remove(span);
            if (sparse) partial_.pushFront(span);
            else { span->parked = true; aged_.pushFront(span); }
        } else if (span->parked && sparse) {
            aged_.remove(span);
            span->parked = false;
            partial_.pushFront(span);
        }
    }
    // In partial_ but nothing reusable left (all free slots on dense pages).
    void parkSpan(Span* span) {
        partial_.remove(span);
        span->parked = true;
        aged_.pushFront(span);
    }

    // A span just filled. Under SMASH_AGE_ORDER it is aged from now on, so its
    // frees bypass the thread cache instead of being handed straight back out.
    void pushFull(Span* span) {
        if (ageOrderEnabled() || densityEnabled()) span->aged = true;
        full_.pushFront(span);
    }

    // Free one object straight to its (aged) span, bypassing the thread cache.
    void deallocateDirect(Span* span, void* ptr) {
        LockGuard guard(lock_);
        drainPending();
        deallocateOne(ptr, span);
    }

    static bool magazineEnabled() {
        static const bool on = [] {
            const char* v = std::getenv("SMASH_MAGAZINE");
            return v && v[0] == '1';
        }();
        return on;
    }

    // ── Magazine (transfer-cache) public entry points ───────────────────────
    // Refill: try to satisfy the whole batch from one popped magazine (no lock_,
    // no bitmap). Returns count handed back, or 0 if the depot is empty (caller
    // falls back to allocateBatch). Only valid when magazineEnabled().
    size_t allocateBatchMag(void** out) {
        return magazinePop(out);
    }

    // Free: push a resolved full-lane batch as a magazine (no lock_, no bitmap).
    // Returns true if pushed; false if the depot is full (caller falls back to
    // the real free path so memory is still reclaimable). Spans are carried so a
    // later depot drain (on shutdown / span reclaim) can return them properly.
    bool deallocateBatchMag(void** ptrs, Span** spans, size_t count) {
        return magazinePush(ptrs, spans, count);
    }

    // Drain the whole depot back through the real free path (bitmap + span
    // lists). Called under lock_ when we need spans reclaimable (scavenge) or at
    // shutdown. Objects held in magazines were never returned to their spans, so
    // this is where their bitmap bits finally clear.
    void drainMagazines() SMASH_REQUIRES(lock_) {
        for (;;) {
            PendingNode* n = nullptr;
            {
                LockGuard g(mag_lock_);
                n = mag_head_;
                if (!n) break;
                mag_head_ = n->next;
                --mag_count_;
            }
            for (uint32_t i = 0; i < n->count; ++i)
                if (n->spans[i]) deallocateOne(n->ptrs[i], n->spans[i]);
            recyclePendingNode(n);
        }
    }

    // Return empty spans' pages to OS
    void scavenge() {
        LockGuard guard(lock_);
        drainPending();
        if (magazineEnabled()) drainMagazines();
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
    // fork(): the slab's leaf locks (magazine transfer, pending-node pool).
    // Taken after every slab lock_ (SmashHeap::lock), released before.
    void lockLeafForFork() SMASH_NO_TS_ANALYSIS { mag_lock_.lock(); pending_pool_lock_.lock(); }
    void unlockLeafForFork() SMASH_NO_TS_ANALYSIS { pending_pool_lock_.unlock(); mag_lock_.unlock(); }
};

} // namespace smash
