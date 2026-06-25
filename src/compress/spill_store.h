// smash/src/compress/spill_store.h - File-backed storage for VERY-COLD blobs
//
// A drop-in sibling of CompressStore (see compress_store.h) whose blob bytes
// live in a MAP_SHARED mapping of an on-disk spill file instead of anonymous
// RAM. The point: once a very-cold compressed blob is written here and left
// untouched, the kernel writes the dirty file pages back and they become
// CLEAN page cache, which the kernel can drop under memory pressure with no
// writeback and no swap. This reduces smash's steady-state resident footprint
// for long-lived cold data. Decompression reads the blob pointer directly out
// of the mapping (a page-cache miss is serviced by an ordinary kernel fault on
// a valid mapping — NOT a re-entry into smash's SIGSEGV handler), so the
// fault/decompress call sites are byte-identical to the anonymous path.
//
// Differences from CompressStore:
//   - Backing is sub-ranges ("region slots") of ONE MAP_SHARED file mapping,
//     handed to init(); CompressStore mmaps a fresh anonymous region per chunk.
//   - Region METADATA (bump offset, live_bytes, free list head, chain) lives
//     OUT OF BAND in an anonymous array — the file pages hold pure blob data,
//     so mutating metadata never dirties (and thus never un-cleans) file pages.
//   - regionOf() maps a pointer to its metadata via index arithmetic rather
//     than address masking.
//   - A fully-drained region punches a hole in the file (FALLOC_FL_PUNCH_HOLE),
//     releasing disk blocks and page cache, the file analog of decommit.
//
// The in-band free list (FreeNode written into a freed slot) is retained from
// CompressStore: releasing a blob writes 8 bytes into the file page. This only
// happens when a very-cold page faults back (rare by definition), and the page
// is already resident from the decompress read at that moment, so it adds no
// extra fault.
#pragma once

#include "smash/config.h"
#include "../vm/platform_mem.h"
#include "../util/spinlock.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__linux__)
#include <fcntl.h>      // fallocate, FALLOC_FL_PUNCH_HOLE
#include <linux/falloc.h>
#endif

namespace smash {

class SpillStore {
    // 16 MB region slots, matching CompressStore. A drained slot is hole-punched.
    static constexpr size_t kRegionSize = 16 * 1024 * 1024;
    // Reserve the first 16 bytes of every region so blob offset 0 is never a
    // valid slot — lets free_head == 0 mean "free list empty" (as in
    // CompressStore, where the embedded Region header occupies offset 0).
    static constexpr size_t kDataStart = 16;

    // In-band free-slot header, identical to CompressStore::FreeNode. Stored in
    // the file at the start of a freed slot; links the slot into its region's
    // free list for reuse without bumping the offset.
    struct FreeNode {
        uint32_t size;          // slot size in bytes (incl. this header)
        uint32_t next_offset;   // offset of next free slot within region, 0 = end
    };
    static_assert(sizeof(FreeNode) <= 16, "FreeNode must fit minimum-alignment slot");

public:
    // Out-of-band region metadata (anonymous — NOT in the file). One per 16 MB
    // file slot. `slot` identifies the file range [slot*kRegionSize, +kRegionSize).
    // Public so the owner (CompressorThread) can allocate the metadata array
    // sized via metadataBytesFor() before init(); its fields are opaque to
    // callers (SpillStore manages them internally).
    struct Region {
        size_t slot;                     // which 16 MB slot in the file
        std::atomic<size_t> offset;      // bump cursor within the region
        std::atomic<size_t> live_bytes;  // bytes currently in use (not freed)
        uint32_t free_head;              // offset of first free slot, 0 = none
        Region* next;                    // shard chain (older regions)
    };

private:
    // Per-shard state: independent lock and region chain, matching CompressStore.
    struct Shard {
        Region* current = nullptr;
        Spinlock lock;
    };
    Shard shards_[kCompressStoreShards];

    // File-backed blob storage.
    int fd_ = -1;                 // unlinked spill file (owned by caller; not closed here)
    char* map_base_ = nullptr;    // MAP_SHARED mapping base
    size_t map_size_ = 0;         // total mapped/reserved file size
    size_t num_slots_ = 0;        // map_size_ / kRegionSize
    Region* regions_ = nullptr;   // anonymous metadata array [num_slots_]
    std::atomic<size_t> next_slot_{0};  // global bump of 16 MB slots to shards
    bool ready_ = false;

    char* regionBase(Region* r) const {
        return map_base_ + r->slot * kRegionSize;
    }

    // Map a blob pointer back to its Region metadata via index arithmetic.
    Region* regionOf(void* ptr) const {
        size_t off = static_cast<char*>(ptr) - map_base_;
        size_t idx = off / kRegionSize;
        return &regions_[idx];
    }

