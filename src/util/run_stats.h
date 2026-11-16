// smash/src/util/run_stats.h - always-on counters behind SMASH_REPORT and
// SMASH_METRICS_FILE
//
// Unlike pressure_stats.h (opt-in, per-page stamps), these are a handful of
// relaxed atomics that are always maintained, so the exit report and the
// metrics file can describe any run without a rerun:
//
//   * decompression-fault service latency: a fixed-bucket histogram plus
//     sum/count, recorded by handleFault (signal context: atomics and
//     clock_gettime only, both async-signal-safe);
//   * tracking faults that did not decompress (monitor / shadow / uffd);
//   * successful compressions and their input/output bytes, and pages the
//     ratio gate rejected;
//   * per-compressor-thread CPU time, published by each thread itself;
//   * RSS sampled once per compressor tick: highest sample and the lowest
//     sample taken after it.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>

namespace smash::rstats {

inline uint64_t monoNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(ts.tv_nsec);
}

inline uint64_t threadCpuNs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(ts.tv_nsec);
}

// Upper bounds (inclusive, microseconds) of the latency buckets; one more
// bucket catches everything above the last bound. 1-2-5 steps from 1 us to
// 1 s: a decompress fault is typically tens of us, a contended or
// prefetch-heavy one hundreds.
inline constexpr uint32_t kLatBoundsUs[] = {
    1, 2, 5, 10, 20, 50, 100, 200, 500,
    1000, 2000, 5000, 10000, 20000, 50000, 100000, 1000000};
inline constexpr int kLatBounds = sizeof(kLatBoundsUs) / sizeof(kLatBoundsUs[0]);
inline constexpr int kLatBuckets = kLatBounds + 1;
// Same bounds in seconds, for Prometheus `le` labels.
inline constexpr const char* kLatBoundsSec[kLatBounds] = {
    "1e-06", "2e-06", "5e-06", "1e-05", "2e-05", "5e-05", "0.0001", "0.0002",
    "0.0005", "0.001", "0.002", "0.005", "0.01", "0.02", "0.05", "0.1", "1"};

inline int latBucket(uint64_t ns) {
    const uint64_t us = (ns + 999) / 1000;  // round up: bucket bounds are inclusive
    for (int b = 0; b < kLatBounds; ++b)
        if (us <= kLatBoundsUs[b]) return b;
    return kLatBounds;
}

// Room for the coordinator plus every helper (kMaxHelpers is 15 today).
inline constexpr int kMaxCpuSlots = 32;

struct Counters {
    std::atomic<uint64_t> fault_hist[kLatBuckets]{};
    std::atomic<uint64_t> fault_ns_sum{0};
    std::atomic<uint64_t> decomp_faults{0};      // == sum of fault_hist
    std::atomic<uint64_t> tracking_faults{0};    // handled, no decompress
    std::atomic<uint64_t> compressions{0};       // pages stored compressed
    std::atomic<uint64_t> comp_in_bytes{0};      // page bytes compressed
    std::atomic<uint64_t> comp_out_bytes{0};     // blob bytes produced
    std::atomic<uint64_t> ratio_rejects{0};      // compressed too poorly to keep
    std::atomic<uint64_t> thread_cpu_ns[kMaxCpuSlots]{};
    std::atomic<uint64_t> rss_peak_kb{0};        // highest tick sample
    std::atomic<uint64_t> rss_min_after_peak_kb{0};
    std::atomic<uint64_t> rss_last_kb{0};
    std::atomic<uint64_t> start_ns{0};
};
inline Counters g;

inline void recordDecompressFault(uint64_t ns) {
    g.fault_hist[latBucket(ns)].fetch_add(1, std::memory_order_relaxed);
    g.fault_ns_sum.fetch_add(ns, std::memory_order_relaxed);
    g.decomp_faults.fetch_add(1, std::memory_order_relaxed);
}

inline void noteTrackingFault() {
    g.tracking_faults.fetch_add(1, std::memory_order_relaxed);
}

inline void noteCompressed(size_t in_bytes, size_t out_bytes) {
    g.compressions.fetch_add(1, std::memory_order_relaxed);
    g.comp_in_bytes.fetch_add(in_bytes, std::memory_order_relaxed);
    g.comp_out_bytes.fetch_add(out_bytes, std::memory_order_relaxed);
}

// Each compressor thread publishes its own cumulative CPU time.
inline void publishThreadCpu(int slot) {
    if (slot >= 0 && slot < kMaxCpuSlots)
        g.thread_cpu_ns[slot].store(threadCpuNs(), std::memory_order_relaxed);
}

inline uint64_t compressorCpuNs() {
    uint64_t s = 0;
    for (const auto& c : g.thread_cpu_ns) s += c.load(std::memory_order_relaxed);
    return s;
}

// Coordinator thread only (single writer).
inline void sampleRss(uint64_t kb) {
    if (kb == 0) return;
    g.rss_last_kb.store(kb, std::memory_order_relaxed);
    if (kb > g.rss_peak_kb.load(std::memory_order_relaxed)) {
        g.rss_peak_kb.store(kb, std::memory_order_relaxed);
        g.rss_min_after_peak_kb.store(kb, std::memory_order_relaxed);
    } else if (kb < g.rss_min_after_peak_kb.load(std::memory_order_relaxed)) {
        g.rss_min_after_peak_kb.store(kb, std::memory_order_relaxed);
    }
}

// Smallest bucket upper bound (us) covering quantile q of the histogram;
// 0 if empty, UINT32_MAX if it falls in the overflow bucket.
inline uint32_t latQuantileUs(double q) {
    uint64_t n = 0, h[kLatBuckets];
    for (int b = 0; b < kLatBuckets; ++b) {
        h[b] = g.fault_hist[b].load(std::memory_order_relaxed);
        n += h[b];
    }
    if (n == 0) return 0;
    uint64_t want = static_cast<uint64_t>(q * static_cast<double>(n) + 0.999999);
    if (want < 1) want = 1;
    uint64_t acc = 0;
    for (int b = 0; b < kLatBounds; ++b) {
        acc += h[b];
        if (acc >= want) return kLatBoundsUs[b];
    }
    return UINT32_MAX;
}

}  // namespace smash::rstats
