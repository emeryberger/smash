// bench_latency.cpp - Alloc/free latency percentiles (p50/p99/p999)
#include "smash_heap.h"
#include "smash/config.h"

#include <cstdio>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <vector>

using namespace smash;

alignas(SmashHeap) static char heap_buf[sizeof(SmashHeap)];
static SmashHeap* heap;

struct LatencyResult {
    double p50_ns;
    double p99_ns;
    double p999_ns;
    double avg_ns;
};

static LatencyResult measureAllocLatency(size_t size, int samples) {
    std::vector<double> latencies(samples);

    for (int i = 0; i < samples; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        void* p = heap->malloc(size);
        auto end = std::chrono::high_resolution_clock::now();
        heap->free(p);
        latencies[i] = std::chrono::duration<double, std::nano>(end - start).count();
    }

    std::sort(latencies.begin(), latencies.end());

    double sum = 0;
    for (auto l : latencies) sum += l;

    return {
        latencies[samples / 2],
        latencies[static_cast<size_t>(samples * 0.99)],
        latencies[static_cast<size_t>(samples * 0.999)],
        sum / samples,
    };
}

static LatencyResult measureFreeLatency(size_t size, int samples) {
    std::vector<void*> ptrs(samples);
    for (int i = 0; i < samples; ++i) {
        ptrs[i] = heap->malloc(size);
    }

    std::vector<double> latencies(samples);
    for (int i = 0; i < samples; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        heap->free(ptrs[i]);
        auto end = std::chrono::high_resolution_clock::now();
        latencies[i] = std::chrono::duration<double, std::nano>(end - start).count();
    }

    std::sort(latencies.begin(), latencies.end());

    double sum = 0;
    for (auto l : latencies) sum += l;

    return {
        latencies[samples / 2],
        latencies[static_cast<size_t>(samples * 0.99)],
        latencies[static_cast<size_t>(samples * 0.999)],
        sum / samples,
    };
}

int main() {
    heap = new (heap_buf) SmashHeap();
    heap->threadInit();

    fprintf(stdout, "=== Alloc/Free Latency Benchmark ===\n\n");

    constexpr int kSamples = 100000;
    size_t sizes[] = { 16, 32, 64, 128, 256, 512, 1024, 4096, 16384 };

    fprintf(stdout, "Malloc latency (ns):\n");
    fprintf(stdout, "  %8s  %8s  %8s  %8s  %8s\n", "size", "avg", "p50", "p99", "p999");
    for (size_t sz : sizes) {
        auto r = measureAllocLatency(sz, kSamples);
        fprintf(stdout, "  %8zu  %8.0f  %8.0f  %8.0f  %8.0f\n",
                sz, r.avg_ns, r.p50_ns, r.p99_ns, r.p999_ns);
    }

    fprintf(stdout, "\nFree latency (ns):\n");
    fprintf(stdout, "  %8s  %8s  %8s  %8s  %8s\n", "size", "avg", "p50", "p99", "p999");
    for (size_t sz : sizes) {
        auto r = measureFreeLatency(sz, kSamples);
        fprintf(stdout, "  %8zu  %8.0f  %8.0f  %8.0f  %8.0f\n",
                sz, r.avg_ns, r.p50_ns, r.p99_ns, r.p999_ns);
    }

    return 0;
}
