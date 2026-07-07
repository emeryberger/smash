// bench_extent_registration.cpp — prototype + measurement for O(1) external
// mapping registration in VmRegion.
//
// Background: SMASH_TRACK_EXTERNAL registers application-direct mmap regions
// page-by-page in an open-addressing hash (VmRegion::trackExternalPage). A
// single huge mapping (e.g. an InnoDB buffer pool) costs O(pages) hash inserts
// at registration and O(1) *amortized* but O(probe) worst-case per lookup, and
// is hard-capped at kTrackMaxPages slots. PR #53 bounded the pathological case
// by skipping oversized mappings — correct, but it means a large arena is
// simply untracked (no compression) rather than tracked cheaply.
//
// This prototype implements an EXTENT registry: one {base, first_index,
// page_count} record per mapping. Registration is O(1) regardless of page
// count; addr<->index lookups are O(log E) over the extent list (E = number of
// live mappings, typically tiny). It preserves the invariant the compressor
// relies on: every tracked page has a stable global index in a contiguous
// range, so PageStateTable / PageLockTable / the flat span table still index
// by page.
//
// We reimplement the CURRENT hash path here (byte-faithful to vm_region.h) so
// the comparison is apples-to-apples, then measure both on a workload that
// mimics the failure case: a few large single mmaps plus many small ones.
//
// Build: g++ -O2 -std=c++20 bench_extent_registration.cpp -o bench_extent_registration
// (standalone; no smash link — this is an algorithm prototype/benchmark.)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <vector>
#include <algorithm>
#include <random>

using Clock = std::chrono::steady_clock;
static double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

static constexpr size_t kPageShift = 12;          // 4 KiB pages (cloudnew)
static constexpr size_t kPageSize  = 1u << kPageShift;
static constexpr size_t kTrackMaxPages = 128 * 1024;   // matches vm_region.h
static constexpr size_t kTrackHashCap  = 256 * 1024;   // 2x headroom
static constexpr size_t kTrackHashMask = kTrackHashCap - 1;

// ─────────────────────────────────────────────────────────────────────────
// APPROACH 1 — current open-addressing per-page hash (faithful reimpl of
// VmRegion::trackExternalPage / lookupIdx / pageAddress, single-threaded so
// the atomics collapse to plain loads/stores).
// ─────────────────────────────────────────────────────────────────────────
struct HashRegistry {
    struct Entry { uintptr_t key = 0; size_t idx = 0; };
    std::vector<Entry> hash = std::vector<Entry>(kTrackHashCap);
    std::vector<uintptr_t> reverse = std::vector<uintptr_t>(kTrackMaxPages, 0);
    size_t slot_next = 0;
    size_t contig_pages = 0;

    // returns global index (>= contig_pages) or 0 on failure/full
    size_t trackPage(uintptr_t page_addr) {
        if (slot_next >= kTrackMaxPages) return 0;   // PR#53 early-out
        uintptr_t key = page_addr >> kPageShift;
        if (key == 0) return 0;
        size_t slot = size_t(key * 0x9E3779B97F4A7C15ULL) >> (64 - 18);
        for (size_t i = 0; i < kTrackHashCap; ++i) {
            size_t s = (slot + i) & kTrackHashMask;
            uintptr_t existing = hash[s].key;
            if (existing == key) return hash[s].idx;
            if (existing == 0) {
                hash[s].key = key;
                size_t local = slot_next++;
                if (local >= kTrackMaxPages) return 0;
                size_t idx = contig_pages + local;
                reverse[local] = page_addr;
                hash[s].idx = idx;
                return idx;
            }
        }
        return 0;
    }
    // register a whole mapping page-by-page
    size_t registerRange(uintptr_t base, size_t npages) {
        size_t tracked = 0;
        for (size_t p = 0; p < npages; ++p)
            if (trackPage(base + p * kPageSize)) ++tracked;
        return tracked;
    }
    size_t lookupIdx(uintptr_t addr) const {
        uintptr_t key = addr >> kPageShift;
        if (key == 0) return 0;
        size_t slot = size_t(key * 0x9E3779B97F4A7C15ULL) >> (64 - 18);
        for (size_t i = 0; i < kTrackHashCap; ++i) {
            size_t s = (slot + i) & kTrackHashMask;
            uintptr_t existing = hash[s].key;
            if (existing == key) return hash[s].idx;
            if (existing == 0) return 0;
        }
        return 0;
    }
    void* pageAddress(size_t idx) const {
        if (idx < contig_pages) return nullptr;
        size_t local = idx - contig_pages;
        if (local >= kTrackMaxPages) return nullptr;
        uintptr_t a = reverse[local];
        return a ? reinterpret_cast<void*>(a) : nullptr;
    }
};

