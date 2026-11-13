// smash/src/compress/compress_store.h - Storage for compressed page blobs
//
// Backed by its own mmap regions (NOT VmRegion — avoids recursive compression).
// Exact-size bump allocator with 16-byte alignment.  No power-of-2 rounding.
// Sharded by page index to reduce lock contention across compressor workers.
//
// Region reclamation: each region tracks live bytes.  When live_bytes drops
// to zero the region's data pages are decommitted (MADV_FREE_REUSABLE on
// macOS, MADV_DONTNEED on Linux), releasing physical memory back to the OS
// while keeping the virtual mapping.  If the region is reused, the OS
// zero-fills on access and the bump allocator is reset.
//
// Partial reclamation: per-page live-blob counts let trim() (once per
// compressor tick) return individual pages whose blobs have all been freed,
// so a mostly-drained region does not keep its freed bytes resident.
#pragma once

#include "smash/config.h"
#include "../vm/platform_mem.h"
#include "../util/spinlock.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace smash {

class CompressStore {
    // Pin mode for CompressStore regions — prevents the kernel from evicting
    // compressed blobs under cgroup memory pressure.  Without pinning, a
    // cgroup MemoryHigh cap pushes CompressStore pages into swap, and
    // decompression then requires swap-in + decompress (double-fault latency).
    //
    // Modes:
    //   kOff       — no pinning (default when not under cgroup pressure)
    //   kMlock     — mlock() each region; hard pin, immune to reclaim
    //   kWillNeed  — madvise(MADV_WILLNEED) after each store(); soft hint
    enum class PinMode : uint8_t { kOff = 0, kMlock = 1, kWillNeed = 2 };

    PinMode pin_mode_ = PinMode::kOff;

    static PinMode detectPinMode() {
        const char* v = std::getenv("SMASH_MLOCK_STORE");
        if (v) {
            if (v[0] == '0') return PinMode::kOff;
            if (v[0] == '2') return PinMode::kWillNeed;
            return PinMode::kMlock;  // "1" or any truthy value
        }
        return PinMode::kOff;
    }

    // In-band free-slot header.  When a blob is released we overwrite the
    // first 16 bytes with this header and link the slot into its region's
    // free list.  Future allocations check the free list first before
    // bumping the offset, so re-tier (release-then-allocate within the same
    // shard) reuses the freed slot in place instead of growing the region.
    //
    // Without this, LZ4 → zstd-9 upgrades on memcached produced ~80 MB
    // of stranded LZ4 regions that couldn't drain (a few stranger pages
    // per region kept live_bytes > 0).  See TIERED_RECOMPRESSION.md.
    //
    // Sized to fit within the 16-byte minimum alignment of every blob.
    struct FreeNode {
        uint32_t size;          // slot size in bytes (incl. this header)
        uint32_t next_offset;   // offset of next free slot within region, 0 = end
    };
    static_assert(sizeof(FreeNode) <= 16, "FreeNode must fit minimum-alignment slot");

    static constexpr size_t kRegionSize = 16 * 1024 * 1024;  // 16MB per region
    // Per-page bookkeeping is sized for the smallest page size (4 KiB); larger
    // OS pages (16/64 KiB) use a prefix of the arrays.
    static constexpr size_t kMaxRegionPages = kRegionSize / 4096;

    // Regions for blob storage (own mmap, not from VmRegion or bootstrap)
    //
    // Partial reclamation: a region is decommitted wholesale only when ALL its
    // blobs are gone, but after a mass refault (e.g. a hot set touching most
    // compressed pages) the few surviving blobs pin every region and the freed
    // bytes stay resident on the free lists. page_live[] counts live blobs
    // touching each OS page; when one drops to zero the page is flagged in
    // dead_bits and trim() (compressor tick, never the fault path) unlinks the
    // free-list nodes stored on it and returns it to the OS.
    struct Region {
        char* base;
        size_t capacity;
        std::atomic<size_t> offset;
        std::atomic<size_t> live_bytes;  // bytes currently in use (not freed)
        uint32_t free_head;              // offset of first free slot, 0 = none
        uint32_t dead_count;             // pages flagged in dead_bits
        Region* next;
        uint16_t page_live[kMaxRegionPages];      // live blobs touching page
        uint64_t dead_bits[kMaxRegionPages / 64]; // page_live hit 0; trim pending
    };