    // Grab the next free 16 MB file slot for a shard. Returns nullptr when the
    // spill file is exhausted (caller falls back to anonymous store).
    Region* newRegion() {
        size_t slot = next_slot_.fetch_add(1, std::memory_order_relaxed);
        if (slot >= num_slots_) {
            next_slot_.fetch_sub(1, std::memory_order_relaxed);
            return nullptr;
        }
        Region* r = &regions_[slot];
        r->slot = slot;
        r->offset.store(kDataStart, std::memory_order_relaxed);
        r->live_bytes.store(0, std::memory_order_relaxed);
        r->free_head = 0;
        r->next = nullptr;
        return r;
    }

    // Reset a fully-empty region: punch a hole in the file so the kernel
    // releases the disk blocks AND the page cache for that range. Caller holds
    // the shard lock.
    void resetRegion(Region* r) {
#if defined(__linux__)
        if (fd_ >= 0) {
            off_t base = static_cast<off_t>(r->slot * kRegionSize);
            // Leave the reserved kDataStart bytes; punch the data range.
            (void)fallocate(fd_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                            base + static_cast<off_t>(kDataStart),
                            static_cast<off_t>(kRegionSize - kDataStart));
        }
#endif
        r->offset.store(kDataStart, std::memory_order_relaxed);
        r->free_head = 0;
        // live_bytes is already 0
    }

    // Free-list first-fit with tail split — byte-for-byte the CompressStore
    // policy, but operating on regionBase(r)-relative offsets.
    void* tryFreeList(Region* r, size_t requested, size_t* out_alloc_size) {
        if (r->free_head == 0) return nullptr;
        char* rbase = regionBase(r);
        uint32_t prev_off = 0;
        uint32_t cur_off = r->free_head;
        while (cur_off != 0) {
            auto* node = reinterpret_cast<FreeNode*>(rbase + cur_off);
            uint32_t slot_size = node->size;
            if (slot_size >= requested) {
                uint32_t next_off = node->next_offset;
                size_t remainder = slot_size - requested;
                if (remainder >= sizeof(FreeNode)) {
                    uint32_t rem_off = cur_off + static_cast<uint32_t>(requested);
                    auto* rem_node = reinterpret_cast<FreeNode*>(rbase + rem_off);
                    rem_node->size = static_cast<uint32_t>(remainder);
                    rem_node->next_offset = next_off;
                    if (prev_off == 0) {
                        r->free_head = rem_off;
                    } else {
                        auto* prev = reinterpret_cast<FreeNode*>(rbase + prev_off);
                        prev->next_offset = rem_off;
                    }
                    *out_alloc_size = static_cast<uint32_t>(requested);
                    r->live_bytes.fetch_add(requested, std::memory_order_relaxed);
                    return rbase + cur_off;
                }
                // No-split: hand the whole slot to caller.
                if (prev_off == 0) {
                    r->free_head = next_off;
                } else {
                    auto* prev = reinterpret_cast<FreeNode*>(rbase + prev_off);
                    prev->next_offset = next_off;
                }
                *out_alloc_size = slot_size;
                r->live_bytes.fetch_add(slot_size, std::memory_order_relaxed);
                return rbase + cur_off;
            }
            prev_off = cur_off;
            cur_off = node->next_offset;
        }
        return nullptr;
    }

    void* bumpAlloc(Shard& shard, size_t size, size_t* out_alloc_size) {
        Region* r = shard.current;
        while (r) {
            if (void* p = tryFreeList(r, size, out_alloc_size)) return p;
            size_t off = r->offset.load(std::memory_order_relaxed);
            size_t aligned = (off + 15) & ~15ULL;
            if (aligned + size <= kRegionSize) {
                r->offset.store(aligned + size, std::memory_order_relaxed);
                r->live_bytes.fetch_add(size, std::memory_order_relaxed);
                *out_alloc_size = size;
                return regionBase(r) + aligned;
            }
            // Current region full: scan older regions for free slots or a
            // reset (empty) region.
            Region* scan = r->next;
            while (scan) {
                if (scan->free_head != 0) {
                    if (void* p = tryFreeList(scan, size, out_alloc_size)) return p;
                }
                if (scan->live_bytes.load(std::memory_order_relaxed) == 0 &&
                    scan->offset.load(std::memory_order_relaxed) == kDataStart) {
                    size_t soff = kDataStart;
                    size_t saligned = (soff + 15) & ~15ULL;
                    if (saligned + size <= kRegionSize) {
                        scan->offset.store(saligned + size, std::memory_order_relaxed);
                        scan->live_bytes.fetch_add(size, std::memory_order_relaxed);
                        *out_alloc_size = size;
                        return regionBase(scan) + saligned;
                    }
                }
                scan = scan->next;
            }
            break;
        }
        // Need a new file slot.
        Region* nr = newRegion();
        if (!nr) return nullptr;  // spill file exhausted → caller falls back
        nr->next = shard.current;
        shard.current = nr;
        size_t off = nr->offset.load(std::memory_order_relaxed);
        size_t aligned = (off + 15) & ~15ULL;
        nr->offset.store(aligned + size, std::memory_order_relaxed);
        nr->live_bytes.store(size, std::memory_order_relaxed);
        *out_alloc_size = size;
        return regionBase(nr) + aligned;
    }

public:
    // Initialize over an already-opened spill file and its MAP_SHARED mapping.
    // The caller (CompressorThread) owns fd lifetime and directory policy
    // (real-disk selection, tmpfs rejection); SpillStore only manages slots.
    // regions_metadata is an anonymous array of at least (map_size/kRegionSize)
    // Region structs supplied by the caller (kept out of the file).
    // Returns false (and stays !ready) if the inputs can't back any region.
    bool init(int fd, void* map_base, size_t map_size,
              Region* regions_metadata, size_t metadata_count) {
        if (fd < 0 || !map_base || map_size < kRegionSize || !regions_metadata)
            return false;
        size_t slots = map_size / kRegionSize;
        if (slots == 0 || metadata_count < slots) return false;
        fd_ = fd;
        map_base_ = static_cast<char*>(map_base);
        map_size_ = slots * kRegionSize;
        num_slots_ = slots;
        regions_ = regions_metadata;
        next_slot_.store(0, std::memory_order_relaxed);
        for (int s = 0; s < kCompressStoreShards; ++s) shards_[s].current = nullptr;
        ready_ = true;
        return true;
    }

