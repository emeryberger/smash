// bench_churn_reheat.cpp - Measures cooling-page closing (allocation churn
// re-heating pages that hold cold survivors).
//
// Scenario: a FIFO cache at steady state — the canonical shape (LRU caches,
// session tables, TTL maps). One call site owns all entries (same arena,
// same size class). Each round evicts the oldest E entries and inserts E new
// ones. Evictions free slots on the OLDEST pages — pages whose remaining
// entries were written long ago and are exactly the ones the compressor
// wants to compress. Eviction frees push those spans to the front of the
// partial list, so first-fit backfills the freshly-vacated slots on cooling
// pages, resetting their cold streaks every round: pages holding cold
// entries never stay quiet long enough to compress. With cooling-page
// closing (default), insertions redirect to decommitted-empty spans instead
// and the cooling pages drain monotonically, compress, and stay compressed.
//
// A/B (cold timeout must exceed the round period so pages are 'cooling',
// not yet compressed, when insertions arrive):
//   LD_PRELOAD=libsmash.so SMASH_COLD_TIMEOUT_SEC=6 SMASH_STATS_SIGNAL=1 \
//     SMASH_COOLING_CLOSE_TICKS=0 ./bench_churn_reheat   # closing OFF
//   LD_PRELOAD=libsmash.so SMASH_COLD_TIMEOUT_SEC=6 SMASH_STATS_SIGNAL=1 \
//     ./bench_churn_reheat                                # closing ON (default)
//
// Compare: METRIC churn_avg_rss_mb / late_rss_mb, and the reheat= counter in
// the [smash stats] line the bench triggers via SIGUSR2 at the end (stderr).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <csignal>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <unistd.h>
#elif defined(__linux__)
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#endif

static size_t getCurrentRSSBytes() {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return info.resident_size;
    return 0;
#elif defined(__linux__)
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0) return 0;
    char buf[4096];
    ssize_t n = syscall(SYS_read, fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    const char* p = strstr(buf, "VmRSS:");
    if (!p) return 0;
    size_t kb = 0;
    sscanf(p, "VmRSS: %zu kB", &kb);
    return kb * 1024;
#else
    return 0;
#endif
}

// When preloaded, smash defers compression until the second threadInit()
// (see bench_kv_store.cpp for the rationale).
static void triggerCompressorStart() {
    for (int i = 0; i < 2; ++i) {
        std::thread helper([] {
            volatile void* p = malloc(64);
            free(const_cast<void*>(p));
        });
        helper.join();
    }
}

static constexpr size_t kObjSize = 256;
// Single allocation site for the whole bench: survivors and churn share the
// arena and size class, as they would inside one cache/table implementation.
static volatile void* g_alloc_sink;
__attribute__((noinline)) static void* alloc_obj() {
    void* p = malloc(kObjSize);
    g_alloc_sink = p;
    return p;
}

// Compressible payload (repeated ASCII structure, varying tail).
static void fillPayload(void* p, uint64_t seq) {
    char* dst = static_cast<char*>(p);
    int n = snprintf(dst, kObjSize,
        "{\"session\":%llu,\"state\":\"established\",\"proto\":\"h2\","
        "\"peer\":\"10.0.%llu.%llu\",\"bytes\":%llu}",
        (unsigned long long)seq,
        (unsigned long long)(seq >> 8 & 0xFF),
        (unsigned long long)(seq & 0xFF),
        (unsigned long long)(seq * 1471));
    if (n > 0 && n < (int)kObjSize) memset(dst + n, ' ', kObjSize - n);
}

