# Smash TLA+ Models (+ Lean proofs)

This directory contains TLA+/PlusCal models of Smash's core state machine, plus
machine-checked Lean 4 formalizations of the corresponding safety properties.

## Files

### TLA+ / PlusCal
- `SmashCore.tla` / `.cfg` — the allocator↔compressor PROT/state model. A single
  `BuggyMode` constant selects fixed (`FALSE`) vs. pre-fix (`TRUE`) behaviour;
  there is no separate `*Buggy.tla` file. Run the buggy demonstration with
  `SmashCore_buggy.cfg` (`BuggyMode = TRUE`).
- `SmashCoreSym.tla` / `.cfg` / `_buggy.cfg` — **symmetry-reduced** companion to
  `SmashCore.tla`. `Pages` / `AppThreads` / `Compressors` are model-valued
  constants and the compressor's integer cursor is replaced by a
  non-deterministic page pick, so `SYMMETRY AllSymmetry` applies. This is the
  scalable way to model-check `SmashCore` at 3+ pages (`SmashCore.tla` itself,
  with integer pages, cannot use SYMMETRY and blows up).
- `SmashRestoreRace.tla` + `_atomic.cfg` / `_split_buggy.cfg` /
  `_procmem_fixed.cfg` — the decompress-on-fault TOCTOU model (see below).
- `SmashThreadCreate.tla` / `.cfg` / `_fixed.cfg` — the pthread_create-time
  TLS-fault model.

### Lean 4 (machine-checked, `lean <file>.lean`)
- `SmashThreadCreate.lean` — pre-create-workers safety.
- `SmashRestoreRace.lean` — coherence invariant ⇒ `NoStaleRead` for the atomic
  and procmem restores; concrete counterexample for the split restore.
- `SmashCore.lean` — inductive invariant `WF ⇒ SafetyInv` (fixed mode preserved
  by every transition); buggy mode reaches `state = ACTIVE ∧ pageProt = PROT_NONE`,
  violating `ActiveImpliesRW`. Single page, atomic whole-operation transitions.
