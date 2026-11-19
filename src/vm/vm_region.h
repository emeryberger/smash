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

#include <new>
#include <sys/mman.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include "smash/config.h"
#include "platform_mem.h"
#include "page_state.h"
#include "../core/bootstrap_alloc.h"
#include "../util/waker.h"
#include "../util/spinlock.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace smash {

// Forward declaration so VmRegion can store an atomic<Span*>* without pulling
// in the full Span definition (avoids circular include with core/span.h).
struct Span;

class VmRegion {
    // Mode determined at init time
    bool tracking_mode_ = false;

    // ── Full mode: contiguous allocation ────────────────────────────────────
    char* base_ = nullptr;
    size_t total_pages_ = 0;
    size_t contig_pages_ = 0;  // = region_size / kPageSize in full mode; 0 in tracking mode
    std::atomic<size_t> next_page_{0};  // bump pointer (normal) or next index (tracking)

    // ── Chunked commit watermark ────────────────────────────────────────────
    // The arena is reserved PROT_NONE; pages must be mprotect'd to PROT_RW
    // before use. Committing per span bump means one mprotect per span, and
    // every mprotect takes the kernel's mmap_lock for write — under a workload
    // that bump-allocates a large live set (e.g. Hoard linux-scalability holds
    // ~1.3 GB live) those serialize on osq_lock and dominate CPU. Instead we
    // commit in large chunks ahead of the bump pointer: committed_pages_ is the
    // high-water page index already PROT_RW, advanced under commit_lock_ in
    // kCommitChunkPages steps. A bump only mprotects when it crosses the
    // watermark, turning millions of mprotects into a few hundred. Behavior is
    // preserved: mprotect on an untouched PROT_NONE anonymous range only flips
    // VMA protection; no physical page is backed until first write, so RSS is
    // unchanged (the kernel still faults pages in lazily on touch).
    std::atomic<size_t> committed_pages_{0};
    Spinlock commit_lock_;

    struct FreeRun {
        size_t page_index;
        size_t page_count;
        FreeRun* next;
    };

    // Pages committed per watermark advance (see committed_pages_).
    //
    // DEFAULT 0 = OFF (commit exactly the requested pages, per-call). Chunked
    // commit was measured to be net-negative: on a single-threaded bump-growth
    // workload (Hoard linux-scalability) it cuts the per-span mprotect/mmap_lock
    // cost ~25%, but it REGRESSES realloc churn (phong +90%) and, worst, holding
    // the global commit_lock_ across a multi-MiB mprotect serializes high
    // thread counts — Larson at 64 threads collapsed ~10x (47M→5M ops/s) with
    // chunking on. The win is narrow and the downside is a scalability cliff, so
    // it ships off. Opt in per-workload via SMASH_COMMIT_CHUNK_KIB=N.
    static constexpr size_t kCommitChunkPages = 0;

    // Sharded free lists to reduce contention under high thread counts.
    // 64 shards = one per typical cache line, threads hash by ID.
    static constexpr size_t kNumFreeShards = 64;
    static constexpr size_t kFreeShardMask = kNumFreeShards - 1;

    struct alignas(64) FreeShard {  // cache-line aligned
        Spinlock lock;
        // free_list AND free_pool are the two singly-linked stacks a shard
        // owns; BOTH must only ever be touched while `lock` is held. An
        // unlocked pop of free_pool from the decommit thread (vs a locked push
        // from allocatePages) once corrupted the stack into a cycle → spin-
        // livelock (fixed 2026-06-25, model/SmashFreePoolRace). GUARDED_BY lets
        // clang -Wthread-safety re-prove that invariant at compile time so the
        // regression can't silently return.
        FreeRun* free_list SMASH_GUARDED_BY(lock) = nullptr;
        FreeRun* free_pool SMASH_GUARDED_BY(lock) = nullptr;
    };
    FreeShard shards_[kNumFreeShards];

    static size_t getShardIdx() {
        // Hash thread ID to shard. Cache per-thread for speed.
        thread_local size_t idx = std::hash<std::thread::id>{}(
            std::this_thread::get_id()) & kFreeShardMask;
        return idx;
    }

    FreeRun* newFreeRun(FreeShard& shard) SMASH_REQUIRES(shard.lock) {
        if (shard.free_pool) {
            FreeRun* r = shard.free_pool;
            shard.free_pool = r->next;
            return r;
        }
        return static_cast<FreeRun*>(
            BootstrapAlloc::instance().allocate(sizeof(FreeRun), alignof(FreeRun)));
    }

    void recycleFreeRun(FreeRun* r, FreeShard& shard) SMASH_REQUIRES(shard.lock) {
        r->next = shard.free_pool;
        shard.free_pool = r;
    }

    // ── Background decommit thread ──────────────────────────────────────────
    // Two-phase release: releasePages() queues pages for decommit, background
    // thread decommits them and THEN adds to free list. This ensures pages
    // can't be reallocated before decommit completes (fixing the race condition).
    struct DecommitEntry {
        void* addr;
        size_t num_pages;
        size_t page_index;
        size_t shard_idx;
        DecommitEntry* next;
    };

    std::atomic<DecommitEntry*> decommit_head_{nullptr};
    DecommitEntry* decommit_pool_ = nullptr;
    Spinlock decommit_pool_lock_;
    pthread_t decommit_thread_{};
    std::atomic<bool> decommit_running_{false};
    Waker decommit_waker_;   // drainer parks here while the queue is empty

    DecommitEntry* newDecommitEntry() {
        LockGuard guard(decommit_pool_lock_);
        if (decommit_pool_) {
            DecommitEntry* e = decommit_pool_;
            decommit_pool_ = e->next;
            return e;
        }
        return static_cast<DecommitEntry*>(
            BootstrapAlloc::instance().allocate(sizeof(DecommitEntry), alignof(DecommitEntry)));
    }

    void recycleDecommitEntry(DecommitEntry* e) {
        LockGuard guard(decommit_pool_lock_);
        e->next = decommit_pool_;
        decommit_pool_ = e;
    }

