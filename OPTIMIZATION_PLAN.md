# Smash malloc fast-path optimization plan

## Context

PR #13 closed the compression-correctness story: the postgres-shim
perf-comparison ran 25/25 with zero timeouts, the previous "stable"
config compressed effectively zero pages and this one compresses
~4700/run, and an interleaved A/B with compression turned off
(`SMASH_COLD_TICKS=9999`) lands within noise of the
compression-on number (5118 vs 5139 tps median).

That last data point is the key takeaway for this plan: **the entire
−23.4 % gap between `shim+smash` and `shim+jemalloc` lives in the
allocator fast path, not in compression work.**

Headline 5×5 numbers (aarch64 Ubuntu 24.04, 2-CPU Parallels VM,
`--mode=compare --runs 5 --clients 2 --perf-duration 30`):

```
config           runs    tps_med    tps_min    tps_max    Δ vs stock
stock               5     6855.7     6617.4     7002.1         —
stock+jemalloc      5     6920.8     6698.1     6966.2      +1.0%
shim                5     6399.4     6307.3     6444.8      −6.7%
shim+jemalloc       5     6703.6     6530.6     6847.3      −2.2%
shim+smash          5     5135.8     5008.2     5204.5     −25.1%
```

The smash-specific overhead vs the next-best malloc is `(6704 − 5136) /
6704 = 23.4 %`, or **+90 µs/transaction** at the median. Pgbench
transactions issue O(50–150) palloc/pfree pairs each, so we're paying
roughly **0.4–1.0 µs of smash-specific overhead per malloc/free pair**.

Goal of this branch: shrink that to **≤10 %** vs `shim+jemalloc`
(equivalent to ~6030 tps on this box) without breaking the compression
correctness now in master.

## What's already known

- LTO is enabled (`CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE` in
  `CMakeLists.txt`) when the toolchain supports it.
- The fast path on Linux is:
  ```
  malloc(sz) → alloc8::do_malloc → HeapRedirect::getHeap()->malloc
              → SmashHeap::malloc(sz)
                → sizeToClass(sz)
                → callsiteArena(sc)              // hashes ra + sh + sc
                → getOrCreateThreadCache()       // thread_local load + null check
                → tc->allocate(sc)               // count check + array index
              → return
  ```
  Most of these are inline candidates under LTO; we have not actually
  *checked* the codegen.
- `free()` always runs `PageMap::get(ptr)`, a two-level radix lookup,
  even when the freed pointer just came from the same span as the last
  free.
- `getOrCreateThreadCache()` does its null-check on every call; the
  result never changes within a thread.
- The compressor coordinator + 2 worker threads spawn unconditionally
  per process. On a 2-CPU VM with 5–7 concurrent postgres backends,
  that's ~21 smash threads competing for 2 cores — context-switch tax
  is small but non-zero.

## Approach overall

A single principle drives this branch:

> **Verify before optimizing.** Each candidate gets an objdump or a perf
> sample showing it's actually a hot instruction *before* we touch it,
> and an A/B `--mode=compare` measurement after to confirm the change
> moved the number.

Per-iteration work:

1. Make the smallest possible focused change.
2. `cd linux-build && make -j` + `ctest --output-on-failure` (15/15).
3. `python3 bench/run_quick_ci.py --build-dir linux-build` (both gates).
4. `python3 bench/postgres_shim_build.py linux-build/libsmash.so
    --mode=compare --runs 5 --clients 2 --perf-duration 30` —
   record the tps_med delta vs the prior baseline.
5. Commit with the delta in the message body.

Cool-tail compression must stay ≥ 95 % at every step
(`python3 bench/postgres_shim_build.py linux-build/libsmash.so` default
mode). It's currently at 99.3 %.

## Optimization candidates, ranked by expected ROI

### 1. Codegen audit of the malloc fast path  *[verification]*

Before any code change, confirm what the optimizer actually produced.

