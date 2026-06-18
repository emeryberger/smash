# Analysis Mode — design (ON HOLD)

> **Status: design-only, not implemented.** This documents a proposed "analysis mode" for
> smash: a read-only profiler that reuses smash's hot/cold page machinery to surface
> **object-splitting** optimization opportunities (which *byte offsets within an object* are
> hot vs cold), attributed back to allocation call sites.
>
> **Why it's on hold:** Valgrind **DHAT** already produces the core signal — per-allocation-site,
> per-byte-offset read/write histograms with cold-region detection — with zero new code. See
> "Relationship to DHAT" below. This mode is only worth building if DHAT proves insufficient for
> a specific need: near-native runtime on a **full-scale** workload that Valgrind is too slow for
> (e.g. the 600 s × 192-thread neuron-cc walrus backend), always-on availability via the shipped
> `LD_PRELOAD`, or visibility into direct-`mmap` arenas. It is *coarser* than DHAT (sampled, not
> exact counts), so it is a production fast-path, not a replacement for DHAT's precision.

## Goal

Instead of compressing cold pages, **observe** access patterns non-destructively and report:
**object-split candidates** — per (allocation call site, size class), an intra-object **offset
heat histogram** showing a hot prefix vs a cold tail, so a developer can split hot fields from
cold fields (cache-conscious structure layout). A secondary, demoted signal flags
allocated-but-never-read regions.

Dropped during design: a "phase-gap / recompute-later" list (deemed not actionable) and a
page-granular "spuriously-hot" heuristic (superseded by the byte-offset histogram below).

## Core mechanism — faulting-address sampling

The page-monitoring path already generates a fault on first access to a monitored page, and the
fault handler already has the exact faulting byte:

- `FaultHandler::signalHandler` captures `addr = info->si_addr` and the write-bit
  (`src/vm/fault_handler.h:149`, REG_ERR decode at `:166-174`), then calls
  `callback_(addr, ctx)` → `handleFault`, which maps `addr` to its page/Span to restore it.

In analysis mode, in that same callback (the Span lookup is already done for the restore):

```
slot_idx = (addr - span.base) / span.object_size
offset   = (addr - span.base) % span.object_size       // intra-object byte offset
bucket   = (span.arena_id, span.size_class)             // or (call site) where available
record sample (bucket, offset, was_write) into a per-thread lock-free ring buffer
```

The compressor thread drains the ring each tick into a per-bucket **offset histogram**
(`hits[nbins]`, optional `writes[nbins]`). Small objects bin per ~8 B (per byte if ≤64 B); large
objects bin into ~256 proportional bins to bound width. The ring is fixed-size and malloc-free
(async-signal-safe); this is the only hot-path addition and it only fires on a monitored-page
fault, which is already a slow path. Steady-state malloc/free are untouched.

### Sampling rate (configurable, default periodic)

The histogram is only as rich as how often monitored pages re-arm to `PROT_READ`:
- **Default**: re-arm on the existing Phase-3 cadence (~1 s) → ~one offset sample per object per
  interval. Near-native runtime; gives a robust hot/cold *partition*, weak access *frequency*.
- `SMASH_ANALYZE_REARM_MS=<n>`: lower it to re-arm aggressively (e.g. right after each served
  fault) for a denser histogram and higher confidence, at the cost of more faults (slower —
  acceptable offline).
The report prints the effective rate and total `fault_samples` so low-confidence rows are visible.

## Mode gate (non-destructive)

- `include/smash/config.h`: extend `enum class SmashMode { Full, CompressOnly, Analyze }`
  (currently Full/CompressOnly at `:593`); parse `SMASH_MODE=analyze` in `getSmashMode()`
  (`:602-613`) plus an `SMASH_ANALYZE=1` alias; add `[[gnu::hot]] inline bool isAnalyzeMode()`
  using the same hand-rolled `atomic<int>` lazy-init as `isLargeOnlyMode()` (`:629-639`).
