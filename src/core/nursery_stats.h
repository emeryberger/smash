// smash/src/core/nursery_stats.h - Measurement-only instrumentation for the
// per-thread page-nursery design question.
//
// Quantifies, on real workloads, the structure that decides whether a page
// nursery (whole-page local reuse + Hound-style age sealing) would pay off:
//
//   Q1  Same-thread whole-span turnover: how often does a span return to
//       fully-empty, and how often is the freeing thread the one that
//       allocated it? This is the ceiling on the throughput benefit of
//       recycling a whole freed page locally instead of round-tripping it
//       through the slab (decommit -> recommit -> kernel zero-fill).
//
//   Q3  Page age-at-first-cold: the elapsed-allocation age (in refill-batch
//       epochs) between a span's birth and the first time one of its pages is
//       selected for compression. If pages cool shortly after birth, sealing
//       by age buys little; if they keep accepting trickle allocations for a
//       long time before cooling, that window is where the RSS is.
//
//   Q2  (added later, sampled) LIFO-reuse-onto-a-cooling-page rate.
//
// Off by default (SMASH_NURSERY_STATS=1). Every hook is a single predicted
// branch when off. All state is in bootstrap/global memory — never on a data
// page, so the compression invariant holds. Dumped from the SIGUSR2 stats
// handler.
#pragma once

#include "../util/safe_printf.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <unistd.h>

namespace smash {

inline bool nurseryStatsOn() {
    static const int on = [] {
        const char* v = std::getenv("SMASH_NURSERY_STATS");
        return (v && v[0] == '1') ? 1 : 0;
    }();
    return on;
}

// Coarse "elapsed allocations" clock: bumped once per slab refill batch
// (~kThreadCacheBatchSize allocations). Monotonic; stamps span birth and dates
// age-at-first-cold. Unit is deliberately batches, not individual allocations —
// it only has to be monotone with churn.
inline std::atomic<uint64_t> g_nursery_epoch{0};
inline uint64_t nurseryEpoch() { return g_nursery_epoch.load(std::memory_order_relaxed); }
inline void nurseryBumpEpoch() {
    if (nurseryStatsOn()) g_nursery_epoch.fetch_add(1, std::memory_order_relaxed);
}

// Small dense per-thread id for same-thread-free attribution.
inline uint32_t nurseryTid() {
    static std::atomic<uint32_t> next{1};
    thread_local uint32_t id = next.fetch_add(1, std::memory_order_relaxed);
    return id;
}

struct NurseryCounters {
    std::atomic<uint64_t> span_births{0};
    std::atomic<uint64_t> span_turnovers{0};          // span went fully empty
    std::atomic<uint64_t> span_turnovers_same_tid{0}; // ...freed by its allocator
    std::atomic<uint64_t> reuse_samples{0};           // Q2: sampled hot-path allocs
    std::atomic<uint64_t> reuse_onto_cold{0};         // Q2: ...page was cooling
    std::atomic<uint64_t> cold_pages{0};              // pages first reaching cold
    // Age-at-first-cold histogram, log2(epoch delta) buckets.
    static constexpr int kAgeBuckets = 24;
    std::atomic<uint64_t> age_hist[kAgeBuckets]{};
};

inline NurseryCounters g_nursery;

inline void nurseryOnSpanBirth() {
    if (!nurseryStatsOn()) return;
    g_nursery.span_births.fetch_add(1, std::memory_order_relaxed);
}

inline void nurseryOnTurnover(bool same_tid) {
    if (!nurseryStatsOn()) return;
    g_nursery.span_turnovers.fetch_add(1, std::memory_order_relaxed);
    if (same_tid) g_nursery.span_turnovers_same_tid.fetch_add(1, std::memory_order_relaxed);
}

inline void nurseryOnReuseSample(bool onto_cold) {
    // Caller has already checked the gate + sampling.
    g_nursery.reuse_samples.fetch_add(1, std::memory_order_relaxed);
    if (onto_cold) g_nursery.reuse_onto_cold.fetch_add(1, std::memory_order_relaxed);
}

inline void nurseryOnPageCold(uint64_t age_epochs) {
    if (!nurseryStatsOn()) return;
    g_nursery.cold_pages.fetch_add(1, std::memory_order_relaxed);
    int b = 0;
    while ((age_epochs >> b) && b < NurseryCounters::kAgeBuckets - 1) ++b;
    g_nursery.age_hist[b].fetch_add(1, std::memory_order_relaxed);
}

// Emit the nursery report. Signal-safe (relaxed atomic loads + safe_snprintf +
// write(2)). Called from the SIGUSR2 handler when the gate is on.
inline void nurseryDump() {
    if (!nurseryStatsOn()) return;
    auto ld = [](std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); };
    uint64_t births = ld(g_nursery.span_births);
    uint64_t turn = ld(g_nursery.span_turnovers);
    uint64_t turn_st = ld(g_nursery.span_turnovers_same_tid);
    uint64_t rs = ld(g_nursery.reuse_samples);
    uint64_t roc = ld(g_nursery.reuse_onto_cold);
    uint64_t cold = ld(g_nursery.cold_pages);

    char buf[512];
    int n = smash::safe_snprintf(buf, sizeof(buf),
        "[nursery] epoch=%llu span_births=%llu turnovers=%llu(%.1f%% of births) "
        "same_tid_turnovers=%llu(%.1f%% of turnovers) "
        "reuse_samples=%llu reuse_onto_cold=%llu(%.1f%%) cold_pages=%llu\n",
        (unsigned long long)nurseryEpoch(),
        (unsigned long long)births,
        (unsigned long long)turn,   births ? 100.0 * turn / births : 0.0,
        (unsigned long long)turn_st, turn ? 100.0 * turn_st / turn : 0.0,
        (unsigned long long)rs,
        (unsigned long long)roc,    rs ? 100.0 * roc / rs : 0.0,
        (unsigned long long)cold);
    if (n > 0) (void)!::write(2, buf, (size_t)n);

    // Age-at-first-cold histogram: one line, nonzero log2 buckets only.
    char hb[512];
    int off = smash::safe_snprintf(hb, sizeof(hb), "[nursery] age_at_cold(epochs):");
    for (int b = 0; b < NurseryCounters::kAgeBuckets && off > 0 && off < (int)sizeof(hb) - 32; ++b) {
        uint64_t c = ld(g_nursery.age_hist[b]);
        if (!c) continue;
        // Bucket b covers ages [2^(b-1), 2^b); print the upper bound.
        unsigned upper = (b == 0) ? 1u : (1u << b);
        int m = smash::safe_snprintf(hb + off, sizeof(hb) - off,
                                     " <%u:%llu", upper, (unsigned long long)c);
        if (m > 0) off += m;
    }
    if (off > 0 && off < (int)sizeof(hb) - 1) { hb[off++] = '\n'; (void)!::write(2, hb, (size_t)off); }
}

} // namespace smash
