// Test multi-threaded contention: 8 threads × 100K alloc/free cycles on SmashHeap
#include "smash_heap.h"
#include "smash/config.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <pthread.h>
#include <atomic>

using namespace smash;

static int failures = 0;
static std::atomic<int> global_failures{0};

#define CHECK(cond, msg, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: " msg "\n", ##__VA_ARGS__); \
        global_failures.fetch_add(1, std::memory_order_relaxed); \
    } \
} while (0)

static SmashHeap* g_heap = nullptr;

struct ThreadArgs {
    int thread_id;
    int iterations;
};

static void* workerThread(void* arg) {
    auto* args = static_cast<ThreadArgs*>(arg);
    g_heap->threadInit();

    // Cycle through different size classes
    constexpr int kHoldCount = 64;
    void* held[kHoldCount]{};
    size_t sizes[] = { 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192 };
    constexpr int kNumSizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int iter = 0; iter < args->iterations; ++iter) {
        // Allocate a batch
        for (int j = 0; j < kHoldCount; ++j) {
            size_t sz = sizes[(iter + j) % kNumSizes];
            held[j] = g_heap->malloc(sz);
            CHECK(held[j] != nullptr, "thread %d: malloc(%zu) returned null at iter %d",
                  args->thread_id, sz, iter);
            if (held[j]) {
                // Write a pattern to verify no corruption
                memset(held[j], static_cast<int>(args->thread_id & 0xFF), sz);
            }
        }

        // Verify patterns
        for (int j = 0; j < kHoldCount; ++j) {
            if (!held[j]) continue;
            size_t sz = sizes[(iter + j) % kNumSizes];
            auto* bytes = static_cast<uint8_t*>(held[j]);
            uint8_t expected = static_cast<uint8_t>(args->thread_id & 0xFF);
            for (size_t b = 0; b < sz; ++b) {
                if (bytes[b] != expected) {
                    CHECK(false, "thread %d: data corruption at iter %d, slot %d, offset %zu",
                          args->thread_id, iter, j, b);
                    break;
                }
            }
        }

        // Free half randomly (odd indices)
        for (int j = 1; j < kHoldCount; j += 2) {
            g_heap->free(held[j]);
            held[j] = nullptr;
        }

        // Reallocate freed slots
        for (int j = 1; j < kHoldCount; j += 2) {
            size_t sz = sizes[(iter + j + 5) % kNumSizes];
            held[j] = g_heap->malloc(sz);
        }

        // Free everything
        for (int j = 0; j < kHoldCount; ++j) {
            g_heap->free(held[j]);
            held[j] = nullptr;
        }
    }

    g_heap->threadCleanup();
    return nullptr;
}

int main() {
    // SmashHeap is normally a singleton managed by alloc8, but for testing
    // we construct one directly. We need to be careful about bootstrap alloc.
    alignas(SmashHeap) static char heap_buf[sizeof(SmashHeap)];
    g_heap = new (heap_buf) SmashHeap();
    g_heap->threadInit();

    constexpr int kNumThreads = 8;
    constexpr int kIterations = 1000;  // Reduced for test speed; set higher for stress

    pthread_t threads[kNumThreads];
    ThreadArgs args[kNumThreads];

    fprintf(stderr, "contention: starting %d threads × %d iterations...\n",
            kNumThreads, kIterations);

    for (int i = 0; i < kNumThreads; ++i) {
        args[i] = { i, kIterations };
        int ret = pthread_create(&threads[i], nullptr, workerThread, &args[i]);
        CHECK(ret == 0, "pthread_create failed for thread %d", i);
    }

    for (int i = 0; i < kNumThreads; ++i) {
        pthread_join(threads[i], nullptr);
    }

    failures = global_failures.load();
    if (failures == 0) {
        fprintf(stderr, "contention: all tests passed (%d threads × %d iterations)\n",
                kNumThreads, kIterations);
        return 0;
    } else {
        fprintf(stderr, "contention: %d failures\n", failures);
        return 1;
    }
}