    // Data starts after the header, rounded up to 16-byte alignment.
    static constexpr size_t kDataStart = (sizeof(Region) + 15) & ~15ULL;

    static size_t pageShift() {
        static const size_t s = [] {
            size_t ps = static_cast<size_t>(sysconf(_SC_PAGESIZE));
            size_t sh = 12;
            while ((size_t{1} << sh) < ps) ++sh;
            return sh;
        }();
        return s;
    }

    static bool isDead(const Region* r, size_t p) {
        return (r->dead_bits[p >> 6] >> (p & 63)) & 1;
    }
    static void setDead(Region* r, size_t p) {
        if (!isDead(r, p)) { r->dead_bits[p >> 6] |= 1ULL << (p & 63); ++r->dead_count; }
    }
    static void clearDead(Region* r, size_t p) {
        if (isDead(r, p)) { r->dead_bits[p >> 6] &= ~(1ULL << (p & 63)); --r->dead_count; }
    }

    // A slot [off, off+size) became live (delta=+1) or free (delta=-1).
    // Caller holds the shard lock.
    static void accountSlot(Region* r, size_t off, size_t size, int delta) {
        const size_t sh = pageShift();
        const size_t first = off >> sh, last = (off + size - 1) >> sh;
        for (size_t p = first; p <= last; ++p) {
            if (delta > 0) {
                ++r->page_live[p];
                clearDead(r, p);
            } else if (--r->page_live[p] == 0) {
                setDead(r, p);
            }
        }
    }

    // Return [addr, addr+len) of a region to the OS. Not vm::decommitPages:
    // its bounds check traps on short ranges outside the VmRegion.
    void decommitStore(char* addr, size_t len) {
        if (pin_mode_ == PinMode::kMlock) vm::unlockPages(addr, len);
#if defined(__linux__)
        (void)madvise(addr, len, MADV_DONTNEED);
#elif defined(__APPLE__)
        (void)madvise(addr, len, MADV_FREE_REUSABLE);
#else
        vm::decommitPages(addr, len);
#endif
    }

    std::atomic<size_t> trimmed_pages_{0};

    // Per-shard state: independent lock and region chain
    struct Shard {
        Region* current = nullptr;
        Spinlock lock;
    };

    Shard shards_[kCompressStoreShards];

    // Allocate a kRegionSize-aligned region so regionOf() can derive
    // the Region* from any interior pointer via address masking.
    Region* newRegion() {
        // Over-allocate to guarantee alignment: request 2x and trim.
        void* raw = vm::mapPages(kRegionSize * 2);
        if (!raw) return nullptr;
        auto raw_addr = reinterpret_cast<uintptr_t>(raw);
        auto aligned_addr = (raw_addr + kRegionSize - 1) & ~(kRegionSize - 1);

        // Unmap the prefix and suffix outside the aligned region.
        size_t prefix = aligned_addr - raw_addr;
        if (prefix > 0)
            vm::unmapPages(raw, prefix);
        size_t suffix = (raw_addr + kRegionSize * 2) - (aligned_addr + kRegionSize);
        if (suffix > 0)
            vm::unmapPages(reinterpret_cast<void*>(aligned_addr + kRegionSize), suffix);

        auto* r = reinterpret_cast<Region*>(aligned_addr);
        r->base = reinterpret_cast<char*>(aligned_addr);
        r->capacity = kRegionSize;
        r->offset.store(kDataStart, std::memory_order_relaxed);
        r->live_bytes.store(0, std::memory_order_relaxed);
        r->free_head = 0;
        r->dead_count = 0;
        r->next = nullptr;

        if (pin_mode_ == PinMode::kMlock) {
            if (!vm::lockPages(r->base, kRegionSize)) {
                // mlock failed (RLIMIT_MEMLOCK too low) — degrade to willneed
                pin_mode_ = PinMode::kWillNeed;
            }
        }
        return r;
    }

