// smash/src/smash_heap.cpp - SmashHeap alloc8 integration + thread cache methods
#include "smash_heap.h"
#include <alloc8/alloc8.h>

// ── Thread init counter for deferred compression start ──────────────────────
// pthread_create during early DYLD_INSERT init crashes the ObjC runtime's
// task_restartable_ranges_register on macOS. We count threadInit() calls
// and only start compression after the second call (first = main thread
// during early init, subsequent = real threads after init is complete).

std::atomic<int> smash::g_thread_init_count{0};

// ── Thread cache methods that depend on Slab ─────────────────────────────────

namespace smash {

void* ThreadCache::refill(uint8_t sc, Slab* slab) {
    // Batch-allocate from slab into our cache
    auto& c = caches_[sc];
    size_t batch = kThreadCacheBatchSize;
    if (batch > kThreadCacheMaxPerClass) batch = kThreadCacheMaxPerClass;

    size_t got = slab->allocateBatch(c.ptrs, batch);
    if (got == 0) return nullptr;

    // Return one, keep the rest in cache
    c.count = static_cast<uint32_t>(got - 1);
    return c.ptrs[got - 1];
}

void ThreadCache::drain(uint8_t sc, Slab* slab) {
    auto& c = caches_[sc];
    // Drain half the cache
    size_t to_drain = c.count / 2;
    if (to_drain == 0) to_drain = c.count;

    size_t start = c.count - to_drain;
    slab->deallocateBatch(&c.ptrs[start], to_drain);
    c.count = static_cast<uint32_t>(start);
}

void ThreadCache::drainAll(Slab* slabs) {
    for (int i = 0; i < kNumClasses; ++i) {
        auto& c = caches_[i];
        if (c.count > 0) {
            slabs[i].deallocateBatch(c.ptrs, c.count);
            c.count = 0;
        }
    }
}

} // namespace smash

// ── alloc8 integration ───────────────────────────────────────────────────────

// Required by alloc8's mac_threads.cpp for thread-created flag
extern "C" volatile int xxthread_created_flag = 0;

using SmashRedirect = alloc8::HeapRedirect<smash::SmashHeap>;
ALLOC8_REDIRECT_WITH_THREADS(SmashRedirect);
