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
#include <unistd.h>
#include "smash/config.h"
#include "platform_mem.h"
#include "page_state.h"
#include "../core/bootstrap_alloc.h"
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

    struct FreeRun {
        size_t page_index;
        size_t page_count;
        FreeRun* next;
    };

    // Sharded free lists to reduce contention under high thread counts.
    // 64 shards = one per typical cache line, threads hash by ID.
    static constexpr size_t kNumFreeShards = 64;
    static constexpr size_t kFreeShardMask = kNumFreeShards - 1;

    struct alignas(64) FreeShard {  // cache-line aligned
        FreeRun* free_list = nullptr;
        FreeRun* free_pool = nullptr;
        Spinlock lock;
    };
    FreeShard shards_[kNumFreeShards];

    static size_t getShardIdx() {
        // Hash thread ID to shard. Cache per-thread for speed.
        thread_local size_t idx = std::hash<std::thread::id>{}(
            std::this_thread::get_id()) & kFreeShardMask;
        return idx;
    }

    FreeRun* newFreeRun(FreeShard& shard) {
        if (shard.free_pool) {
            FreeRun* r = shard.free_pool;
            shard.free_pool = r->next;
            return r;
        }
        return static_cast<FreeRun*>(
            BootstrapAlloc::instance().allocate(sizeof(FreeRun), alignof(FreeRun)));
    }

    void recycleFreeRun(FreeRun* r, FreeShard& shard) {
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

        // NOW add to free list (safe - pages are decommitted and PROT_RW)
        FreeShard& shard = shards_[e->shard_idx];
        FreeRun* run = newFreeRun(shard);
        run->page_index = e->page_index;
        run->page_count = e->num_pages;
        {
            LockGuard guard(shard.lock);
            run->next = shard.free_list;
            shard.free_list = run;
        }
    }

    static void* decommitThreadEntry(void* arg) {
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
                usleep(100);  // 100µs - short sleep for responsiveness
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
        pthread_create(&decommit_thread_, nullptr, decommitThreadEntry, this);
    }

    void ensureDecommitThreadStarted() {
        if (tracking_mode_) return;
        if (decommit_running_.load(std::memory_order_relaxed)) return;
        startDecommitThread();
    }

    void stopDecommitThread() {
        if (!decommit_running_.load(std::memory_order_relaxed)) return;
        decommit_running_.store(false, std::memory_order_release);
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
    static constexpr size_t kTrackMaxPages = 128 * 1024;  // 128K pages (~2GB on 16K pages)
    static constexpr size_t kTrackHashCap  = 256 * 1024;  // 2x headroom
    static constexpr size_t kTrackHashMask = kTrackHashCap - 1;

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

    // Flat direct page-index → Span* table. Sized to total_pages_ entries
    // (8 B each) and mmap'd with MAP_NORESERVE so we only pay RSS for the
    // committed slice (~6K pages → ~48 KB resident for a 100 MB heap).
    // Provides a single-load lookup that replaces the two-level radix
    // walk on the free() hot path. Sits outside BootstrapAlloc-managed
    // memory because the radix tree allocates lazily L2 pages from
    // BootstrapAlloc and we want the flat table to escape that.
    std::atomic<Span*>* page_to_span_ = nullptr;
    size_t page_to_span_bytes_ = 0;  // total mapped bytes (for cleanup / sizing)

    size_t lookupIdx(uintptr_t addr) const {
        if (!track_hash_) return 0;
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

        // Both modes use the tracking hash:
        //   - compress-only mode: it IS the page directory.
        //   - full mode: it tracks application-direct mmap / Mach VM
        //     allocations alongside the contiguous arena.
        track_hash_ = static_cast<TrackEntry*>(
            BootstrapAlloc::instance().allocate(
                kTrackHashCap * sizeof(TrackEntry), 64));
        track_reverse_ = static_cast<uintptr_t*>(
            BootstrapAlloc::instance().allocate(
                kTrackMaxPages * sizeof(uintptr_t), 8));
        if (!track_hash_ || !track_reverse_) return false;
        for (size_t i = 0; i < kTrackHashCap; ++i)
            new (&track_hash_[i]) TrackEntry();
        __builtin_memset(track_reverse_, 0, kTrackMaxPages * sizeof(uintptr_t));

        if (!tracking_mode_) {
            // Full mode: reserve contiguous VM region. total_pages_ covers
            // both the contiguous range AND a tail reserved for external
            // pages, so PageStateTable / PageLockTable have room for both.
            contig_pages_ = region_size / kPageSize;
            total_pages_ = contig_pages_ + kTrackMaxPages;
            base_ = static_cast<char*>(vm::reservePages(region_size));
            if (!base_) return false;
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
        page_to_span_bytes_ = total_pages_ * sizeof(std::atomic<Span*>);
        void* m = ::mmap(nullptr, page_to_span_bytes_,
                         PROT_READ | PROT_WRITE,
                         MAP_ANON | MAP_PRIVATE | MAP_NORESERVE,
                         -1, 0);
        if (m == MAP_FAILED) {
            page_to_span_ = nullptr;
            page_to_span_bytes_ = 0;
            // Non-fatal: callers fall back to the radix tree via PageMap.
        } else {
            page_to_span_ = static_cast<std::atomic<Span*>*>(m);
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
    void resetDecommitThreadForFork() {
        // Thread doesn't exist in child - don't try to join it
        decommit_running_.store(false, std::memory_order_relaxed);
        // Drain pending entries synchronously
        DecommitEntry* batch = decommit_head_.exchange(nullptr, std::memory_order_acquire);
        while (batch) {
            DecommitEntry* next = batch->next;
            processDecommitEntry(batch);
            recycleDecommitEntry(batch);
            batch = next;
        }
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
        size_t start = next_page_.fetch_add(num_pages, std::memory_order_relaxed);
        if (start + num_pages > contig_pages_) {
            next_page_.fetch_sub(num_pages, std::memory_order_relaxed);
            return nullptr;
        }
        void* addr = base_ + start * kPageSize;
        vm::commitPages(addr, num_pages * kPageSize);
        return addr;
    }

    void releasePages(void* addr, size_t num_pages) {
        if (tracking_mode_) return;

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
            (void)::mprotect(addr, num_pages * kPageSize, PROT_NONE);
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
                    // Reserve a slot first (use relaxed since we control visibility)
                    size_t local = external_slot_next_.fetch_add(1, std::memory_order_relaxed);
                    if (local >= kTrackMaxPages) {
                        // No more slots available
                        return 0;
                    }
                    size_t idx = contig_pages_ + local;
                    // Write address before publishing.
                    track_reverse_[local] = page_addr;
                    track_hash_[s].idx.store(idx, std::memory_order_release);
                    // Only increment external_count_ AFTER everything is set.
                    // This is what committedPages() reads, so the compressor
                    // won't iterate to this index until the address is valid.
                    // Use release so the address write is visible before the
                    // count increment.
                    external_count_.fetch_add(1, std::memory_order_release);
                    return idx;
                }
                if (track_hash_[s].key.load(std::memory_order_relaxed) == key)
                    return track_hash_[s].idx.load(std::memory_order_acquire);
            }
        }
        return 0;
    }

    // Untrack a previously-registered external page (e.g. on munmap /
    // vm_deallocate). Marks the hash slot as freed (key = ~0ULL — a
    // tombstone — so the open-addressing probe skips past it but treats it
    // as occupied for collision purposes). The PageStateTable entry must be
    // cleared by the caller (set to EMPTY) so the compressor stops scanning
    // that index.
    void untrackExternalPage(uintptr_t page_addr) {
        if (tracking_mode_ || !track_hash_) return;
        uintptr_t key = page_addr >> kPageShift;
        if (key == 0) return;
        size_t slot = static_cast<size_t>(key * 0x9E3779B97F4A7C15ULL) >> (64 - 18);
        for (size_t i = 0; i < kTrackHashCap; ++i) {
            size_t s = (slot + i) & kTrackHashMask;
            uintptr_t existing = track_hash_[s].key.load(std::memory_order_relaxed);
            if (existing == 0) return;  // not present
            if (existing == key) {
                // Mark with a tombstone so probe doesn't terminate early.
                track_hash_[s].key.store(~uintptr_t{0}, std::memory_order_release);
                return;
            }
        }
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

    size_t pageIndex(uintptr_t addr) const {
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

    [[gnu::always_inline]]
    void setSpan(size_t page_idx, Span* span) {
        if (!page_to_span_) [[unlikely]] return;
        if (page_idx >= total_pages_) [[unlikely]] return;
        page_to_span_[page_idx].store(span, std::memory_order_release);
    }

    // True iff the flat table is mapped (mmap succeeded in init()). Used by
    // PageMap to decide whether to write the flat slot in addition to the
    // radix tree.
    bool hasSpanTable() const { return page_to_span_ != nullptr; }

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
    size_t committedPages() const {
        size_t bump = next_page_.load(std::memory_order_relaxed);
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
