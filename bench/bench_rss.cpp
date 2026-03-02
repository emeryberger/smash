// bench_rss.cpp - RSS reduction over time: allocate, idle, sample RSS
#include "smash_heap.h"
#include "smash/config.h"

#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <fstream>
#include <string>
#endif

using namespace smash;

static size_t getCurrentRSSBytes() {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return info.resident_size;
    }
    return 0;
#elif defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            size_t kb = 0;
            sscanf(line.c_str(), "VmRSS: %zu kB", &kb);
            return kb * 1024;
        }
    }
    return 0;
#else
    return 0;
#endif
}

int main() {
    alignas(SmashHeap) static char heap_buf[sizeof(SmashHeap)];
    auto* heap = new (heap_buf) SmashHeap();
    heap->threadInit();
    // Second threadInit to start compression
    heap->threadInit();

    fprintf(stdout, "=== RSS Reduction Benchmark ===\n");
    fprintf(stdout, "Page size: %zu, Compress interval: %d ms, Cold ticks: %d\n\n",
            kPageSize, kCompressIntervalMs, kColdTicks);

    size_t initial_rss = getCurrentRSSBytes();
    fprintf(stdout, "Initial RSS: %.1f MB\n", initial_rss / (1024.0 * 1024.0));

    // Allocate ~64MB of data in 4KB chunks
    constexpr size_t kAllocSize = 4096;
    constexpr size_t kTotalAlloc = 64 * 1024 * 1024;
    constexpr size_t kNumAllocs = kTotalAlloc / kAllocSize;

    void** ptrs = new void*[kNumAllocs];
    for (size_t i = 0; i < kNumAllocs; ++i) {
        ptrs[i] = heap->malloc(kAllocSize);
        if (ptrs[i]) {
            // Fill with compressible pattern
            memset(ptrs[i], 0x42, kAllocSize);
        }
    }

    size_t peak_rss = getCurrentRSSBytes();
    fprintf(stdout, "After allocation: %.1f MB (+%.1f MB)\n",
            peak_rss / (1024.0 * 1024.0),
            (peak_rss - initial_rss) / (1024.0 * 1024.0));

    // Stop writing to pages and let them go cold
    fprintf(stdout, "\nWaiting for compression (sampling RSS every second)...\n");
    for (int sec = 1; sec <= 10; ++sec) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        size_t rss = getCurrentRSSBytes();
        double reduction = 1.0 - (static_cast<double>(rss) / peak_rss);
        fprintf(stdout, "  t=%2ds: RSS=%.1f MB (%.0f%% reduction from peak)\n",
                sec, rss / (1024.0 * 1024.0), reduction * 100.0);
    }

    // Touch some pages to trigger decompression
    fprintf(stdout, "\nTouching 10%% of pages...\n");
    for (size_t i = 0; i < kNumAllocs; i += 10) {
        if (ptrs[i]) {
            volatile uint8_t* p = static_cast<volatile uint8_t*>(ptrs[i]);
            p[0] = 0xFF;  // Trigger fault if compressed
        }
    }

    size_t after_touch = getCurrentRSSBytes();
    fprintf(stdout, "After touching: %.1f MB\n", after_touch / (1024.0 * 1024.0));

    // Cleanup
    for (size_t i = 0; i < kNumAllocs; ++i) {
        heap->free(ptrs[i]);
    }
    delete[] ptrs;

    size_t final_rss = getCurrentRSSBytes();
    fprintf(stdout, "After free: %.1f MB\n\n", final_rss / (1024.0 * 1024.0));

    return 0;
}