// ─────────────────────────────────────────────────────────────────────────
// APPROACH 2 — extent registry. One record per mapping. Index space is a
// contiguous block per extent, assigned by bumping a global counter by
// page_count (O(1) registration). addr->idx and idx->addr are binary searches
// over the (small) extent list. Capacity is bounded by the index space, not by
// a per-page slot count — so a single 8 GiB arena costs ONE record.
// ─────────────────────────────────────────────────────────────────────────
struct ExtentRegistry {
    struct Extent {
        uintptr_t base;        // page-aligned start address
        size_t    first_index; // global index of base page
        size_t    npages;
        bool      live;
    };
    // Two orderings for O(log E) lookup. Kept sorted by base and by first_index.
    // In the allocator these would be small (E = live external mappings), so a
    // sorted vector with binary search beats a tree. Registration appends and
    // keeps both sorted (insertion is O(E), negligible for small E).
    std::vector<Extent> by_addr;   // sorted by base
    std::vector<Extent> by_index;  // sorted by first_index
    size_t index_next = 0;         // next free external index (local)
    size_t contig_pages = 0;

    // O(1) amortized registration (the sorted-insert is O(E), E tiny).
    size_t registerRange(uintptr_t base, size_t npages) {
        size_t first_local = index_next;
        index_next += npages;                        // O(1) index-block bump
        size_t first_index = contig_pages + first_local;
        Extent e{base, first_index, npages, true};
        // insert keeping by_addr sorted on base
        auto ia = std::lower_bound(by_addr.begin(), by_addr.end(), base,
            [](const Extent& x, uintptr_t b){ return x.base < b; });
        by_addr.insert(ia, e);
        auto ii = std::lower_bound(by_index.begin(), by_index.end(), first_index,
            [](const Extent& x, size_t fi){ return x.first_index < fi; });
        by_index.insert(ii, e);
        return npages;
    }
    // addr -> global index (0 if not covered)
    size_t lookupIdx(uintptr_t addr) const {
        // upper_bound by base, step back one → candidate extent
        auto it = std::upper_bound(by_addr.begin(), by_addr.end(), addr,
            [](uintptr_t a, const Extent& x){ return a < x.base; });
        if (it == by_addr.begin()) return 0;
        --it;
        if (!it->live) return 0;
        uintptr_t end = it->base + it->npages * kPageSize;
        if (addr < it->base || addr >= end) return 0;
        size_t page_off = (addr - it->base) >> kPageShift;
        return it->first_index + page_off;
    }
    void* pageAddress(size_t idx) const {
        auto it = std::upper_bound(by_index.begin(), by_index.end(), idx,
            [](size_t i, const Extent& x){ return i < x.first_index; });
        if (it == by_index.begin()) return nullptr;
        --it;
        if (!it->live) return nullptr;
        if (idx < it->first_index || idx >= it->first_index + it->npages)
            return nullptr;
        size_t off = idx - it->first_index;
        return reinterpret_cast<void*>(it->base + off * kPageSize);
    }
    // O(log E) untrack: mark the covering extent dead (address-range munmap of
    // a whole mapping is the common case; partial munmap would split, omitted
    // in the prototype — noted as a design point).
    void untrackRange(uintptr_t base) {
        auto it = std::lower_bound(by_addr.begin(), by_addr.end(), base,
            [](const Extent& x, uintptr_t b){ return x.base < b; });
        if (it != by_addr.end() && it->base == base) {
            it->live = false;
            auto ji = std::lower_bound(by_index.begin(), by_index.end(), it->first_index,
                [](const Extent& x, size_t fi){ return x.first_index < fi; });
            if (ji != by_index.end() && ji->first_index == it->first_index)
                ji->live = false;
        }
    }
    size_t liveExtents() const {
        size_t n = 0; for (auto& e : by_addr) if (e.live) ++n; return n;
    }
};

