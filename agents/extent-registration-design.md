# Extent-based external-mapping registration (design + prototype results)

## Problem

`SMASH_TRACK_EXTERNAL=1` registers application-direct `mmap` regions with the
compressor by inserting **every page** into an open-addressing hash
(`VmRegion::trackExternalPage`). Two lurking scalability limits:

1. **Registration is O(pages)** per mapping, done synchronously inside the
   caller's `mmap()`. A single large arena (a DB buffer pool, a JVM/GC heap)
   is one giant `mmap`, so a multi-GiB mapping means hundreds of thousands of
   hash inserts on the mmap hot path. (PR #53 stopped the O(n²) pathology but
   at the cost of *skipping* oversized mappings entirely — no coverage.)
2. **Coverage is hard-capped** at `kTrackMaxPages = 128K` slots = **512 MiB**
   at 4 KiB pages. A buffer pool larger than that is partly or wholly
   untracked → not compressed.

This matters: with the pool *under* the cap, smash compresses MariaDB's
InnoDB buffer pool for a **~39% RSS reduction** (measured, 512 MiB pool,
1.5M rows). Above the cap that win is lost purely to the registry structure,
not to any workload property.

## Design: extents, not pages

One record per mapping instead of one per page:

```
struct Extent { uintptr_t base; size_t first_index; size_t npages; bool live; };
```

External pages still get a **stable global page index** in
`[contig_pages_, contig_pages_ + N)` — the invariant the compressor relies on
(it iterates `committedPages()` indices and calls `pageAddress(idx)` /
`states_->get(idx)`). An extent owns a *contiguous block* of indices assigned
by one `index_next += npages` bump (O(1)). Lookups become:

- `pageIndex(addr)`: binary-search extents by base → `first_index + (addr-base)>>shift`.
- `pageAddress(idx)`: binary-search extents by first_index → `base + (idx-first_index)<<shift`.

Registration is O(1) in page count (O(E) sorted-insert, E = live mappings).

## Prototype (`bench/bench_extent_registration.cpp`)

Faithful reimpl of the current hash path vs. an extent registry vs. a hybrid,
on a realistic workload (one large arena + 2000 small mmaps). 4 KiB pages.

### Results (2 GiB arena + 2000 small mmaps)

| Metric        | Hash (current)      | Extent-only | **Hybrid** |
|---------------|---------------------|-------------|------------|
| Registration  | 0.57 ms (CAPPED)    | 0.07 ms     | **0.33 ms** |
| addr→idx lookup | 8.2 ns            | 14.0 ns     | **2.5 ns** |
| Coverage      | 131K pages (capped) | 589K (all)  | **589K (all)** |

Registration is **O(1) in arena size** for extents: 512→2048 MiB arena leaves
extent registration flat (~0.06 ms) while it tracks 4× the pages; the hash
stays capped at 131K pages and drops the rest.

With **few large extents** (n_small=0 — the pure DB-arena case), extent lookup
is **1.4 ns vs the hash's 9.3 ns**: a big arena floods the hash and lengthens
its probe chains, whereas a 1-extent binary search + shift is nearly free.

Correctness: 0 round-trip failures; extent tracks every page the hash tracked
(and the ~458K the hash dropped).

### Why HYBRID, not extent-only

Extent lookup degrades with E (2000 small extents → 14 ns). Small mappings are
numerous and better served by the hash; large mappings are few and better
served by extents. Route by a page-count threshold
(`kExtentThresholdPages = 256` = 1 MiB, matching `kLargeAllocVmThreshold`):
big → extent, small → hash. Lookup probes the tiny extent list first, then the
hash. Best of both: O(1) big-arena registration + full coverage + **2.5 ns
lookup (faster than today's 8.2 ns)** because the arena no longer pollutes the
hash.

## Lifting into `vm_region.h` (production plan — NOT yet done)

Touch points that must learn about extents (all currently hash-only):
`lookupIdx`, `trackExternalPage` (→ add `trackExternalRange`), `pageIndex`,
`pageAddress`, `contains`, `untrackExternalPage`, `untrackExternalPageLocked`,
and the caller `registerLinuxExternalRange` / `deregisterLinuxExternalRange`
in `linux_syscall_wrappers.cpp` (+ the macOS mmap/mach path in `smash_heap.cpp`).

Concurrency / correctness notes:
- The extent list is written on mmap/munmap (rare) and read on every tick +
  fault. A seqlock or RCU-style swap keeps readers lock-free; or reuse the
  existing per-page lock discipline since extents don't overlap.
- **Formal model**: `model/SmashExternalRace.{lean,tla}` proves the munmap-vs-
  compressor-snapshot race is safe under the *per-page hash tombstone* scheme.
  Extents change the untrack primitive (mark-extent-dead vs. tombstone-slot),
  so the model must be updated and re-checked before shipping — this is the
  main reason the lift is a separate, careful PR rather than folded in here.
- **Partial munmap** of a large arena would split an extent (prototype marks
  the whole extent dead; production needs split-on-partial-unmap or a
  fall-through to per-page tombstones for the unmapped sub-range).

Net: the prototype proves the algorithm (O(1) registration, full coverage,
faster lookup). The production change is mechanical but must carry the formal
model with it.