- `src/compress/compressor_thread.h` `tick()` (~`:3318-3366`): when `isAnalyzeMode()`, **skip**
  the destructive dispatches — Phase 2 compress (`:3323`) and Phase B reclaim (`:3320`). **Keep**
  Phase 1 access tracking (`:3318`) and Phase 3 `PROT_READ` monitoring (`:3364`) — the data
  source. Add a lightweight pass that drains the sample ring and de-escalates any page left
  `PROT_NONE` back to `PROT_READ` (so no page is stuck unreachable; the fault handler already
  restores `PROT_RW` + sets `accessed_`). Result: app runs near-native, nothing is ever
  compressed or reclaimed.

## Call-site attribution

`callsiteArena()` / `callsiteArenaForLarge()` (`src/smash_heap.h:431-560`) currently consume the
return address and keep only an 8-bit arena id. To attribute findings to source:

- Add `uintptr_t alloc_ra` to `Span` (`src/core/span.h:19-62`) — one pointer, bootstrap memory,
  one store at span creation, zero per-object hot-path cost.
- Plumb the existing `caller_ra` / `ra_for_arena` (already in hand at `smash_heap.h:1016,1031,
  1058,1281`) into `Slab::allocateNewSpan` (slab spans) and `LargeAlloc::allocate`
  (`src/core/large_alloc.h:42,86`).
- Resolve RA → `dso!symbol+offset` **only at report time** via `dladdr`, recording
  `dso + offset-within-dso` for ASLR stability (exactly as `stableCallsiteHashSlow`,
  `smash_heap.h:407-417`).
- **Attribution accuracy**: exact for large allocations (1 span = 1 alloc); for slab spans it is
  the *creating* call site (objects from other sites may share the span) — a documented
  approximation, flagged per row as `span_representative` vs `exact_large`.

## Report (exit + SIGUSR2; JSON/CSV)

Extend `sigusr1Handler` (`compressor_thread.h:4122-4167`, already branches to `dumpBucketStats`)
with an `isAnalyzeMode()` → `dumpAnalysisReport()` branch; same at the atexit/`stop()` path
(`~:4071`). Async-signal-safety: the SIGUSR2 path emits a `write()` summary only; the full
JSON/CSV (with the per-offset histograms) is written from the compressor thread on the next tick
(flag-driven). `SMASH_ANALYZE_OUT=<path>` (default stderr), `SMASH_ANALYZE_FORMAT=json|csv`.
Headline = ranked **object-split candidates** by `cold_byte_fraction × bytes_allocated`
(reclaimable bytes if the cold tail is split out); each row carries its offset histogram and
resolved call site. Secondary = a never-read footnote list. Prefix `[smash analyze]`.

### Coverage preamble (maximize-visibility default)

When `isAnalyzeMode()` and no explicit mode is set, default to **full mode + lowered threshold +
`SMASH_TRACK_EXTERNAL=1`** (safe — nothing is compressed/reclaimed). The report header prints
process RSS (`/proc/self/statm`) vs smash-tracked bytes (`committedPages × kPageSize`) → a
coverage %, plus active flags, so findings are not over-read on uncovered memory. (This matters:
in `SMASH_LARGE_ONLY=1` smash sees only `malloc(>threshold)` and only `≥1 MB` is trackable — on
neuron-cc that was ~⅓ of RSS.)

## Example report (illustrative shapes — real reports show only measured data)

Console (`[smash analyze]`, on SIGUSR2 or exit):