    // Find which region a pointer belongs to.
    // Works because regions are kRegionSize-aligned.
    static Region* regionOf(void* ptr) {
        auto addr = reinterpret_cast<uintptr_t>(ptr);
        return reinterpret_cast<Region*>(addr & ~(kRegionSize - 1));
    }

    // Reset a fully-empty region: unlock and decommit data pages.
    // Caller must hold the shard lock.
    void resetRegion(Region* r) {
        size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        size_t decommit_start = (kDataStart + page_size - 1) & ~(page_size - 1);
        if (decommit_start < r->capacity) {
            if (pin_mode_ == PinMode::kMlock)
                vm::unlockPages(r->base + decommit_start,
                                r->capacity - decommit_start);
            vm::decommitPages(r->base + decommit_start,
                              r->capacity - decommit_start);
        }
        r->offset.store(kDataStart, std::memory_order_relaxed);
        r->free_head = 0;
        // live_bytes and every page_live[] are already 0; the whole data
        // range was just decommitted, so no trim is pending.
        std::memset(r->dead_bits, 0, sizeof(r->dead_bits));
        r->dead_count = 0;
    }

    // Try to satisfy an allocation from a region's free list (LIFO,
    // first-fit; splits the slot when the remainder is large enough to
    // hold its own free-list header — recovers the unused tail when a
    // smaller blob (e.g. zstd-9, 0.7 KB) is allocated into a slot freed
    // by a larger blob (e.g. LZ4, 1.3 KB).  Caller (bumpAlloc) holds the
    // shard lock.  Returns nullptr if no fitting free slot exists; on
    // success writes the actual returned-slot size into *out_alloc_size.
    static void* tryFreeList(Region* r, size_t requested,
                             size_t* out_alloc_size) {
        if (r->free_head == 0) return nullptr;
        uint32_t prev_off = 0;
        uint32_t cur_off = r->free_head;
        while (cur_off != 0) {
            auto* node = reinterpret_cast<FreeNode*>(r->base + cur_off);
            uint32_t slot_size = node->size;
            if (slot_size >= requested) {
                uint32_t next_off = node->next_offset;
                size_t remainder = slot_size - requested;
                if (remainder >= sizeof(FreeNode)) {
                    // Split: requested bytes go to caller; remainder
                    // stays on the free list at offset cur_off+requested.
                    // requested is 16-byte aligned (enforced in store()),
                    // and so is slot_size (originally allocated 16-byte
                    // aligned, never broken since), so the new node's
                    // offset is also 16-byte aligned.
                    uint32_t rem_off = cur_off +
                        static_cast<uint32_t>(requested);
                    auto* rem_node = reinterpret_cast<FreeNode*>(
                        r->base + rem_off);
                    rem_node->size = static_cast<uint32_t>(remainder);
                    rem_node->next_offset = next_off;
                    if (prev_off == 0) {
                        r->free_head = rem_off;
                    } else {
                        auto* prev = reinterpret_cast<FreeNode*>(
                            r->base + prev_off);
                        prev->next_offset = rem_off;
                    }
                    *out_alloc_size = static_cast<uint32_t>(requested);
                    r->live_bytes.fetch_add(requested,
                        std::memory_order_relaxed);
                    accountSlot(r, cur_off, requested, +1);
                    // The remainder's header may sit on a page with no live
                    // blob (possibly one trim() already returned and this
                    // write just faulted back in): flag it so trim() drops
                    // the node and returns the page again.
                    if (r->page_live[rem_off >> pageShift()] == 0)
                        setDead(r, rem_off >> pageShift());
                    return r->base + cur_off;
                }
                // No-split: hand the whole slot to caller — remainder
                // too small to manage on its own.
                if (prev_off == 0) {
                    r->free_head = next_off;
                } else {
                    auto* prev = reinterpret_cast<FreeNode*>(r->base + prev_off);
                    prev->next_offset = next_off;
                }
                *out_alloc_size = slot_size;
                r->live_bytes.fetch_add(slot_size, std::memory_order_relaxed);
                accountSlot(r, cur_off, slot_size, +1);
                return r->base + cur_off;
            }
            prev_off = cur_off;
            cur_off = node->next_offset;
        }
        return nullptr;
    }