// ── Workload ───────────────────────────────────────────────────────────────
// Simulate a process under SMASH_TRACK_EXTERNAL: a small number of LARGE single
// mmaps (DB buffer pool / GC heap style) plus many small mmaps. Then do random
// page lookups (the compressor tick + fault handler path).

struct Mapping { uintptr_t base; size_t npages; };

static std::vector<Mapping> makeWorkload(size_t big_pool_pages, int n_small,
                                         std::mt19937_64& rng) {
    std::vector<Mapping> maps;
    uintptr_t cursor = 0x100000000000ULL;  // arbitrary high base
    // one big arena
    maps.push_back({cursor, big_pool_pages});
    cursor += (big_pool_pages + 16) * kPageSize;
    // many small mappings (1..64 pages)
    std::uniform_int_distribution<size_t> small(1, 64);
    for (int i = 0; i < n_small; ++i) {
        size_t np = small(rng);
        maps.push_back({cursor, np});
        cursor += (np + 4) * kPageSize;  // gap so ranges don't touch
    }
    return maps;
}

int main(int argc, char** argv) {
    // big pool size in MiB (default 512 = fits hash budget; try 2048/8192 to
    // show the hash cliff vs extent flat cost).
    size_t big_mib = (argc > 1) ? strtoull(argv[1], nullptr, 10) : 512;
    int n_small    = (argc > 2) ? atoi(argv[2]) : 2000;
    size_t big_pages = big_mib * 1024 * 1024 / kPageSize;

    std::mt19937_64 rng(12345);
    auto maps = makeWorkload(big_pages, n_small, rng);

    printf("=== extent-registration prototype ===\n");
    printf("big arena: %zu MiB = %zu pages; small mappings: %d; "
           "hash slot budget: %zu pages\n\n",
           big_mib, big_pages, n_small, kTrackMaxPages);

    // ── Registration cost ────────────────────────────────────────────────
    HashRegistry hashReg;
    {
        auto t0 = Clock::now();
        size_t tracked = 0;
        for (auto& m : maps) tracked += hashReg.registerRange(m.base, m.npages);
        double t = ms_since(t0);
        printf("[hash]   register %zu mappings: %.2f ms  (tracked %zu pages, "
               "slot_next=%zu%s)\n",
               maps.size(), t, tracked, hashReg.slot_next,
               hashReg.slot_next >= kTrackMaxPages ? " CAPPED" : "");
    }
    ExtentRegistry extReg;
    {
        auto t0 = Clock::now();
        size_t tracked = 0;
        for (auto& m : maps) tracked += extReg.registerRange(m.base, m.npages);
        double t = ms_since(t0);
        printf("[extent] register %zu mappings: %.2f ms  (tracked %zu pages, "
               "live extents=%zu)\n\n",
               maps.size(), t, tracked, extReg.liveExtents());
    }

    // ── Lookup cost ──────────────────────────────────────────────────────
    // Build a set of valid addresses (sample pages across all mappings).
    std::vector<uintptr_t> probe_addrs;
    probe_addrs.reserve(1'000'000);
    {
        std::uniform_int_distribution<size_t> mi(0, maps.size() - 1);
        for (int i = 0; i < 1'000'000; ++i) {
            auto& m = maps[mi(rng)];
            std::uniform_int_distribution<size_t> pi(0, m.npages - 1);
            probe_addrs.push_back(m.base + pi(rng) * kPageSize);
        }
    }
    volatile size_t sink = 0;
    {
        auto t0 = Clock::now();
        for (uintptr_t a : probe_addrs) sink += hashReg.lookupIdx(a);
        double t = ms_since(t0);
        printf("[hash]   1e6 addr->idx lookups: %.2f ms  (%.1f ns/lookup)\n",
               t, t * 1e6 / probe_addrs.size());
    }
    {
        auto t0 = Clock::now();
        for (uintptr_t a : probe_addrs) sink += extReg.lookupIdx(a);
        double t = ms_since(t0);
        printf("[extent] 1e6 addr->idx lookups: %.2f ms  (%.1f ns/lookup)\n\n",
               t, t * 1e6 / probe_addrs.size());
    }

    // ── HYBRID: large mappings → extents, small mappings → hash ───────────
    // This is the proposed production shape. Route any mapping whose page
    // count exceeds a threshold to the extent registry; keep the per-page hash
    // for the many small mappings (SpiderMonkey GC arenas etc.). Lookup tries
    // the (tiny) extent list first, then the hash. Gives O(1) registration +
    // full coverage for big arenas AND a short hash for the small ones.
    static constexpr size_t kExtentThresholdPages = 256;  // 1 MiB @ 4 KiB
    HashRegistry hy_hash; ExtentRegistry hy_ext;
    {
        auto t0 = Clock::now();
        size_t th = 0, te = 0;
        for (auto& m : maps) {
            if (m.npages >= kExtentThresholdPages) te += hy_ext.registerRange(m.base, m.npages);
            else th += hy_hash.registerRange(m.base, m.npages);
        }
        double t = ms_since(t0);
        printf("[hybrid] register %zu mappings: %.2f ms  (extent pages=%zu in "
               "%zu extents, hash pages=%zu)\n",
               maps.size(), t, te, hy_ext.liveExtents(), th);
    }
    {
        auto t0 = Clock::now();
        for (uintptr_t a : probe_addrs) {
            size_t idx = hy_ext.lookupIdx(a);
            if (idx == 0) idx = hy_hash.lookupIdx(a);
            sink += idx;
        }
        double t = ms_since(t0);
        printf("[hybrid] 1e6 addr->idx lookups: %.2f ms  (%.1f ns/lookup)\n\n",
               t, t * 1e6 / probe_addrs.size());
    }

    // ── Correctness cross-check (only where the hash actually tracked) ────
    // For every mapping page the hash tracked, extent must agree on a valid
    // index and round-trip addr->idx->addr.
    size_t checked = 0, mismatches = 0, rt_fail = 0;
    for (auto& m : maps) {
        for (size_t p = 0; p < m.npages; p += (m.npages > 1024 ? 997 : 1)) {
            uintptr_t a = m.base + p * kPageSize;
            size_t hi = hashReg.lookupIdx(a);
            size_t ei = extReg.lookupIdx(a);
            if (hi != 0) {  // hash tracked this page → both must be valid
                ++checked;
                // indices need not be equal (different assignment order),
                // but each registry must round-trip its own index back to addr.
                void* ha = hashReg.pageAddress(hi);
                if (ha != reinterpret_cast<void*>(a)) ++rt_fail;
            }
            if (ei != 0) {
                void* ea = extReg.pageAddress(ei);
                if (ea != reinterpret_cast<void*>(a)) ++rt_fail;
            }
            // extent should track EVERYTHING (no cap), hash may have capped
            if (hi != 0 && ei == 0) ++mismatches;
        }
    }
    printf("correctness: checked=%zu hash-tracked pages; extent round-trip "
           "failures=%zu; pages hash-tracked-but-extent-missed=%zu\n",
           checked, rt_fail, mismatches);
    printf("coverage: hash tracked %zu pages (capped at %zu); extent tracked "
           "%zu pages (all mappings)\n",
           hashReg.slot_next, kTrackMaxPages, extReg.index_next);

    (void)sink;
    return (rt_fail == 0 && mismatches == 0) ? 0 : 1;
}