    void queueForDecommit(void* addr, size_t num_pages, size_t page_index, size_t shard_idx) {
        DecommitEntry* e = newDecommitEntry();
        e->addr = addr;
        e->num_pages = num_pages;
        e->page_index = page_index;
        e->shard_idx = shard_idx;
        // Lock-free push to MPSC queue
        DecommitEntry* head = decommit_head_.load(std::memory_order_relaxed);
        do {
            e->next = head;
        } while (!decommit_head_.compare_exchange_weak(head, e,
            std::memory_order_release, std::memory_order_relaxed));
        // Wake the drainer only on the empty -> non-empty edge; while it is
        // still draining a burst, later pushes need no wake-up.
        if (head == nullptr) decommit_waker_.notifyAll();
    }

    void processDecommitEntry(DecommitEntry* e) {
        // Restore PROT_RW on the range. A freed range may include pages
        // smash compressed before the free (state COMPRESSED →
        // mprotect PROT_NONE). Without this, allocatePages's freelist-pop
        // path hands the still-PROT_NONE range back to the application,
        // and the next access faults. The fault handler sees state=EMPTY
        // (cleared by releaseHook in the free path) and bails — kernel
        // delivers SIGSEGV. Manifests on workloads with many compressed
        // pages followed by reuse of those addresses (e.g. test7_full
        // walrus crash with "DenseMap node count imbalance" or null-deref
        // in nlohmann::json::destroy).
        vm::commitPages(e->addr, e->num_pages * kPageSize);

        // Decommit the pages (release physical backing).
        vm::decommitPages(e->addr, e->num_pages * kPageSize);
        vm::uffdZeroFill(e->addr, e->num_pages * kPageSize);

        // NOW add to free list (safe - pages are decommitted and PROT_RW).
        // CRITICAL: newFreeRun() POPS shard.free_pool (the FreeRun recycle
        // stack), which is guarded by shard.lock — the SAME lock that guards
        // free_list and that recycleFreeRun (called from allocatePages) holds
        // when it PUSHES free_pool. This runs on the decommit thread; before
        // 2026-06-25 newFreeRun was called OUTSIDE the lock here, so the
        // decommit-thread pop raced the app-thread locked push on free_pool,
        // corrupting that singly-linked stack into a cycle → newFreeRun /
        // allocatePages free-list walks spun forever (full-mode LargeAlloc
        // spin-livelock). All free_pool/free_list mutations for a shard MUST
        // hold shard.lock. Proven by model/SmashFreePoolRace.{lean,tla}.
        // Deadlock-safe: newFreeRun's only nested lock is BootstrapAlloc's
        // expand_lock_ (taken after, never before, shard.lock) — no inversion.
        FreeShard& shard = shards_[e->shard_idx];
        {
            LockGuard guard(shard.lock);
            FreeRun* run = newFreeRun(shard);
            run->page_index = e->page_index;
            run->page_count = e->num_pages;
            run->next = shard.free_list;
            shard.free_list = run;
        }
    }

    static void* decommitThreadEntry(void* arg) {
        // Block asynchronous signals so a process-directed signal (e.g. an
        // app's shutdown SIGUSR2/SIGTERM) is never absorbed by this smash
        // background thread instead of reaching a host application thread.
        // SIGSEGV/SIGBUS/SIGABRT/SIGFPE/SIGILL left unblocked (synchronous /
        // fatal). Mirrors CompressorThread::blockAsyncSignalsOnSelf().
        {
            sigset_t set;
            sigfillset(&set);
            sigdelset(&set, SIGSEGV);
            sigdelset(&set, SIGBUS);
            sigdelset(&set, SIGABRT);
            sigdelset(&set, SIGFPE);
            sigdelset(&set, SIGILL);
            pthread_sigmask(SIG_BLOCK, &set, nullptr);
        }
        auto* self = static_cast<VmRegion*>(arg);
        while (self->decommit_running_.load(std::memory_order_relaxed)) {
            // Drain the queue
            DecommitEntry* batch = self->decommit_head_.exchange(nullptr, std::memory_order_acquire);
            if (batch) {
                while (batch) {
                    DecommitEntry* next = batch->next;
                    self->processDecommitEntry(batch);
                    self->recycleDecommitEntry(batch);
                    batch = next;
                }
            } else {
                // Park until queueForDecommit() pushes or stop clears
                // decommit_running_ (was: usleep(100) polling; util/waker.h).
                self->decommit_waker_.waitUntil([&] {
                    return self->decommit_head_.load(std::memory_order_acquire) != nullptr ||
                           !self->decommit_running_.load(std::memory_order_relaxed);
                }, 100);
            }
        }
        return nullptr;
    }

    void startDecommitThread() {
        if (tracking_mode_) return;
        bool expected = false;
        if (!decommit_running_.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return;
        }
        if (pthread_create(&decommit_thread_, nullptr, decommitThreadEntry, this) != 0) {
            // EAGAIN under thread exhaustion. Without the rollback,
            // decommit_running_ claims a drainer exists while nothing ever
            // pops the MPSC queue: freed runs never reach the free list
            // (allocation starvation) and are never madvise'd (RSS never
            // drops). Roll back so the next release retries creation, and
            // drain whatever is queued right now inline (the exchange-based
            // drain is safe from any thread).
            decommit_running_.store(false, std::memory_order_release);
            DecommitEntry* batch =
                decommit_head_.exchange(nullptr, std::memory_order_acquire);
            while (batch) {
                DecommitEntry* next = batch->next;
                processDecommitEntry(batch);
                recycleDecommitEntry(batch);
                batch = next;
            }
        }
    }

    void ensureDecommitThreadStarted() {
        if (tracking_mode_) return;
        if (decommit_running_.load(std::memory_order_relaxed)) return;
        startDecommitThread();
    }

    void stopDecommitThread() {
        if (!decommit_running_.load(std::memory_order_relaxed)) return;
        decommit_running_.store(false, std::memory_order_release);
        decommit_waker_.notifyAll();
        pthread_join(decommit_thread_, nullptr);
        // Drain any remaining entries
        DecommitEntry* batch = decommit_head_.exchange(nullptr, std::memory_order_acquire);
        while (batch) {
            DecommitEntry* next = batch->next;
            processDecommitEntry(batch);
            recycleDecommitEntry(batch);
            batch = next;
        }
    }