    void* bumpAlloc(Shard& shard, size_t size, size_t* out_alloc_size)
            SMASH_REQUIRES(shard.lock) {
        // Try current region's free list first.  In re-tier scenarios
        // (release X's old blob, then immediately allocate X's new blob)
        // this returns the just-freed slot, preventing the bump pointer
        // from growing past kRegionSize.
        Region* r = shard.current;
        while (r) {
            if (void* p = tryFreeList(r, size, out_alloc_size)) return p;
            size_t off = r->offset.load(std::memory_order_relaxed);
            size_t aligned = (off + 15) & ~15ULL;  // 16-byte alignment
            if (aligned + size <= r->capacity) {
                if (r->offset.compare_exchange_weak(off, aligned + size,
                        std::memory_order_relaxed)) {
                    r->live_bytes.fetch_add(size, std::memory_order_relaxed);
                    accountSlot(r, aligned, size, +1);
                    *out_alloc_size = size;
                    return r->base + aligned;
                }
                continue;  // retry
            }
            // Region full.  Check older regions for one that was reset OR
            // has free-list slots we can reuse.
            Region* scan = r->next;
            while (scan) {
                if (scan->free_head != 0) {
                    if (void* p = tryFreeList(scan, size, out_alloc_size))
                        return p;
                }
                if (scan->live_bytes.load(std::memory_order_relaxed) == 0 &&
                    scan->offset.load(std::memory_order_relaxed) == kDataStart) {
                    off = scan->offset.load(std::memory_order_relaxed);
                    aligned = (off + 15) & ~15ULL;
                    if (aligned + size <= scan->capacity) {
                        if (scan->offset.compare_exchange_weak(off, aligned + size,
                                std::memory_order_relaxed)) {
                            scan->live_bytes.fetch_add(size, std::memory_order_relaxed);
                            accountSlot(scan, aligned, size, +1);
                            *out_alloc_size = size;
                            return scan->base + aligned;
                        }
                    }
                }
                scan = scan->next;
            }
            break;
        }
        // Need new region
        Region* nr = newRegion();
        if (!nr) return nullptr;
        nr->next = shard.current;
        shard.current = nr;
        size_t off = nr->offset.load(std::memory_order_relaxed);
        size_t aligned = (off + 15) & ~15ULL;
        nr->offset.store(aligned + size, std::memory_order_relaxed);
        nr->live_bytes.store(size, std::memory_order_relaxed);
        accountSlot(nr, aligned, size, +1);
        *out_alloc_size = size;
        return nr->base + aligned;
    }

public:
    void init() {
        pin_mode_ = detectPinMode();
        for (int s = 0; s < kCompressStoreShards; ++s)
            shards_[s].current = newRegion();
    }

    // Push a fresh region to the head of each shard's chain.
    // Call before a bulk upgrade to ensure new blobs go into fresh regions
    // while old blob releases drain the old regions (enabling decommit).
    void pushFreshRegions() {
        for (int s = 0; s < kCompressStoreShards; ++s) {
            LockGuard guard(shards_[s].lock);
            Region* nr = newRegion();
            if (nr) {
                nr->next = shards_[s].current;
                shards_[s].current = nr;
            }
        }
    }

    // Store a compressed blob. page_idx selects the shard.
    // Returns pointer to stored data and the actual allocation size — the
    // latter may be larger than the requested size if the allocator
    // returned a free-list slot from a previous release.  Caller MUST pass
    // the exact `alloc_size` back to release() so live_bytes accounting
    // stays balanced.
    void* store(const void* data, size_t size, size_t* alloc_size,
                size_t page_idx = 0) {
        size_t aligned_size = (size + 15) & ~15ULL;
        // Free-list nodes need 8 bytes at the head, and bumped allocations
        // are 16-byte aligned, so any slot we hand out is at least 16
        // bytes.  Enforce here so future free-list pushes always fit.
        if (aligned_size < sizeof(FreeNode)) aligned_size = sizeof(FreeNode);

        int shard_idx = static_cast<int>(page_idx % kCompressStoreShards);
        Shard& shard = shards_[shard_idx];

        LockGuard guard(shard.lock);
        size_t actual_size = 0;
        void* ptr = bumpAlloc(shard, aligned_size, &actual_size);
        if (ptr) {
            __builtin_memcpy(ptr, data, size);
            *alloc_size = actual_size;
            if (pin_mode_ == PinMode::kWillNeed)
                vm::willNeedPages(ptr, actual_size);
        } else {
            *alloc_size = 0;
        }
        return ptr;
    }