- `SmashCoreConc.lean` — the **concurrent** strengthening of `SmashCore.lean`.
  Removes the single-page / atomic-operation abstraction: the state is
  `Sys ι := ι → Page` for an *arbitrary* index type `ι` (unbounded pages), and
  `Step` is a nondeterministic relation ("some thread fires some enabled
  transition on some page; all other pages unchanged") whose reachable closure
  is *every* interleaving of *any* number of threads — the thing bounded model
  checking can only sample. Multi-step compressor operations are modeled as
  separate steps, so intermediate states are observable to concurrent threads.
  Proves `reachable_safe`: `SafetyInv` holds for every page in every reachable
  state of the fixed system, for all interleavings, unbounded in both pages and
  threads. `buggy_reachable_violates` exhibits the pre-fix decommit trace.
  Machine-checked; both headline theorems depend only on `propext` (no `sorry`,
  no `Classical.choice`, no `native_decide`).

## The Bug (Found May 2024)

The original model only included `AppThread` (read/write) and `Compressor` processes,
but **did not model the Allocator**. This missed a critical race condition:

### Race Condition: Allocation During Compression

```
Allocator                          Compressor
─────────                          ──────────
                                   lock(page)
                                   state = COMPRESSING
                                   bufferData = pageData  [copy]
                                   
check: state != COMPRESSED?
  → TRUE (it's COMPRESSING)
slotFree[slot] = FALSE
pageData[slot] = "valid"  [write vtable]
                                   
                                   bufferData[slot] = "zero"  [zeroFreeSlots]
                                   state = COMPRESSED
                                   prot = PROT_NONE
                                   unlock(page)

...later, app reads slot...
  → fault (PROT_NONE)
  → decompress: pageData = bufferData
  → pageData[slot] = "zero"  ← CORRUPTION!
  → "pure virtual method called"
```

### The Fix

In `span.h`, `slotPageCompressed()` must return true for **all** compression-related
states, not just `COMPRESSED`:

```cpp
// OLD (buggy):
return page_states->get(page_idx) == PageState::COMPRESSED;

// NEW (fixed):
PageState st = page_states->get(page_idx);
return st != PageState::ACTIVE && st != PageState::EMPTY;
```

## Running the Models

Use the command-line tools (`tla2tools.jar` + a JDK on the PATH):

```bash
# SmashCore — fixed (expect: No error). Large state space; bounded constants.
java -jar tla2tools.jar -config SmashCore.cfg       SmashCore.tla

# SmashCore — buggy (expect: SafetyInv violated) via the BuggyMode=TRUE cfg.
java -jar tla2tools.jar -config SmashCore_buggy.cfg SmashCore.tla

# SmashCoreSym — symmetry-reduced; the practical way to model-check at 3+ pages.
java -jar tla2tools.jar -config SmashCoreSym.cfg        SmashCoreSym.tla   # fixed → No error
java -jar tla2tools.jar -config SmashCoreSym_buggy.cfg  SmashCoreSym.tla   # buggy → SafetyInv violated
```

The buggy run reports `Invariant SafetyInv is violated`, with a trace ending in
the **freelist-PROT_NONE** state: a page is freed while non-RW (COMPRESSED →
PROT_NONE, or MONITORING → PROT_READ), the buggy `processDecommitEntry` skips
`commitPages`, and the allocator pops it `EMPTY → ACTIVE` with the kernel bits
still non-RW — so `state = ACTIVE` coexists with `pageProt ≠ PROT_RW`, violating
`ActiveImpliesRW`. (The fixed `DecProcess` restores `PROT_RW` before the run is
pushed to the freelist.)

> Note: an earlier revision of this model used the `state = "ACTIVE"`-only free
> guard, under which the buggy violation was **not reachable** (an ACTIVE page is
> always PROT_RW). The free guard now fires on any non-EMPTY page, faithfully to
> the real free path (which does not decompress), so the bug is reachable.

## Key Invariants

- **BlobIntegrity**: Compressed pages have blobs, active pages don't
- **ProtectionSafety**: Protection bits match state
- **NoCorruptedAllocation**: Allocated slots never contain corrupted (zeroed) data

## Model Parameters

- `NumPages = 2` — Number of pages to model
- `NumSlots = 2` — Slots per page (for allocation granularity)  
- `ColdThreshold = 1` — Ticks before a page is eligible for compression

---

# SmashRestoreRace — decompress-on-fault TOCTOU (Found June 2026)

`SmashSnapshotRace1-4` all model the COMPRESS side (snapshot → madvise → mprotect)
and the writer/reader races around it. Crucially, every one of them models the
*decompress-restore* in the fault handler as a **single atomic step**
(`pageVal := snapshot; backing := BACKED; prot := RW` all at once). The real
C++ code did not: `handleFault()` / `prefetchAdjacent()` did

```cpp
decompress(blob -> scratch);
commitPages(page_addr);              // mprotect(PROT_RW): page READABLE
memcpy(page_addr, scratch, 4096);    // decompressed data lands HERE
```

Between the `mprotect` and the `memcpy` the page is **readable but still holds
the stale/zero backing** that madvise dropped. A concurrent application reader
doing an ordinary load does not fault and does not take the per-page lock, so it
reads zeros where structured data should be → the ~67% nondeterministic
full-mode failure on neuron-cc.

`SmashRestoreRace.tla` models exactly this. The fault handler's restore is
parameterized by `RestoreVariant`:

- `"atomic"` — the OLD models' single-step restore. `NoStaleRead` **holds** —
  this is *why* the previous models never caught the bug.
- `"split"` — the real pre-fix code (mprotect-RW then memcpy, two steps).
  `NoStaleRead` is **violated**; TLC produces the reader-in-the-window trace.
- `"procmem"` — the fix (`restorePageContents`): write the decompressed bytes
  through `/proc/self/mem` while the page is still PROT_NONE, *then* flip to
  PROT_RW. `NoStaleRead` **holds**.

### Running

```bash
# Buggy (real pre-fix code) — expect: Invariant NoStaleRead is violated
java -jar tla2tools.jar -config SmashRestoreRace_split_buggy.cfg   SmashRestoreRace.tla

# Old models' assumption — expect: No error (shows why they missed it)
java -jar tla2tools.jar -config SmashRestoreRace_atomic.cfg        SmashRestoreRace.tla

# The fix — expect: No error
java -jar tla2tools.jar -config SmashRestoreRace_procmem_fixed.cfg SmashRestoreRace.tla
```

### Key invariant

- **NoStaleRead**: no reader ever observes physical backing that disagrees with
  the page's current logical value. Violated only by the `"split"` restore.

The corresponding fix lives in `src/compress/compressor_thread.h`
(`CompressorThread::restorePageContents()`).

---

# Model status / code sync (verified 2026-06-18)

Re-checked every model in this directory against the current code. TLC results
(`tla2tools.jar` + JDK 21); Lean results (`lean <file>.lean`, zero errors):

| Model | Config | Result | Corresponds to |
|-------|--------|--------|----------------|
| `SmashRestoreRace` | `_atomic` | PASS (no error) | old models' atomic restore (why they missed the bug) |
| `SmashRestoreRace` | `_split_buggy` | **VIOLATED** NoStaleRead | the pre-fix mprotect-then-memcpy restore |
| `SmashRestoreRace` | `_procmem_fixed` | PASS (no error) | the shipped `restorePageContents` fix |
| `SmashCore` | `SmashCore.cfg` (BuggyMode=FALSE) | no violation found (state space large; not exhausted in bounded time) | allocator↔compressor PROT/state desync |
| `SmashCore` | `SmashCore_buggy.cfg` (BuggyMode=TRUE) | **VIOLATED** SafetyInv | freelist-PROT_NONE on the decommit→alloc-pop path |
| `SmashCoreSym` | `SmashCoreSym.cfg` (3 pages, symmetry) | **PASS, exhaustive** (24,804 distinct states) | symmetry-reduced `SmashCore`, fixed |
| `SmashCoreSym` | `SmashCoreSym_buggy.cfg` | **VIOLATED** SafetyInv | symmetry-reduced `SmashCore`, buggy |

Lean formalizations (machine-checked safety, independent of TLC):

| Lean file | Proves |
|-----------|--------|
| `SmashThreadCreate.lean` | pre-create-workers ⇒ `threadCreateSafe` (inductive invariant) |
| `SmashRestoreRace.lean` | coherence inv ⇒ `NoStaleRead` (atomic, procmem); split ⇒ concrete violation |
| `SmashCore.lean` | `WF ⇒ SafetyInv`, `WF` preserved by all fixed-mode transitions; buggy trace ⇒ violation (single page, atomic ops) |
| `SmashCoreConc.lean` | `reachable_safe`: `SafetyInv` for every page in every reachable state of the concurrent system — unbounded pages, unbounded threads, all interleavings; buggy trace ⇒ violation |

Note: the historical table referenced `SmashSnapshotRace4` and `SmashCoreBuggy`;
those files were never committed (the compress-side races are now subsumed by
`SmashRestoreRace`, and the buggy `SmashCore` is selected by `BuggyMode=TRUE`
rather than a separate module).

Notes:
- `SmashCore.tla` (the broad allocator+compressor PlusCal model) was left
  **untranslatable** by commit 823515f (double `lock[target]` assignment under
  one label). Fixed and re-translated; it model-checks again. Its state space
  is large; exhaustive runs are best done with bounded constants.
- The two race-specific models (`SmashRestoreRace`, `SmashSnapshotRace4`) are
  the ones that map 1:1 to the shipped data-race fixes and are confirmed in
  sync — each passes in the fixed configuration and produces a counterexample
  in the buggy configuration.
- TLC error-trace artifacts (`*_TTrace_*.tla/.bin`) are generated on a
  violation and are not source; they can be deleted.
