// smash/src/compress/compress_store.h - Storage for compressed page blobs
//
// Backed by its own mmap regions (NOT VmRegion — avoids recursive compression).
// Bucket-based allocator with free lists per power-of-2 size.
#pragma once

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
        Region* next;
    };

    static constexpr size_t kRegionSize = 16 * 1024 * 1024;  // 16MB per region

    // Bucket free lists for power-of-2 sizes: 64, 128, 256, ..., 16384
    static constexpr int kMinBucketLog = 6;   // 64 bytes min
    static constexpr int kMaxBucketLog = 14;  // 16384 bytes max
    static constexpr int kNumBuckets = kMaxBucketLog - kMinBucketLog + 1;

    struct FreeNode { FreeNode* next; };
    FreeNode* free_lists_[kNumBuckets]{};

    Region* current_ = nullptr;
    Spinlock lock_;

    int bucketIndex(size_t size) const {
        if (size <= (1U << kMinBucketLog)) return 0;
        // ceil(log2(size)) - kMinBucketLog
        int log2 = 64 - __builtin_clzll(size - 1);
        int idx = log2 - kMinBucketLog;
        if (idx >= kNumBuckets) idx = kNumBuckets - 1;
        return idx;
    }

    size_t bucketSize(int idx) const {
        return 1ULL << (idx + kMinBucketLog);
    }

    Region* newRegion() {
        void* mem = vm::mapPages(kRegionSize);
        if (!mem) return nullptr;
        // Store Region header at the front of the region itself
        auto* r = static_cast<Region*>(mem);
        r->base = static_cast<char*>(mem);
        r->capacity = kRegionSize;
        r->offset.store(sizeof(Region), std::memory_order_relaxed);  // skip header
        r->next = nullptr;
        return r;
    }

    void* bumpAlloc(size_t size) {
        // Try current region
        Region* r = current_;
        while (r) {
            size_t off = r->offset.load(std::memory_order_relaxed);
            size_t aligned = (off + 15) & ~15ULL;  // 16-byte alignment
            if (aligned + size <= r->capacity) {
                if (r->offset.compare_exchange_weak(off, aligned + size,
                        std::memory_order_relaxed))
                    return r->base + aligned;
                continue;  // retry
            }
            break;  // region full
        }
        // Need new region
        Region* nr = newRegion();
        if (!nr) return nullptr;
        nr->next = current_;
        current_ = nr;
        size_t off = nr->offset.load(std::memory_order_relaxed);
        size_t aligned = (off + 15) & ~15ULL;
        nr->offset.store(aligned + size, std::memory_order_relaxed);
        return nr->base + aligned;
    }

public:
    void init() {
        current_ = newRegion();
    }

    // Store a compressed blob. Returns pointer to stored data and the allocated size.
    // The caller must save both for later release.
    void* store(const void* data, size_t size, size_t* alloc_size) {
        int bucket = bucketIndex(size);
        size_t bsize = bucketSize(bucket);
        *alloc_size = bsize;

        void* ptr = nullptr;

        LockGuard guard(lock_);

        // Try free list first
        if (free_lists_[bucket]) {
            FreeNode* node = free_lists_[bucket];
            free_lists_[bucket] = node->next;
            ptr = node;
        } else {
            ptr = bumpAlloc(bsize);
        }

        if (ptr) {
            __builtin_memcpy(ptr, data, size);
        }
        return ptr;
    }

    // Release a previously stored blob back to the free list.
    void release(void* ptr, size_t alloc_size) {
        if (!ptr) return;
        int bucket = bucketIndex(alloc_size);

        LockGuard guard(lock_);
        auto* node = static_cast<FreeNode*>(ptr);
        node->next = free_lists_[bucket];
        free_lists_[bucket] = node;
    }
};

} // namespace smash