    // Release a previously stored blob.  `alloc_size` MUST be the value
    // returned by the matching store() — the free-list reuses the slot
    // verbatim, so the size must match the slot, not the original
    // requested size.
    void release(void* ptr, size_t alloc_size, size_t page_idx = 0) {
        if (!ptr) return;

        int shard_idx = static_cast<int>(page_idx % kCompressStoreShards);
        Shard& shard = shards_[shard_idx];

        LockGuard guard(shard.lock);

        Region* r = regionOf(ptr);
        size_t prev = r->live_bytes.fetch_sub(alloc_size, std::memory_order_relaxed);
        accountSlot(r, static_cast<size_t>(static_cast<char*>(ptr) - r->base),
                    alloc_size, -1);

        if (prev <= alloc_size) {
            // Region is now empty — decommit its data pages.  This also
            // clears free_head, dropping any free-list nodes (their backing
            // memory is about to be decommitted).
            resetRegion(r);
        } else {
            // Region still has live blobs.  Push this slot onto the
            // region's free list so a future allocation can reuse it
            // without bumping the offset.
            uint32_t off = static_cast<uint32_t>(
                static_cast<char*>(ptr) - r->base);
            auto* node = reinterpret_cast<FreeNode*>(ptr);
            node->size = static_cast<uint32_t>(alloc_size);
            node->next_offset = r->free_head;
            r->free_head = off;
        }
    }

    // Return pages whose blobs have all been released (page_live == 0) to the
    // OS. Call periodically from the compressor thread — never from the fault
    // handler (it walks free lists and makes syscalls). Free-list nodes whose
    // header lies on such a page are unlinked first, so no node ever lives on
    // a decommitted page; a free slot that merely extends onto one is kept
    // (reusing it faults the page back in as zeros, and accountSlot marks it
    // live again). Returns the number of pages decommitted.
    size_t trim() {
        const size_t sh = pageShift();
        const size_t ps = size_t{1} << sh;
        const size_t npages = kRegionSize >> sh;
        const size_t first_data = (kDataStart + ps - 1) >> sh;  // header pages stay
        size_t total = 0;
        for (int s = 0; s < kCompressStoreShards; ++s) {
            LockGuard guard(shards_[s].lock);
            for (Region* r = shards_[s].current; r; r = r->next) {
                if (r->dead_count == 0) continue;
                // 1. Unlink free nodes stored on dead pages.
                uint32_t prev_off = 0, cur = r->free_head;
                while (cur != 0) {
                    auto* node = reinterpret_cast<FreeNode*>(r->base + cur);
                    uint32_t next = node->next_offset;
                    if (isDead(r, cur >> sh)) {
                        if (prev_off == 0) r->free_head = next;
                        else reinterpret_cast<FreeNode*>(r->base + prev_off)->next_offset = next;
                    } else {
                        prev_off = cur;
                    }
                    cur = next;
                }
                // 2. Decommit dead pages in contiguous runs.
                size_t p = first_data;
                while (p < npages) {
                    if (!isDead(r, p)) { ++p; continue; }
                    size_t q = p;
                    while (q < npages && isDead(r, q)) ++q;
                    decommitStore(r->base + (p << sh), (q - p) << sh);
                    total += q - p;
                    p = q;
                }
                std::memset(r->dead_bits, 0, sizeof(r->dead_bits));
                r->dead_count = 0;
            }
        }
        trimmed_pages_.fetch_add(total, std::memory_order_relaxed);
        return total;
    }

    size_t trimmedPages() const { return trimmed_pages_.load(std::memory_order_relaxed); }
};

} // namespace smash