int main() {
    triggerCompressorStart();

    // ── Phase 1: fill the cache ─────────────────────────────────────────────
    // 128k entries = 32 MB, inserted in order (FIFO age == address order,
    // roughly, since spans fill sequentially).
    constexpr size_t kCapacity = 131072;
    printf("=== Churn re-heat benchmark (FIFO cache) ===\n");
    printf("Capacity: %zu x %zu B = %.1f MB\n", kCapacity, kObjSize,
           kCapacity * kObjSize / (1024.0 * 1024.0));
    fflush(stdout);

    // Append-ordered store of live entries; `head` marks the oldest
    // still-live index. Slots are nulled when evicted.
    std::vector<void*> live;
    live.reserve(kCapacity * 3);
    uint64_t seq = 0;
    for (size_t i = 0; i < kCapacity; ++i) {
        void* p = alloc_obj();
        fillPayload(p, seq++);
        live.push_back(p);
    }
    size_t head = 0;
    size_t peak_rss = getCurrentRSSBytes();
    printf("Filled; peak RSS %.1f MB\n", peak_rss / (1024.0 * 1024.0));
    fflush(stdout);

    // ── Phase 2: steady-state eviction/insertion rounds ─────────────────────
    // Each round, batched (interleaving free/alloc would just recycle each
    // slot through the thread cache and never exercise span placement):
    //   1. Evict kFifoEvict oldest entries (TTL sweep — vacates whole pages,
    //      feeding the empty-span list).
    //   2. Evict kRandEvict random entries (LRU scatter — punches holes into
    //      pages whose other entries are old and cooling).
    //   3. Insert kFifoEvict + kRandEvict new entries.
    // Then idle ~1.3 s. With a 6 s cold timeout, the scattered holes sit on
    // 'cooling' (not yet compressed) pages when the insertions arrive:
    // first-fit backfills them and re-heats the pages every round; closing
    // redirects the insertions to the vacated empty spans instead.
    constexpr int kRounds = 16;
    constexpr size_t kFifoEvict = 7168;
    constexpr size_t kRandEvict = 1024;
    uint64_t rng = 0x9E3779B97F4A7C15ULL;
    auto next_rand = [&rng]() {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        return rng;
    };
    double churn_rss_sum = 0, late_rss_sum = 0;
    int late_samples = 0;
    for (int r = 0; r < kRounds; ++r) {
        size_t evicted = 0;
        while (evicted < kFifoEvict && head < live.size()) {
            if (live[head]) { free(live[head]); live[head] = nullptr; ++evicted; }
            ++head;
        }
        // Scattered evictions target the cohort inserted ~4 rounds ago
        // (~5.2 s): old enough that its pages are cooling (streak past the
        // closing threshold of cold_ticks/2 = 3) but not yet compressed
        // (< 6 ticks). Backfilling THOSE holes is what perpetually vetoes
        // compression: the backfilled entries are young, so the page hosts
        // mixed ages forever and its streak resets every round. Entries
        // older than that sit on already-COMPRESSED pages, which the
        // pre-existing compressed-page avoidance protects in both configs.
        constexpr size_t kRoundIns = kFifoEvict + kRandEvict;
        size_t cohort_start = live.size() >= 3 * kRoundIns
                            ? live.size() - 3 * kRoundIns : head;
        size_t cohort_len = std::min(kRoundIns, live.size() - cohort_start);
        for (size_t i = 0; i < kRandEvict; ++i) {
            size_t idx = cohort_start + next_rand() % cohort_len;
            while (idx < live.size() && !live[idx]) ++idx;
            if (idx < live.size()) { free(live[idx]); live[idx] = nullptr; }
        }
        for (size_t i = 0; i < kFifoEvict + kRandEvict; ++i) {
            void* p = alloc_obj();
            fillPayload(p, seq++);
            live.push_back(p);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1300));
        double rss_mb = getCurrentRSSBytes() / (1024.0 * 1024.0);
        churn_rss_sum += rss_mb;
        if (r >= kRounds / 2) { late_rss_sum += rss_mb; ++late_samples; }
        if ((r + 1) % 4 == 0) {
            printf("  round %d: RSS=%.1f MB\n", r + 1, rss_mb);
            fflush(stdout);
        }
    }
    double churn_avg_rss = churn_rss_sum / kRounds;
    double late_rss = late_samples ? late_rss_sum / late_samples : 0;

    // ── Phase 3: final cool, then report ────────────────────────────────────
    std::this_thread::sleep_for(std::chrono::seconds(8));
    size_t end_rss = getCurrentRSSBytes();

    // Touch a sample of live entries to prove integrity (round-trips through
    // decompression for entries whose pages compressed).
    uint64_t checksum = 0;
    for (size_t i = head; i < live.size(); i += 64) {
        if (!live[i]) continue;
        const char* s = static_cast<const char*>(live[i]);
        checksum += static_cast<uint8_t>(s[0]) + static_cast<uint8_t>(s[12]);
    }

    double reduction = peak_rss ? (1.0 - double(end_rss) / peak_rss) * 100 : 0;
    printf("\nMETRIC peak_rss_mb %.1f\n", peak_rss / (1024.0 * 1024.0));
    printf("METRIC churn_avg_rss_mb %.1f\n", churn_avg_rss);
    printf("METRIC late_rss_mb %.1f\n", late_rss);
    printf("METRIC end_rss_mb %.1f\n", end_rss / (1024.0 * 1024.0));
    printf("METRIC rss_reduction_pct %.1f\n", reduction);
    printf("METRIC entry_checksum %llu\n", (unsigned long long)checksum);
    fflush(stdout);

    // Ask a preloaded libsmash for its stats line — includes the reheat=
    // counter. Requires SMASH_STATS_SIGNAL=1 (the SIGUSR2 handler install is
    // gated; see compressor_thread.h).
    raise(SIGUSR2);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return 0;
}