    // ── Tracking mode: hash map for arbitrary page addresses ────────────────
    //
    // Used in two situations:
    //   (a) compress-only mode — every malloc and mmap is tracked here, the
    //       contiguous arena is unused.
    //   (b) full mode — the contiguous arena holds all malloc-routed slab
    //       pages, AND this hash holds *external* pages (application-direct
    //       mmap and Mach VM allocations) so the compressor can compress
    //       them too. External pages get indices in
    //       [kVmMaxPages, kVmMaxPages + kTrackMaxPages); contiguous pages
    //       keep their existing low indices.
    // External slot budget: 4 GiB of tracked memory (1M pages at 4 KiB, 256K
    // at 16 KiB). Every per-page table is demand-zero, so unused capacity costs
    // address space, not RSS. Slots are recycled when pages are untracked
    // (munmap / mremap / mprotect), so the budget bounds LIVE tracked memory,
    // not cumulative mmap traffic (CPython arena churn used to exhaust a
    // non-recycled budget and silently end tracking for the rest of the run).
    static constexpr size_t kTrackMaxPages = (size_t{4} << 30) / kPageSize;
    static constexpr size_t kTrackHashCap  = 2 * kTrackMaxPages;  // 2x headroom
    static constexpr size_t kTrackHashMask = kTrackHashCap - 1;
    static constexpr int kTrackHashBits = __builtin_ctzll(kTrackHashCap);
    static_assert((kTrackHashCap & kTrackHashMask) == 0, "hash cap must be a power of 2");
    static constexpr uintptr_t kTombstone = ~uintptr_t{0};
    // Hash 64-page groups; a group's pages take adjacent slots. Hashing each
    // page on its own scattered a mapping's pages over the whole 32 MiB
    // table, one demand-zero 4 KiB table page per tracked page: RocksDB
    // (~4.7K external pages over its run) carried ~21 MB more smash metadata
    // with SMASH_TRACK_EXTERNAL=1. Grouped, a mapping fills the table densely
    // (a group is 1 KiB of table), and a lookup of a neighbouring page hits
    // the same cache lines. Larger groups would make colliding groups probe
    // further.
    static constexpr int kTrackGroupBits = 6;
    static size_t trackHashSlot(uintptr_t key) {
        const size_t group = static_cast<size_t>((key >> kTrackGroupBits) * 0x9E3779B97F4A7C15ULL) >>
                             (64 - (kTrackHashBits - kTrackGroupBits));
        return (group << kTrackGroupBits) | (key & ((uintptr_t{1} << kTrackGroupBits) - 1));
    }

    struct TrackEntry {
        std::atomic<uintptr_t> key{0};  // page addr >> kPageShift; 0 = empty/freed
        std::atomic<size_t> idx{0};
    };

    TrackEntry* track_hash_ = nullptr;
    uintptr_t* track_reverse_ = nullptr;  // idx → page_addr (page-aligned)

    // Full-mode external-page bookkeeping. The contiguous arena uses
    // next_page_ for its bump pointer (indices 0..kVmMaxPages-1); this
    // counter assigns external indices starting at kVmMaxPages.
    // external_slot_next_: monotonically incremented to reserve a slot
    // external_count_: only incremented after the slot is fully populated
    //                  (address written). committedPages() reads this, so
    //                  the compressor won't iterate to slots-in-progress.
    std::atomic<size_t> external_slot_next_{0};
    std::atomic<size_t> external_count_{0};
    // Live external pages (tracked minus untracked) — stats only.
    std::atomic<size_t> external_live_{0};
    // Serializes external-hash MUTATIONS (track / untrack / slot recycling) so
    // tombstone reuse and tombstone-to-empty cleanup stay correct; lookups
    // (fault handler, compressor) remain lock-free. Taken only on the mmap /
    // munmap / madvise / mprotect interposer paths, never in a signal handler.
    Spinlock track_lock_;
    // Recycled external slot indices (local, i.e. idx - contig_pages_).
    uint32_t* ext_free_slots_ = nullptr;
    size_t ext_free_top_ = 0;

    // Flat direct page-index → Span* table. Sized to total_pages_ entries
    // (8 B each) and mmap'd with MAP_NORESERVE so we only pay RSS for the
    // committed slice (~6K pages → ~48 KB resident for a 100 MB heap).
    // Provides a single-load lookup that replaces the two-level radix
    // walk on the free() hot path. Sits outside BootstrapAlloc-managed
    // memory because the radix tree allocates lazily L2 pages from
    // BootstrapAlloc and we want the flat table to escape that.
    std::atomic<Span*>* page_to_span_ = nullptr;
    size_t page_to_span_bytes_ = 0;  // total mapped bytes (for cleanup / sizing)
    // Parallel per-page tag, kept in step with page_to_span_: for a page of a
    // small-object span, kPageTagSmall | arena << 6 | size class; 0 for no
    // span or a large span. Lets free() route a pointer to its thread-cache
    // lane with one 2-byte load instead of a table load plus a dependent load
    // of the Span (the second cache miss in a random-access free).
    std::atomic<uint16_t>* page_tag_ = nullptr;

