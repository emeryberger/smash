// smash/src/core/thread_cache.h - Per-thread allocation cache
//
// Stores pointers in arrays (NOT inline freelists) to avoid writing
// metadata into user data pages — critical for compression.
#pragma once

#include "smash/config.h"
#include "bootstrap_alloc.h"
#include "size_classes.h"
#include "../util/spinlock.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace smash {

// Forward declarations
class Slab;
class PageMap;

class ThreadCache {
    struct ClassCache {
        void* ptrs[kThreadCacheMaxPerClass];
        uint32_t count;
    };

    ClassCache caches_[kNumClasses];

public:
    // Used by ThreadCachePool for recycling
    ThreadCache* pool_next;

    ThreadCache() {
        __builtin_memset(caches_, 0, sizeof(caches_));
        pool_next = nullptr;
    }

    // Allocate from thread cache. Returns nullptr if cache is empty for this class.
    void* allocate(uint8_t sc) {
        auto& c = caches_[sc];
        if (c.count > 0) [[likely]] {
            return c.ptrs[--c.count];
        }
        return nullptr;
    }

    // Free to thread cache. Returns false if cache is full for this class.
    bool deallocate(uint8_t sc, void* ptr) {
        auto& c = caches_[sc];
        if (c.count < kThreadCacheMaxPerClass) [[likely]] {
            c.ptrs[c.count++] = ptr;
            return true;
        }
        return false;
    }

    bool isFull(uint8_t sc) const {
        return caches_[sc].count >= kThreadCacheMaxPerClass;
    }

    bool isEmpty(uint8_t sc) const {
        return caches_[sc].count == 0;
    }

    // Refill this cache for size class `sc` by batch-allocating from the slab.
    // Returns a pointer to one allocated object (or nullptr).
    void* refill(uint8_t sc, Slab* slab);

    // Drain cache for size class `sc`, routing pointers to their arena's slab.
    void drain(uint8_t sc, Slab* all_slabs, PageMap* page_map);

    // Drain all classes, routing pointers to their arena's slab.
    void drainAll(Slab* all_slabs, PageMap* page_map);

    // Reset for reuse from pool
    void reset() {
        // All caches should already be drained
        __builtin_memset(caches_, 0, sizeof(caches_));
        pool_next = nullptr;
    }
};

// Pool of thread caches for reuse across threads
class ThreadCachePool {
    ThreadCache* freelist_ = nullptr;
    Spinlock lock_;

public:
    static ThreadCachePool& instance() {
        alignas(ThreadCachePool) static char buf[sizeof(ThreadCachePool)];
        static ThreadCachePool* inst = new (buf) ThreadCachePool;
        return *inst;
    }

    ThreadCache* acquire() {
        LockGuard guard(lock_);
        ThreadCache* tc = freelist_;
        if (tc) {
            freelist_ = tc->pool_next;
            tc->pool_next = nullptr;
        }
        return tc;
    }

    void release(ThreadCache* tc) {
        LockGuard guard(lock_);
        tc->pool_next = freelist_;
        freelist_ = tc;
    }
};

// Allocate a new ThreadCache from bootstrap memory
inline ThreadCache* newThreadCache() {
    // Try pool first
    ThreadCache* tc = ThreadCachePool::instance().acquire();
    if (tc) return tc;

    // Allocate from bootstrap
    void* mem = BootstrapAlloc::instance().allocate(sizeof(ThreadCache), alignof(ThreadCache));
    return new (mem) ThreadCache();
}

// Return ThreadCache to pool for reuse
inline void returnThreadCache(ThreadCache* tc) {
    tc->reset();
    ThreadCachePool::instance().release(tc);
}

} // namespace smash