```
[smash analyze] ===== Smash Analysis Report (run: walrus_driver, 612s) =====
[smash analyze] COVERAGE: tracked 28.9 GB of 30.4 GB process RSS (95%) | samples: 4.1M faults
[smash analyze]   mode=analyze  large_only=0  threshold=4KB  track_external=1  rearm=1000ms
[smash analyze]
[smash analyze] --- TOP OBJECT-SPLIT CANDIDATES (hot prefix + cold tail within object) ---
[smash analyze]  rank  obj_size  cold_tail   reclaimable  site
[smash analyze]   1     88 B      56 B (64%)  3.1 GB       libwalrus.so!Instruction (alloc @ Instruction::create+0x40)
[smash analyze]         offset heat (8B bins, '#'=hot .=cold):  ##### . . . . .
[smash analyze]         0..31 HOT (opcode,flags,next*) | 32..87 COLD (debug_loc, sched_meta)
[smash analyze]   2     64 B      24 B (38%)  0.4 GB       libwalrus.so!LiveRange (alloc @ Coloring::newRange+0x18)
[smash analyze]         offset heat:  ##### . . .   (cold tail = 2 rarely-read u64 stats fields)
[smash analyze]
[smash analyze] --- secondary: never-read-after-alloc (footnote) ---
[smash analyze]   1.9 GB across 3 sites; largest: ColoringAllocator::scratch+0x1a4 (1.1 GB)
[smash analyze] ============================================================
```

JSON row (the histogram IS the deliverable — a human/tool reads the hot→cold boundary off it):

```json
{"rank": 1, "dso": "libwalrus.so", "symbol": "Instruction",
 "alloc_site": "Instruction::create+0x40", "object_size": 88, "bin_width": 8,
 "attribution": "span_representative", "samples": 612440,
 "offset_hits":   [180322, 174553, 169201, 90114, 61003, 9, 2, 0, 1, 0, 0],
 "offset_writes": [120010,  90221,  44102, 12000,  3001, 0, 0, 0, 0, 0, 0],
 "hot_prefix_bytes": 32, "cold_tail_bytes": 56, "cold_tail_fraction": 0.64,
 "bytes_allocated": 4831838208, "reclaimable_if_split_bytes": 3092376453}
```

## Honest limits (must be carried in the report, not buried)

- **Sampling, not a trace**: PROT_READ gives *first-touch per re-arm interval*, not exact access
  frequency. The hot/cold *partition* of offsets is robust; precise per-offset counts are not.
- **Offset ≠ named field**: offsets are byte positions in the allocator slot. Mapping to a
  source-level struct field needs a DWARF post-pass (`addr2line`-style on the offsets) — out of
  scope for v1. For variable-size/array allocations, "offset" is a byte position, not a field.
- **Coverage-bound**: only profiles memory smash actually sees (see coverage preamble).
- **Attribution**: exact for large, `span_representative` for slab.
- `samples` shown per row so low-confidence rows are visible.

## Relationship to DHAT (why this is on hold)

Valgrind DHAT already records, per allocation point (allocation stack), a **per-byte-offset
read/write histogram** within the block, prints unaccessed offsets specially, and flags cold
blocks via read/write ratios ("Block layout can often be inferred from counts"). That is exactly
the object-splitting signal, with **exact** counts (not sampled), aggregated by call stack, zero
new code. DHAT's limits: ~20–50× slowdown and thread serialization (can't run a full-scale
600 s × 192-thread compile — use a reduced repro), and per-offset histogramming capped at 1024 B
by default (raise via `DHAT_HISTOGRAM_MEMORY` to ~25 KB) — but the small structs that are split
candidates are well under the cap. **Use DHAT first.** Build this mode only for the full-scale /
always-on / mmap-arena cases DHAT can't cover.

## Literature

- Hot/cold structure splitting — Chilimbi, Davidson, Larus, *Cache-Conscious Structure
  Definition* (PLDI'99). The offset histogram is the field-heat input their splitting transform
  needs.
- Pool/region allocation by call site — Lattner & Adve, *Automatic Pool Allocation* (PLDI'05).
  smash already pools by call site (`callsiteArena`); analysis reports which pools have cold tails.
- Stack-hash allocation provenance — LLAMA, Maas et al. (ASPLOS'20). Basis of `callsiteArena`;
  extended here from routing-only to retained-and-reported.
- Address-sampling access profilers — DProf, Memoro, MemProf. Same idea (object+offset
  attribution from sampled accesses); here the "sampler" is the existing PROT_READ fault.
- Cold-data identification — Google *Software-Defined Far Memory* (ISCA'19). Same idle-bit / page
  age machinery; the secondary never-read flag.