    size_t lookupIdx(uintptr_t addr) const {
        if (!track_hash_) return 0;
        uintptr_t key = addr >> kPageShift;
        if (key == 0) return 0;
        size_t slot = trackHashSlot(key);
        for (size_t i = 0; i < kTrackHashCap; ++i) {
            size_t s = (slot + i) & kTrackHashMask;
            // acquire: pairs with trackExternalPage's key release-store, which
            // follows the idx store (a reused tombstone slot changes idx).
            uintptr_t existing = track_hash_[s].key.load(std::memory_order_acquire);
            if (existing == key)
                return track_hash_[s].idx.load(std::memory_order_acquire);
            if (existing == 0)
                return 0;
        }
        return 0;
    }

public:
    // fork(): free-shard locks nest inside slab locks and around BootstrapAlloc
    // (see the shard.lock / expand_lock_ ordering note below); commit_lock_ and
    // decommit_pool_lock_ are leaves.
    void lockForFork() SMASH_NO_TS_ANALYSIS {
        for (size_t i = 0; i < kNumFreeShards; ++i) shards_[i].lock.lock();
        commit_lock_.lock();
        decommit_pool_lock_.lock();
        track_lock_.lock();
    }
    void unlockForFork() SMASH_NO_TS_ANALYSIS {
        track_lock_.unlock();
        decommit_pool_lock_.unlock();
        commit_lock_.unlock();
        for (size_t i = kNumFreeShards; i-- > 0;) shards_[i].lock.unlock();
    }

public:
    bool init(size_t region_size) {
        tracking_mode_ = isCompressOnlyMode();

        // Both modes use the tracking hash:
        //   - compress-only mode: it IS the page directory.
        //   - full mode: it tracks application-direct mmap / Mach VM
        //     allocations alongside the contiguous arena.
        // allocateZeroed uses direct mmap for large allocs → demand-paged,
        // zero RSS until first access. TrackEntry's default state is all-zero
        // (key=0 means empty), so no explicit initialization needed.
        track_hash_ = static_cast<TrackEntry*>(
            BootstrapAlloc::instance().allocateZeroed(
                kTrackHashCap * sizeof(TrackEntry), 64));
        track_reverse_ = static_cast<uintptr_t*>(
            BootstrapAlloc::instance().allocateZeroed(
                kTrackMaxPages * sizeof(uintptr_t), 8));
        if (!track_hash_ || !track_reverse_) return false;
        if (!tracking_mode_) {
            ext_free_slots_ = static_cast<uint32_t*>(
                BootstrapAlloc::instance().allocateZeroed(
                    kTrackMaxPages * sizeof(uint32_t), 8));
            if (!ext_free_slots_) return false;
        }

        if (!tracking_mode_) {
            // Full mode: reserve contiguous VM region. total_pages_ covers
            // both the contiguous range AND a tail reserved for external
            // pages, so PageStateTable / PageLockTable have room for both.
            contig_pages_ = region_size / kPageSize;
            total_pages_ = contig_pages_ + kTrackMaxPages;
            base_ = static_cast<char*>(vm::reservePages(region_size));
            if (!base_) return false;
            vm::setVmBounds(base_, region_size);
        } else {
            // Compress-only mode: contiguous arena is unused; tracked pages
            // get indices 1..kTrackMaxPages-1 (index 0 reserved as sentinel).
            (void)region_size;
            total_pages_ = kTrackMaxPages;
            next_page_.store(1, std::memory_order_relaxed);
        }

        // Lazy flat page-index → Span* table. MAP_NORESERVE means the kernel
        // backs only the pages we touch — virtual cost = total_pages_*8B
        // (~32 MiB at kVmMaxPages=1M), resident cost ≈ committed-pages*8B
        // rounded to OS-page granularity.
        // vm::rawMmap, not ::mmap: this runs inside the SmashHeap constructor,
        // and on Linux ::mmap is smash's own interposer, which lazily resolves
        // the real mmap with dlsym(RTLD_NEXT). glibc < 2.34's dlsym allocates
        // its dlerror state with calloc, which re-enters the half-constructed
        // heap and spins forever in HeapRedirect::getHeap (state 1) — every
        // preloaded process hung at startup on Ubuntu 20.04 / Debian 11
        // (glibc 2.31).
        page_to_span_bytes_ = total_pages_ * sizeof(std::atomic<Span*>);
        void* m = vm::rawMmap(nullptr, page_to_span_bytes_,
                              PROT_READ | PROT_WRITE,
                              MAP_ANON | MAP_PRIVATE | MAP_NORESERVE);
        if (m == MAP_FAILED) {
            page_to_span_ = nullptr;
            page_to_span_bytes_ = 0;
            // Non-fatal: callers fall back to the radix tree via PageMap.
        } else {
            page_to_span_ = static_cast<std::atomic<Span*>*>(m);
            void* t = vm::rawMmap(nullptr, total_pages_ * sizeof(std::atomic<uint16_t>),
                                  PROT_READ | PROT_WRITE,
                                  MAP_ANON | MAP_PRIVATE | MAP_NORESERVE);
            page_tag_ = (t == MAP_FAILED) ? nullptr
                                          : static_cast<std::atomic<uint16_t>*>(t);
        }

        // Decommit thread is started lazily on first releasePages() call
        // to avoid reentrancy issues during early library initialization.

        return true;
    }

    ~VmRegion() {
        stopDecommitThread();
    }

    bool isTrackingMode() const { return tracking_mode_; }

    // Reset decommit thread state after fork(). The thread doesn't exist in the
    // child (Linux fork only clones the calling thread), so we must reset state
    // so ensureDecommitThreadStarted() will restart it on first releasePages().
    //
    // Pending entries are left on the queue: the restarted thread drains them.
    // Do NOT process them here. This runs from smash's pthread_atfork child
    // handler, which (registered at constructor priority 202) runs BEFORE
    // alloc8's child handler releases the locks xxmalloc_lock took in prepare
    // (child handlers run in registration order). processDecommitEntry takes
    // a free-shard lock this same thread still holds, so the old synchronous
    // drain self-deadlocked the child whenever a decommit was pending at
    // fork() (issue #61; caught by test_fork_stress in full mode on cloudnew:
    // 1-2 hung children per 150-fork run in 3 of 4 runs; backtrace:
    // smash_atfork_child -> resetDecommitThreadForFork -> processDecommitEntry
    // -> Spinlock::lockSlow).
    void resetDecommitThreadForFork() {
        // Thread doesn't exist in child - don't try to join it
        decommit_running_.store(false, std::memory_order_relaxed);
        decommit_waker_.reinitAfterFork();
    }

    // ── Allocation (full mode only) ─────────────────────────────────────────
    void* allocatePages(size_t num_pages) {
        if (tracking_mode_) return nullptr;

        // Try this thread's shard first (low contention path)
        size_t my_shard = getShardIdx();
        FreeShard& shard = shards_[my_shard];
        {
            LockGuard guard(shard.lock);
            FreeRun** prev = &shard.free_list;
            FreeRun* run = shard.free_list;
            while (run) {
                if (run->page_count >= num_pages) {
                    size_t page_idx = run->page_index;
                    if (run->page_count == num_pages) {
                        *prev = run->next;
                        recycleFreeRun(run, shard);
                    } else {
                        run->page_index += num_pages;
                        run->page_count -= num_pages;
                    }
                    return base_ + page_idx * kPageSize;
                }
                prev = &run->next;
                run = run->next;
            }
        }

        // Try other shards round-robin
        for (size_t i = 1; i < kNumFreeShards; ++i) {
            FreeShard& other = shards_[(my_shard + i) & kFreeShardMask];
            if (!other.lock.tryLock()) continue;  // skip contended shards
            FreeRun** prev = &other.free_list;
            FreeRun* run = other.free_list;
            while (run) {
                if (run->page_count >= num_pages) {
                    size_t page_idx = run->page_index;
                    if (run->page_count == num_pages) {
                        *prev = run->next;
                        recycleFreeRun(run, other);
                    } else {
                        run->page_index += num_pages;
                        run->page_count -= num_pages;
                    }
                    other.lock.unlock();
                    return base_ + page_idx * kPageSize;
                }
                prev = &run->next;
                run = run->next;
            }
            other.lock.unlock();
        }

        // Bump allocate from fresh pages (lock-free)
        size_t start = next_page_.fetch_add(num_pages, std::memory_order_release);
        if (start + num_pages > contig_pages_) {
            next_page_.fetch_sub(num_pages, std::memory_order_relaxed);
            return nullptr;
        }
        void* addr = base_ + start * kPageSize;
        ensureCommitted(start + num_pages);
        return addr;
    }

