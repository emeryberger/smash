// smash/src/compress/compress_store.h - Storage for compressed page blobs
//
// Backed by its own mmap regions (NOT VmRegion — avoids recursive compression).
// Bucket-based allocator with free lists per power-of-2 size.
// Sharded by page index to reduce lock contention across compressor workers.
//
// Region reclamation: each region tracks live bytes.  When live_bytes drops
// to zero the region's data pages are decommitted (MADV_FREE on macOS,
// MADV_DONTNEED on Linux), releasing physical memory back to the OS while
// keeping the virtual mapping.  If the region is reused, the OS zero-fills
// on access and the bump allocator is reset.
#pragma once

#include "smash/config.h"
#include "../vm/platform_mem.h"
#include "../util/spinlock.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace smash {

class CompressStore {
    // Regions for blob storage (own mmap, not from VmRegion or bootstrap)
    struct Region {
        char* base;
        size_t capacity;
        std::atomic<size_t> offset;
        std::atomic<size_t> live_bytes;  // bytes currently in use (not freed)
        Region* next;
    };

    static constexpr size_t kRegionSize = 16 * 1024 * 1024;  // 16MB per region
    // Data starts after the header, rounded up to 16-byte alignment.
    static constexpr size_t kDataStart = (sizeof(Region) + 15) & ~15ULL;

    // Bucket free lists for power-of-2 sizes: 64, 128, 256, ..., 16384
    static constexpr int kMinBucketLog = 6;   // 64 bytes min
    static constexpr int kMaxBucketLog = 14;  // 16384 bytes max
    static constexpr int kNumBuckets = kMaxBucketLog - kMinBucketLog + 1;

    struct FreeNode { FreeNode* next; };

    // Per-shard state: independent lock, free lists, and region chain
    struct Shard {
        FreeNode* free_lists[kNumBuckets]{};
        Region* current = nullptr;
        Spinlock lock;
    };

    Shard shards_[kCompressStoreShards];

    static int bucketIndex(size_t size) {
        if (size <= (1U << kMinBucketLog)) return 0;
        // ceil(log2(size)) - kMinBucketLog
        int log2 = 64 - __builtin_clzll(size - 1);
        int idx = log2 - kMinBucketLog;
        if (idx >= kNumBuckets) idx = kNumBuckets - 1;
        return idx;
    }

    static size_t bucketSize(int idx) {
        return 1ULL << (idx + kMinBucketLog);
    }

    // Allocate a kRegionSize-aligned region so regionOf() can derive
    // the Region* from any interior pointer via address masking.
    static Region* newRegion() {
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
        r->next = nullptr;
        return r;
    }

    // Find which region a pointer belongs to.
    // Works because regions are kRegionSize-aligned.
    static Region* regionOf(void* ptr) {
        auto addr = reinterpret_cast<uintptr_t>(ptr);
        return reinterpret_cast<Region*>(addr & ~(kRegionSize - 1));
    }

    // Reset a fully-empty region: decommit data pages and purge free lists.
    // Caller must hold the shard lock.
    static void resetRegion(Region* r, Shard& shard) {
        // Decommit data pages (skip header to keep Region struct alive).
        size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        size_t decommit_start = (kDataStart + page_size - 1) & ~(page_size - 1);
        if (decommit_start < r->capacity) {
            vm::decommitPages(r->base + decommit_start,
                              r->capacity - decommit_start);
        }
        r->offset.store(kDataStart, std::memory_order_relaxed);
        // live_bytes is already 0

        // Purge free-list entries that point into this region.
        for (int b = 0; b < kNumBuckets; ++b) {
            FreeNode** pp = &shard.free_lists[b];
            while (*pp) {
                if (regionOf(*pp) == r) {
                    *pp = (*pp)->next;  // unlink
                } else {
                    pp = &(*pp)->next;
                }
            }
        }
    }

    static void* bumpAlloc(Shard& shard, size_t size) {
        // Try current region
        Region* r = shard.current;
        while (r) {
            size_t off = r->offset.load(std::memory_order_relaxed);
            size_t aligned = (off + 15) & ~15ULL;  // 16-byte alignment
            if (aligned + size <= r->capacity) {
                if (r->offset.compare_exchange_weak(off, aligned + size,
                        std::memory_order_relaxed)) {
                    r->live_bytes.fetch_add(size, std::memory_order_relaxed);
                    return r->base + aligned;
                }
                continue;  // retry
            }
            // Region full.  Check older regions for one that was reset.
            Region* scan = r->next;
            while (scan) {
                if (scan->live_bytes.load(std::memory_order_relaxed) == 0 &&
                    scan->offset.load(std::memory_order_relaxed) == kDataStart) {
                    off = scan->offset.load(std::memory_order_relaxed);
                    aligned = (off + 15) & ~15ULL;
                    if (aligned + size <= scan->capacity) {
                        if (scan->offset.compare_exchange_weak(off, aligned + size,
                                std::memory_order_relaxed)) {
                            scan->live_bytes.fetch_add(size, std::memory_order_relaxed);
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
        return nr->base + aligned;
    }

public:
    void init() {
        for (int s = 0; s < kCompressStoreShards; ++s)
            shards_[s].current = newRegion();
    }

    // Store a compressed blob. page_idx selects the shard.
    // Returns pointer to stored data and the allocated size.
    void* store(const void* data, size_t size, size_t* alloc_size,
                size_t page_idx = 0) {
        int bucket = bucketIndex(size);
        size_t bsize = bucketSize(bucket);
        *alloc_size = bsize;

        int shard_idx = static_cast<int>(page_idx % kCompressStoreShards);
        Shard& shard = shards_[shard_idx];

        void* ptr = nullptr;

        LockGuard guard(shard.lock);

        // Try free list first
        if (shard.free_lists[bucket]) {
            FreeNode* node = shard.free_lists[bucket];
            shard.free_lists[bucket] = node->next;
            ptr = node;
            // Re-account: the slot was freed (live_bytes decremented),
            // now it's live again.
            Region* r = regionOf(ptr);
            r->live_bytes.fetch_add(bsize, std::memory_order_relaxed);
        } else {
            ptr = bumpAlloc(shard, bsize);
        }

        if (ptr) {
            __builtin_memcpy(ptr, data, size);
        }
        return ptr;
    }

    // Release a previously stored blob back to the free list.
    void release(void* ptr, size_t alloc_size, size_t page_idx = 0) {
        if (!ptr) return;
        int bucket = bucketIndex(alloc_size);

        int shard_idx = static_cast<int>(page_idx % kCompressStoreShards);
        Shard& shard = shards_[shard_idx];

        LockGuard guard(shard.lock);

        Region* r = regionOf(ptr);
        size_t prev = r->live_bytes.fetch_sub(alloc_size, std::memory_order_relaxed);

        if (prev <= alloc_size) {
            // Region is now empty — decommit its data pages.
            resetRegion(r, shard);
            // Don't add to free list — the region has been reset.
        } else {
            auto* node = static_cast<FreeNode*>(ptr);
            node->next = shard.free_lists[bucket];
            shard.free_lists[bucket] = node;
        }
    }
};

} // namespace smash
