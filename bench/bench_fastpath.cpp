// bench_fastpath.cpp — low-noise measurement of the malloc/free hot path.
//
// Reports the MINIMUM ns/op across many fixed-size windows. Min-filtering
// rejects scheduling / frequency-scaling noise, so it resolves sub-nanosecond
// changes to the inlined fast path (callsiteArena, thread-cache hit, free
// span-cache hit) that a single wall-clock average buries.
#include "smash_heap.h"
#include "smash/config.h"

#include <cstdio>
#include <cstdint>
#include <chrono>
#include <algorithm>

using namespace smash;
alignas(SmashHeap) static char heap_buf[sizeof(SmashHeap)];
static SmashHeap* heap;

// LIFO single alloc/free — pure thread-cache-hit path (no slab, no drain).
static double minNsPerOp_single(size_t size, int windows, int win_ops) {
    double best = 1e30;
    volatile void* sink;
    for (int w = 0; w < windows; ++w) {
        auto s = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < win_ops; ++i) {
            void* p = heap->malloc(size);
            sink = p;
            heap->free(p);
        }
        auto e = std::chrono::high_resolution_clock::now();
        double ns = std::chrono::duration<double, std::nano>(e - s).count() / (win_ops * 2.0);
        best = std::min(best, ns);
    }
    (void)sink;
    return best;
}

int main(int argc, char** argv) {
    heap = new (heap_buf) SmashHeap();
    heap->threadInit();

    int windows = 2000, win_ops = 20000;
    size_t sizes[] = {16, 64, 256, 1024};
    printf("=== fast-path min ns/op (best of %d windows x %d ops) ===\n", windows, win_ops);
    for (size_t sz : sizes) {
        // warm up
        minNsPerOp_single(sz, 50, win_ops);
        double ns = minNsPerOp_single(sz, windows, win_ops);
        printf("  single alloc/free size=%5zu : %.3f ns/op\n", sz, ns);
    }
    (void)argc; (void)argv;
    return 0;
}