```
objdump -d --no-show-raw-insn linux-build/libsmash.so \
  | awk '/<.*malloc.*>:/{p=1} p; /^$/{p=0}' \
  > /tmp/smash_malloc_codegen.txt
```

Things to look for:
- Is `SmashHeap::malloc` inlined into `do_malloc`, or is there an
  indirect call? Library boundary may defeat LTO even when it's nominally
  enabled.
- Is `tc->allocate(sc)` inlined, or a plain `bl`/`blr`?
- Is `getOrCreateThreadCache` issuing a function call or did it inline?
- Is `__builtin_return_address` / `__builtin_frame_address` generating
  unwind prologue work?

If any of those is *not* inlined, mark the function `[[gnu::always_inline]]`
or move its definition to the header. **Likely win: 30–60 µs/txn (largest
single candidate)**.

### 2. Per-thread "last span" MRU cache for `free()`

`SmashHeap::free` always runs `page_map_.get(ptr)` (two-level radix).
For typical workloads, consecutive frees from a thread frequently hit
the same span — pgbench backends churn allocations from a small set of
spans (one per palloc size class).

Add a 1-entry per-thread cache:

```cpp
inline std::pair<Span*, uintptr_t>& currentFreeMRU() {
    static thread_local Span* span = nullptr;
    static thread_local uintptr_t span_lo = 0;
    static thread_local uintptr_t span_hi = 0;
    return ...;
}

void free(void* ptr) {
    if (!ptr) return;
    if (BootstrapAlloc::instance().owns(ptr)) return;

    uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
    Span* span;
    if (mru_lo <= p && p < mru_hi) {
        span = mru_span;
    } else {
        span = page_map_.get(p);
        if (span && !span->is_large) {
            mru_span = span;
            mru_lo = (uintptr_t)span->base;
            mru_hi = mru_lo + span->page_count * kPageSize;
        }
    }
    // ... rest unchanged
}
```

Cost: 24 bytes of TLS per thread, two extra branches on free. Drop-in,
no API change. **Likely win: 15–25 µs/txn**.

### 3. Bump `kThreadCacheMaxPerClass`

Refill cost is amortized over the cache size. `kThreadCacheMaxPerClass`
is a single number in `include/smash/config.h`. Doubling it halves the
refill rate and cuts slab-lock acquisition by 2×.

Trade-off: more memory in thread caches when they're full (idle
thread-local storage). At 64 size classes × N pointers × 8 bytes per
pointer × M threads, the absolute number stays small (<1 MiB even at
N=128, M=8).

A/B measure with the current value, 2×, 4× — pick the knee. **Likely
win: 10–20 µs/txn**, but risk of inflating idle memory; cool-tail RSS
ratio is the gate.

### 4. Single-direct-mapped page-to-span table

`PageMap` is a two-level radix because the address space is 2⁴⁸. But
smash's data pages all live inside `g_smash_vm_region`, a contiguous
reservation of `kVmRegionSize` (~16 GiB). Within that range, a single
direct-mapped `Span**` table (one entry per page) is feasible.

Layout:
- `Span* page_to_span_[total_pages]` allocated lazily from
  `BootstrapAlloc`. Same lifetime as `cold_count_`.
- On `Slab::allocateNewSpan`: write `page_to_span_[idx..idx+pages]` to
  the span pointer.
- On `Slab::releaseSpan`: clear those entries.
- `pageToSpan(uintptr_t p)`: branch on `g_smash_vm_region->contains(p)`,
  if yes use direct index; if no fall back to `page_map_.get` for
  large-only-mode passthroughs and external mappings.

Memory cost: 8 B × kVmRegionSize / 4 KiB ≈ 32 MiB at 16 GiB region.
That's significant. Could be downsized: the actual committed region is
much smaller in practice, so allocate a smaller direct map and grow as
the region grows. Or use 4 B span-id (24-bit pool) instead of 8 B span
pointer.

**Likely win: 10–20 µs/txn**. Pairs well with #2 — the MRU cache hits
the truly-hot 95 % path; the direct map handles the cold-path miss
faster than the radix.

