// bench_throughput.cpp - Measure malloc/free ops/sec across size classes
#include "smash_heap.h"
#include "smash/config.h"

#include <cstdio>
#include <cstring>
#include <chrono>

using namespace smash;

alignas(SmashHeap) static char heap_buf[sizeof(SmashHeap)];
static SmashHeap* heap;

static void benchSizeClass(size_t size, int iterations) {
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        void* p = heap->malloc(size);
        heap->free(p);
    }

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_us = std::chrono::duration<double, std::micro>(end - start).count();
    double ops_per_sec = (iterations * 2.0) / (elapsed_us / 1e6);

    fprintf(stdout, "  size=%5zu: %.0f ops/sec (%.1f ns/op)\n",
            size, ops_per_sec, (elapsed_us * 1000.0) / (iterations * 2));
}

static void benchBatchPattern(size_t size, int batch_size, int iterations) {
    void** ptrs = new void*[batch_size];

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        for (int j = 0; j < batch_size; ++j)
            ptrs[j] = heap->malloc(size);
        for (int j = 0; j < batch_size; ++j)
            heap->free(ptrs[j]);
    }

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_us = std::chrono::duration<double, std::micro>(end - start).count();
    double ops_per_sec = (static_cast<double>(iterations) * batch_size * 2.0) / (elapsed_us / 1e6);

    fprintf(stdout, "  size=%5zu batch=%d: %.0f ops/sec\n",
            size, batch_size, ops_per_sec);

    delete[] ptrs;
}

int main() {
    heap = new (heap_buf) SmashHeap();
    heap->threadInit();

    fprintf(stdout, "=== SmashHeap Throughput Benchmark ===\n\n");

    fprintf(stdout, "Single alloc/free:\n");
    size_t sizes[] = { 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384 };
    for (size_t sz : sizes) {
        benchSizeClass(sz, 1000000);
    }

    fprintf(stdout, "\nBatch alloc/free (64 at a time):\n");
    for (size_t sz : sizes) {
        benchBatchPattern(sz, 64, 10000);
    }

    return 0;
}