    bool ready() const { return ready_; }

    // Reset to the uninitialized state WITHOUT touching the fd/mapping (the
    // caller owns those and unmaps/closes them separately). Used post-fork to
    // forget the inherited parent spill so the child can re-init lazily. Safe
    // because SpillStore holds no resources of its own beyond the (caller-owned)
    // fd/mapping/metadata pointers.
    void reset() {
        for (int s = 0; s < kCompressStoreShards; ++s) shards_[s].current = nullptr;
        fd_ = -1;
        map_base_ = nullptr;
        map_size_ = 0;
        num_slots_ = 0;
        regions_ = nullptr;
        next_slot_.store(0, std::memory_order_relaxed);
        ready_ = false;
    }

    // Bytes of metadata the caller must allocate for `regions_metadata`,
    // given a planned mapping size. Exposed so the caller can size the
    // anonymous metadata array before init().
    static size_t metadataBytesFor(size_t map_size) {
        return (map_size / kRegionSize) * sizeof(Region);
    }
    static size_t regionSize() { return kRegionSize; }

    // True if `ptr` lies within the spill mapping (used by release-dispatch and
    // by tests to confirm a blob actually landed in the file).
    bool contains(const void* ptr) const {
        auto p = reinterpret_cast<const char*>(ptr);
        return ready_ && p >= map_base_ && p < map_base_ + map_size_;
    }

    // Store a compressed blob into the spill file. page_idx selects the shard.
    // Returns a pointer INTO the mapping (usable directly as the decompress
    // source) and the actual allocation size, which the caller MUST pass back
    // to release(). Returns nullptr if the spill file is exhausted — the caller
    // then falls back to the anonymous CompressStore.
    void* store(const void* data, size_t size, size_t* alloc_size,
                size_t page_idx = 0) {
        if (!ready_) { *alloc_size = 0; return nullptr; }
        size_t aligned_size = (size + 15) & ~15ULL;
        if (aligned_size < sizeof(FreeNode)) aligned_size = sizeof(FreeNode);

        int shard_idx = static_cast<int>(page_idx % kCompressStoreShards);
        Shard& shard = shards_[shard_idx];

        LockGuard guard(shard.lock);
        size_t actual_size = 0;
        void* ptr = bumpAlloc(shard, aligned_size, &actual_size);
        if (ptr) {
            // Writing through the MAP_SHARED mapping dirties the file page; the
            // kernel writes it back and it becomes clean/evictable thereafter.
            __builtin_memcpy(ptr, data, size);
            *alloc_size = actual_size;
        } else {
            *alloc_size = 0;
        }
        return ptr;
    }

    // Release a previously stored blob. `alloc_size` MUST be the value returned
    // by the matching store(). Async-signal-safe: pointer-only free-list work
    // under the shard spinlock (no malloc, no explicit syscalls). Reachable
    // from handleFault, like CompressStore::release.
    void release(void* ptr, size_t alloc_size, size_t page_idx = 0) {
        if (!ptr) return;
        int shard_idx = static_cast<int>(page_idx % kCompressStoreShards);
        Shard& shard = shards_[shard_idx];

        LockGuard guard(shard.lock);
        Region* r = regionOf(ptr);
        size_t prev = r->live_bytes.fetch_sub(alloc_size, std::memory_order_relaxed);
        if (prev <= alloc_size) {
            // Region now empty — punch a hole to release disk + page cache.
            resetRegion(r);
        } else {
            uint32_t off = static_cast<uint32_t>(
                static_cast<char*>(ptr) - regionBase(r));
            auto* node = reinterpret_cast<FreeNode*>(ptr);
            node->size = static_cast<uint32_t>(alloc_size);
            node->next_offset = r->free_head;
            r->free_head = off;
        }
    }
};

} // namespace smash