    // Ensure pages [0, end_page) are committed (PROT_RW). Fast path: a single
    // relaxed load when the watermark already covers end_page (the common case
    // — one chunk commit serves thousands of bumps). Slow path: take
    // commit_lock_, re-check, and mprotect forward to the next kCommitChunkPages
    // boundary (clamped to contig_pages_). The double-check under the lock means
    // concurrent bumps that all cross the watermark issue exactly one mprotect
    // for the chunk, not one each.
    // Runtime-overridable commit chunk (pages). SMASH_COMMIT_CHUNK_KIB lets the
    // chunk size be swept without rebuilding; 0 disables chunking (commit
    // exactly the requested pages, like the pre-watermark behavior).
    static size_t commitChunkPages() {
        static const size_t pages = [] {
            const char* v = std::getenv("SMASH_COMMIT_CHUNK_KIB");
            if (!v) return kCommitChunkPages;
            long kib = std::atol(v);
            if (kib < 0) return kCommitChunkPages;
            return static_cast<size_t>((static_cast<size_t>(kib) * 1024) / kPageSize);
        }();
        return pages;
    }

    void ensureCommitted(size_t end_page) {
        if (end_page <= committed_pages_.load(std::memory_order_acquire)) [[likely]]
            return;
        LockGuard guard(commit_lock_);
        size_t already = committed_pages_.load(std::memory_order_relaxed);
        if (end_page <= already) return;  // another thread committed it
        const size_t chunk = commitChunkPages();
        // Commit forward to the next chunk boundary so a run of bumps shares
        // this mprotect; round end_page up, clamp to the arena end. chunk==0
        // means "no chunking" — commit exactly what's needed.
        size_t target = (chunk == 0) ? end_page : already + chunk;
        if (target < end_page) {
            target = ((end_page + chunk - 1) / chunk) * chunk;
        }
        if (target > contig_pages_) target = contig_pages_;
        void* addr = base_ + already * kPageSize;
        size_t commit_bytes = (target - already) * kPageSize;
        vm::commitPages(addr, commit_bytes);
        vm::uffdZeroFill(addr, commit_bytes);
        committed_pages_.store(target, std::memory_order_release);
    }

    // SMASH_LANDMINES / SMASH_ZERO_FREE (debug): releasePages must see every
    // freed range, so callers that would otherwise keep a freed range
    // (LargeAlloc's block cache) check this first.
    static bool releaseDebugModes() {
        static const bool on = [] {
            const char* l = std::getenv("SMASH_LANDMINES");
            const char* z = std::getenv("SMASH_ZERO_FREE");
            return (l && l[0] == '1') || (z && z[0] == '1');
        }();
        return on;
    }

    void releasePages(void* addr, size_t num_pages) {
        if (tracking_mode_) return;

        // Refuse non-arena addresses. The free list stores ARENA page indices;
        // pageIndex() on a tracked-external address returns an external index
        // (>= contig_pages_) which allocatePages would re-mint as
        // base_ + idx*kPageSize — an address past the arena end (#84). No
        // caller should pass one after the LargeAlloc routing fix; if one
        // does, unmap it (the only legitimate producer was a direct-mmap
        // fallback page) and warn once rather than corrupt the free list.
        if (!inContigArena(reinterpret_cast<uintptr_t>(addr))) {
            static std::atomic<bool> warned{false};
            bool expected = false;
            if (warned.compare_exchange_strong(expected, true,
                                               std::memory_order_relaxed)) {
                char buf[128];
                int n = smash::safe_snprintf(buf, sizeof(buf),
                    "[smash WARN] releasePages: non-arena addr=%p npages=%zu"
                    " — unmapping instead of freelisting\n", addr, num_pages);
                if (n > 0) (void)!::write(2, buf, (size_t)n);
            }
            vm::unmapPages(addr, num_pages * kPageSize);
            return;
        }

        // SMASH_LANDMINES=1: synchronously mprotect(PROT_NONE) the freed range
        // and log it. Pages are never returned to the free list, so any
        // dangling pointer in the application surfaces as SIGSEGV at the
        // exact dangling-read site (instead of corrupting later allocations
        // or zero-filling silently). Compares to glibc/jemalloc which keep
        // freed contents intact: this distinguishes "smash decommit causes
        // crash" (landmines pass) from "app UAF" (landmines crash, with
        // si_addr inside a logged range).
        static const bool landmines = []{
            const char* v = std::getenv("SMASH_LANDMINES");
            return v && v[0] == '1';
        }();
        if (landmines) {
            (void)vm::rawMprotect(addr, num_pages * kPageSize, PROT_NONE);
            char buf[96];
            int n = smash::safe_snprintf(buf, sizeof(buf),
                "[smash landmine] free addr=%p npages=%zu range=[%p,%p)\n",
                addr, num_pages, addr,
                static_cast<char*>(addr) + num_pages * kPageSize);
            if (n > 0) (void)!::write(2, buf, (size_t)n);
            return;  // never reuse — fail loudly on dangling reuse
        }

        // SMASH_ZERO_FREE=1: synchronously zero (memset) the freed range AND
        // allow normal reuse via the freelist. If walrus reads from freed
        // memory between free() and a subsequent allocation, this is exactly
        // what madvise(MADV_DONTNEED) does (zero on next access) but without
        // any kernel involvement: the zero is immediate. If this crashes,
        // walrus has a UAF that reads pre-free data. If it passes, the
        // problem is something else in the madvise/reuse path (e.g. TLB
        // shootdown timing, freelist coherency).
        static const bool zero_free = []{
            const char* v = std::getenv("SMASH_ZERO_FREE");
            return v && v[0] == '1';
        }();
        if (zero_free) {
            __builtin_memset(addr, 0, num_pages * kPageSize);
            // fall through to normal queue+decommit+reuse path
        }

        // Start decommit thread lazily on first release (avoids reentrancy
        // during early library initialization when pthread_create might malloc).
        ensureDecommitThreadStarted();

        // Two-phase release: queue pages for background decommit. The background
        // thread will decommit them and THEN add to free list. This ensures
        // pages can't be reallocated before decommit completes.
        size_t page_idx = pageIndex(reinterpret_cast<uintptr_t>(addr));
        size_t shard_idx = getShardIdx();
        queueForDecommit(addr, num_pages, page_idx, shard_idx);
    }