### 5. Hot-size short-circuit in `sizeToClass`

A handful of palloc sizes (16, 32, 64, 128, 256, 512, 1024, 2048) cover
the bulk of pgbench's allocation traffic. Adding a small switch at the
top of `sizeToClass` for these (which most compilers will turn into a
jump table) sometimes outperforms a generic table-lookup.

Verify with codegen first (#1) — it may already be optimal.

**Likely win: 2–5 µs/txn**. Fallback: low-priority.

### 6. Compressor skip-tick when nothing to do

Each backend has a coordinator thread that wakes every
`kCompressIntervalMs = 1000` ms and walks the `live_chunks_` bitmap.
If the bitmap hasn't changed since last tick AND
`worker_pages_eligible_` was 0 last tick, the coordinator can sleep
another tick without waking workers.

This is a *global* optimization that helps all workloads with idle
backends, not just pgbench. Across 5–7 concurrent postgres backends
this saves 5–7 wakeups/sec on the host = ~50–350 µs of context-switch
overhead per second.

**Likely win: 5–10 µs/txn**. Cheap and self-contained.

### 7. Defer coordinator spawn until first allocation

Currently the coordinator spawns at compressor `init()` (priority 201
constructor). Backends that exit before issuing any user allocation
still pay the spawn cost — pgbench is fast enough that this matters.

Move the spawn to lazy-on-first-allocation, behind a `std::atomic_flag`.

**Likely win: 5–10 µs/txn for workloads with many short-lived
processes**. Probably less for pgbench specifically (backends live
30 s).

### 8. Inline `callsiteArena` MRU cache

The `(ra, sh, sc) → arena_id` hash is computed per-malloc. For most
hot loops the call site is stable. A 1-entry per-thread cache keyed on
`ra` is a couple of branches but skips the multiplication.

```cpp
thread_local uintptr_t last_ra = 0;
thread_local uint8_t  last_arena = 0;

uintptr_t ra = ...;
if (ra == last_ra) return last_arena;
uint8_t a = computeArena(ra, sh, sc);
last_ra = ra; last_arena = a;
return a;
```

**Likely win: 5–10 µs/txn**.

## Verification at every step

1. **15/15 ctest pass** — non-negotiable.
2. **`run_quick_ci`** — `bench_rss` ≥ 30 %, `bench_sqlite` ≥ 5 %.
3. **Cool-tail ratio ≥ 95 %** — currently 99.3 %.
4. **Perf 5×5 with `--mode=compare`** — record the tps_med delta vs
   master.
5. **Interleaved A/B with compression off** (`SMASH_COLD_TICKS=9999` set
   inside `stage_run` since the script strips `SMASH_*`) — confirm any
   improvements affect both equally; if a fix only helps the
   compression-on number, that's a red flag (we may have just made
   the compressor accidentally faster, which is interesting but not
   what we're chasing on this branch).

## Out of scope

- Changing the public API of smash.
- Touching the compressor's correctness / scheduling logic
  (PR #13 just landed; further changes there should be a separate
  branch).
- Changing alloc8 itself unless absolutely necessary — keep the layering
  intact.
- Any new compression mechanism (this branch is purely about the
  allocator fast path).

## Suggested sequence for the branch

1. **Codegen audit** (#1) — produces a paragraph documenting where
   the time goes; informs the rest.
2. **Per-thread span MRU for free** (#2) — cleanest standalone win.
3. **`kThreadCacheMaxPerClass` bump** (#3) — single-line A/B.
4. **Direct page-to-span table** (#4) — bigger refactor, plumb
   carefully; pair with the MRU.
5. **Compressor skip-tick** (#6) — orthogonal cleanup.
6. Re-measure 5×5 against `shim+jemalloc`. If we hit ≤10 %, declare
   victory and write up. Otherwise iterate on (#5, #7, #8).

Total expected win is additive only in the limit — these candidates
overlap. Realistically the first three should net ~50–80 µs/txn (over
half the gap); the rest squeezes the last 20 %.