    // ── Page tracking (compress-only mode) ──────────────────────────────────
    // Track a page address, assigning it a compact index.
    // Returns the index (>0) on success, 0 on failure (table full or wrong mode).
    size_t trackPage(uintptr_t page_addr) {
        if (!tracking_mode_ || !track_hash_) return 0;

        uintptr_t key = page_addr >> kPageShift;
        if (key == 0) return 0;
        size_t slot = trackHashSlot(key);
        for (size_t i = 0; i < kTrackHashCap; ++i) {
            size_t s = (slot + i) & kTrackHashMask;
            uintptr_t existing = track_hash_[s].key.load(std::memory_order_relaxed);
            if (existing == key)
                return track_hash_[s].idx.load(std::memory_order_acquire);
            if (existing == 0) {
                uintptr_t expected = 0;
                if (track_hash_[s].key.compare_exchange_strong(
                        expected, key, std::memory_order_acq_rel)) {
                    size_t idx = next_page_.fetch_add(1, std::memory_order_release);
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

    // ── External-page tracking (full mode) ──────────────────────────────────
    // Register a page from an application-direct mmap or Mach VM allocation.
    // Returns the global index assigned (>= contig_pages_), or 0 on failure
    // (table full, wrong mode, or page already inside the contiguous arena).
    //
    // The returned index lives in the same PageStateTable / PageLockTable as
    // contiguous-arena pages, so the compressor's existing tick/dispatch
    // logic processes external pages without modification.
    size_t trackExternalPage(uintptr_t page_addr) {
        if (tracking_mode_ || !track_hash_) return 0;
        // Page already covered by smash's own contiguous arena? Skip.
        auto b = reinterpret_cast<uintptr_t>(base_);
        if (page_addr >= b && page_addr < b + contig_pages_ * kPageSize)
            return 0;
        uintptr_t key = page_addr >> kPageShift;
        if (key == 0 || key == kTombstone) return 0;

        LockGuard g(track_lock_);
        // Slot budget exhausted (no fresh and no recycled slot)? Bail BEFORE
        // touching the hash: a mapping larger than the budget (a multi-GiB
        // InnoDB buffer pool) must not turn registration into O(pages × cap)
        // probing — that wedged mariadbd startup for minutes.
        if (ext_free_top_ == 0 &&
            external_slot_next_.load(std::memory_order_relaxed) >= kTrackMaxPages)
            return 0;

        // Probe for the key; remember the first tombstone so a re-insert
        // reuses it (otherwise mmap/munmap churn fills the table with
        // tombstones and every miss probes the whole table).
        size_t slot = trackHashSlot(key);
        size_t reuse = SIZE_MAX;
        size_t s = 0;
        bool found_empty = false;
        for (size_t i = 0; i < kTrackHashCap; ++i) {
            s = (slot + i) & kTrackHashMask;
            uintptr_t existing = track_hash_[s].key.load(std::memory_order_relaxed);
            if (existing == key)
                return track_hash_[s].idx.load(std::memory_order_acquire);
            if (existing == kTombstone) {
                if (reuse == SIZE_MAX) reuse = s;
                continue;
            }
            if (existing == 0) { found_empty = true; break; }
        }
        if (reuse == SIZE_MAX) {
            if (!found_empty) return 0;  // table full
            reuse = s;
        }

        size_t local;
        bool fresh = false;
        if (ext_free_top_ > 0) {
            local = ext_free_slots_[--ext_free_top_];
        } else {
            local = external_slot_next_.fetch_add(1, std::memory_order_relaxed);
            fresh = true;
        }
        size_t idx = contig_pages_ + local;
        // Publish order: reverse map, then index, then key — a lock-free
        // lookup that matches the key always sees a valid index.
        track_reverse_[local] = page_addr;
        track_hash_[reuse].idx.store(idx, std::memory_order_release);
        track_hash_[reuse].key.store(key, std::memory_order_release);
        // external_count_ is the slot HIGH-WATER the compressor iterates to
        // (committedPages()); bump it only for a fresh slot, after the address
        // is written, so the compressor never walks a half-published slot.
        if (fresh) external_count_.fetch_add(1, std::memory_order_release);
        external_live_.fetch_add(1, std::memory_order_relaxed);
        return idx;
    }

    // Untrack a previously-registered external page: drop its hash entry and
    // clear its reverse-map slot. Returns the external index it had (0 if it
    // was not tracked). If recycle, the slot goes back on the free list — the
    // caller must already have reset every per-page table for idx (the
    // compressor's detachExternalRange does), because the next registration
    // hands the same idx to an unrelated page. Caller holds the page lock.
    size_t untrackExternalPage(uintptr_t page_addr, bool recycle = false) {
        if (tracking_mode_ || !track_hash_) return 0;
        uintptr_t key = page_addr >> kPageShift;
        if (key == 0 || key == kTombstone) return 0;
        LockGuard g(track_lock_);
        size_t slot = trackHashSlot(key);
        for (size_t i = 0; i < kTrackHashCap; ++i) {
            size_t s = (slot + i) & kTrackHashMask;
            uintptr_t existing = track_hash_[s].key.load(std::memory_order_relaxed);
            if (existing == 0) return 0;  // not present
            if (existing != key) continue;
            size_t idx = track_hash_[s].idx.load(std::memory_order_relaxed);
            // Tombstone, then — if the probe chain ends right after this slot
            // — turn this and any tombstones immediately before it back into
            // empty slots. Safe under track_lock_: no live key can sit past an
            // empty slot in its probe chain, and inserts are serialized.
            track_hash_[s].key.store(kTombstone, std::memory_order_release);
            size_t next = (s + 1) & kTrackHashMask;
            if (track_hash_[next].key.load(std::memory_order_relaxed) == 0) {
                size_t c = s;
                while (track_hash_[c].key.load(std::memory_order_relaxed) == kTombstone) {
                    track_hash_[c].key.store(0, std::memory_order_release);
                    c = (c - 1) & kTrackHashMask;
                    if (c == s) break;
                }
            }
            if (idx >= contig_pages_) {
                size_t local = idx - contig_pages_;
                if (local < kTrackMaxPages) {
                    track_reverse_[local] = 0;
                    if (recycle && ext_free_top_ < kTrackMaxPages)
                        ext_free_slots_[ext_free_top_++] = static_cast<uint32_t>(local);
                }
            }
            external_live_.fetch_sub(1, std::memory_order_relaxed);
            return idx;
        }
        return 0;
    }

    // Untrack an external page UNDER ITS PER-PAGE LOCK, the serialization the
    // formal model (model/SmashExternalRace.{lean,tla}) proves is required to
    // make external munmap safe against a concurrent compressor worker.
    //
    // The compressor worker compresses an external page by snapshotting it:
    //   tryLock(idx); check ACTIVE; state=COMPRESSING; memcpy(buf, pageAddress(idx), kPageSize)
    // all UNDER lock[idx]. If munmap clears state + tears down the mapping
    // WITHOUT that lock (the pre-fix bug), the worker's memcpy reads an unmapped
    // page → SIGSEGV. By taking lock[idx] here we cannot run while the worker
    // holds it mid-snapshot; and the worker re-loads pageAddress(idx) under the
    // lock and bails when we've cleared track_reverse_ to 0.
    //
    // DEADLOCK SAFETY: this holds only a per-page lock and does NOT perform any
    // mprotect / TLB-shootdown / real_munmap while holding it — the caller does
    // real_munmap AFTER this returns. So it cannot reintroduce the
    // TLB-shootdown-vs-page-lock deadlock documented in compressor_thread.h.
    // (locks/states are passed in to avoid a header dependency cycle.)
    //
    // Does not free a compressed blob or recycle the slot; the full-mode
    // interposers use CompressorThread::detachExternalRange, which does both.
    // This remains for callers without a compressor.
    void untrackExternalPageLocked(uintptr_t page_addr,
                                   PageLockTable* locks,
                                   PageStateTable* states) {
        if (tracking_mode_ || !track_hash_) return;
        size_t idx = pageIndex(page_addr);
        if (idx == 0) return;                       // not tracked
        if (locks) locks->lock(idx);
        if (states) states->set(idx, PageState::EMPTY);
        untrackExternalPage(page_addr);             // tombstone + clear reverse map
        if (locks) locks->unlock(idx);
    }

    // Return an untracked external slot to the free list. For callers that
    // untrack with recycle=false, finish their per-page bookkeeping, and only
    // then let the next registration reuse the index.
    void recycleExternalSlot(size_t idx) {
        if (idx < contig_pages_) return;
        const size_t local = idx - contig_pages_;
        LockGuard g(track_lock_);
        if (local < kTrackMaxPages && track_reverse_[local] == 0 && ext_free_top_ < kTrackMaxPages)
            ext_free_slots_[ext_free_top_++] = static_cast<uint32_t>(local);
    }
    // Hold off external track / untrack (the compressor registers a run of
    // tracked pages with its userfaultfd under this, so no page of the run is
    // untracked between the check and the registration). Leaf lock; never
    // taken in a signal handler.
    void lockTracking() SMASH_NO_TS_ANALYSIS { track_lock_.lock(); }
    void unlockTracking() SMASH_NO_TS_ANALYSIS { track_lock_.unlock(); }

    size_t externalLivePages() const { return external_live_.load(std::memory_order_relaxed); }

    // Diagnostics / tests: resident bytes of the (demand-zero) tracking hash.
    size_t trackHashResidentBytes() const {
        const size_t ps = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        const size_t bytes = kTrackHashCap * sizeof(TrackEntry);
        const size_t n = (bytes + ps - 1) / ps;
        size_t resident = 0;
        unsigned char vec[512];
        for (size_t off = 0; off < n; off += sizeof(vec)) {
            const size_t m = n - off < sizeof(vec) ? n - off : sizeof(vec);
#ifdef __APPLE__
            char* v = reinterpret_cast<char*>(vec);  // macOS: char*
#else
            unsigned char* v = vec;
#endif
            if (mincore(reinterpret_cast<char*>(track_hash_) + off * ps, m * ps, v) != 0) return 0;
            for (size_t k = 0; k < m; ++k) resident += vec[k] & 1;
        }
        return resident * ps;
    }

    bool contains(uintptr_t addr) const {
        if (tracking_mode_) {
            return lookupIdx(addr) != 0;
        }
        // Full mode: contiguous range OR external hash hit.
        auto b = reinterpret_cast<uintptr_t>(base_);
        if (addr >= b && addr < b + contig_pages_ * kPageSize) return true;
        return lookupIdx(addr) != 0;
    }

    [[gnu::always_inline]] size_t pageIndex(uintptr_t addr) const {
        if (tracking_mode_) return lookupIdx(addr);
        auto b = reinterpret_cast<uintptr_t>(base_);
        if (addr >= b && addr < b + contig_pages_ * kPageSize)
            return (addr - b) / kPageSize;
        return lookupIdx(addr);
    }

    void* pageAddress(size_t index) const {
        if (tracking_mode_) {
            if (index == 0 || index >= total_pages_) return nullptr;
            uintptr_t addr = track_reverse_[index];
            return addr ? reinterpret_cast<void*>(addr) : nullptr;
        }
        // Full mode
        if (index < contig_pages_) return base_ + index * kPageSize;
        size_t local = index - contig_pages_;
        if (local >= kTrackMaxPages) return nullptr;
        // track_reverse_[local] may be 0 if the slot was reserved but the
        // address hasn't been written yet (race with trackExternalPage).
        uintptr_t addr = track_reverse_[local];
        return addr ? reinterpret_cast<void*>(addr) : nullptr;
    }

    // ── Flat page-index → Span* table ───────────────────────────────────────
    // Single-load lookup that replaces the two-level radix walk on the
    // free()/getSize() hot path. Returns nullptr if the table isn't mapped
    // (early init, mmap failure) or if the page isn't currently owned by a
    // span — callers fall back to PageMap::get() in that case.
    [[gnu::always_inline]]
    Span* getSpan(size_t page_idx) const {
        if (!page_to_span_) [[unlikely]] return nullptr;
        if (page_idx >= total_pages_) [[unlikely]] return nullptr;
        return page_to_span_[page_idx].load(std::memory_order_acquire);
    }

    static constexpr uint16_t kPageTagSmall = 0x8000;
    // SMASH_AGE_ORDER=3 (TAP): page is cooling/compressed, so the lean free
    // must route frees on it to the slow path (no thread-cache reuse). Bit 14
    // is free: the arena field (bits 6..13) needs at most 8 bits, and the lean
    // free's uint8_t(tag >> 6) drops it. Maintained by the compressor each tick
    // (markPageCold); setSpan/setSpanRange overwrite it with a fresh tag.
    static constexpr uint16_t kPageTagCold = 0x4000;

    // CAS so a concurrent setSpan (span re-tag) always wins.
    void markPageCold(size_t page_idx, bool cold) {
        if (!page_tag_ || page_idx >= total_pages_) return;
        uint16_t t = page_tag_[page_idx].load(std::memory_order_relaxed);
        if (!(t & kPageTagSmall)) return;
        const uint16_t want = cold ? (t | kPageTagCold) : (t & ~kPageTagCold);
        if (want != t)
            page_tag_[page_idx].compare_exchange_strong(t, want, std::memory_order_release,
                                                        std::memory_order_relaxed);
    }

    // `tag` is the page tag for `span` (see page_tag_); PageMap computes it.
    [[gnu::always_inline]]
    void setSpan(size_t page_idx, Span* span, uint16_t tag) {
        if (!page_to_span_) [[unlikely]] return;
        if (page_idx >= total_pages_) [[unlikely]] return;
        // Tag first on set, last on clear: a nonzero tag always names a span
        // whose table entry is already published.
        if (span) {
            page_to_span_[page_idx].store(span, std::memory_order_release);
            if (page_tag_) page_tag_[page_idx].store(tag, std::memory_order_release);
        } else {
            if (page_tag_) page_tag_[page_idx].store(0, std::memory_order_release);
            page_to_span_[page_idx].store(nullptr, std::memory_order_release);
        }
    }

    // Set a contiguous run of flat-table slots to the same span. Bounds are
    // checked once for the whole run, then the loop is a tight sequential store
    // — avoids the per-page table-pointer null check + bound compare that
    // calling setSpan() num_pages times would repeat. Used by the span-creation
    // path (PageMap::setRange) where every page of a fresh span maps to the
    // same Span*.
    void setSpanRange(size_t first_page_idx, size_t num_pages, Span* span, uint16_t tag) {
        if (!page_to_span_) [[unlikely]] return;
        if (first_page_idx + num_pages > total_pages_) [[unlikely]] return;
        if (span) {
            for (size_t i = 0; i < num_pages; ++i)
                page_to_span_[first_page_idx + i].store(span, std::memory_order_release);
            if (page_tag_)
                for (size_t i = 0; i < num_pages; ++i)
                    page_tag_[first_page_idx + i].store(tag, std::memory_order_release);
        } else {
            if (page_tag_)
                for (size_t i = 0; i < num_pages; ++i)
                    page_tag_[first_page_idx + i].store(0, std::memory_order_release);
            for (size_t i = 0; i < num_pages; ++i)
                page_to_span_[first_page_idx + i].store(nullptr, std::memory_order_release);
        }
    }
    std::atomic<uint16_t>* pageTagTable() const { return page_tag_; }

    // True iff the flat table is mapped (mmap succeeded in init()). Used by
    // PageMap to decide whether to write the flat slot in addition to the
    // radix tree.
    bool hasSpanTable() const { return page_to_span_ != nullptr; }
    std::atomic<Span*>* spanTable() const { return page_to_span_; }

    // Cheap pointer-in-contiguous-arena check. Used by the free() / getSize()
    // hot path to decide whether the flat span table lookup is sufficient
    // (single load) or whether the caller must fall through to PageMap
    // (which also handles non-VmRegion large allocations). Excludes the
    // external-tracking tail because those pages aren't reachable through a
    // contiguous-base offset.
    [[gnu::always_inline]]
    bool inContigArena(uintptr_t addr) const {
        auto b = reinterpret_cast<uintptr_t>(base_);
        return addr >= b && addr < b + contig_pages_ * kPageSize;
    }

    // Contig-arena-only page index. Faster than pageIndex() which dispatches
    // on tracking-mode and hashes for external pages. Caller must have
    // verified inContigArena(addr).
    [[gnu::always_inline]]
    size_t contigPageIndex(uintptr_t addr) const {
        return (addr - reinterpret_cast<uintptr_t>(base_)) >> kPageShift;
    }

    char* base() const { return base_; }
    size_t totalPages() const { return total_pages_; }
    // Contiguous-arena page count. In full mode, this is the bump-arena
    // capacity (total_pages_ minus the external-page tail). In tracking
    // mode the contiguous arena is unused, so the value is 0.
    size_t contigPages() const { return contig_pages_; }

    // Total external-page slot budget (compile-time constant). Exposed so the
    // mmap interposer can cheaply reject a single mapping that could never fit
    // — see registerLinuxExternalRange().
    static constexpr size_t externalSlotCapacity() { return kTrackMaxPages; }
    // Remaining unclaimed external slots (approximate; races with concurrent
    // tracking but only used as a fast pre-filter, never for correctness).
    size_t externalSlotsRemaining() const {
        size_t used = external_slot_next_.load(std::memory_order_relaxed);
        return (used >= kTrackMaxPages ? 0 : kTrackMaxPages - used) + ext_free_top_;
    }
    size_t rawNextPage() const { return next_page_.load(std::memory_order_acquire); }

    size_t committedPages() const {
        size_t bump = next_page_.load(std::memory_order_acquire);
        if (tracking_mode_) return bump;
        size_t ext = external_count_.load(std::memory_order_relaxed);
        if (ext == 0) return bump;
        // External pages occupy [contig_pages_, contig_pages_ + ext); the
        // compressor must iterate up to the high end. Pages between bump
        // and contig_pages_ are EMPTY → cheap to skip via the chunk bitmap.
        return contig_pages_ + ext;
    }
};

} // namespace smash
