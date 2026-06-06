# Smash TLA+ Model

This directory contains TLA+/PlusCal models of Smash's core state machine.

## Files

- `SmashCore.tla` / `.cfg` — **Fixed version** with correct allocator check
- `SmashCoreBuggy.tla` / `.cfg` — **Buggy version** that demonstrates the race

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

Install TLA+ Toolbox or use the command-line tools:

```bash
# Run the FIXED model (should pass all invariants)
java -jar tla2tools.jar -config SmashCore.cfg SmashCore.tla

# Run the BUGGY model (should find invariant violation)
java -jar tla2tools.jar -config SmashCoreBuggy.cfg SmashCoreBuggy.tla
```

The buggy model will report a violation of `NoCorruptedAllocation`:
```
Invariant NoCorruptedAllocation is violated.
```

With an error trace showing:
1. Compressor enters `COMPRESSING`, copies page to buffer
2. Allocator sees `state != COMPRESSED`, allocates slot, writes data
3. Compressor zeros the (now-allocated) slot in buffer
4. Compressor finishes, page becomes `COMPRESSED`
5. App faults, decompression overwrites allocated slot with zeros

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

# Model status / code sync (verified 2026-06-06)

Re-checked all hand-written models against the current code after the
session's fixes (TOCTOU `restorePageContents`, P2_CHUNK default-on, the
deferred-madvise TTL fix). Results with the bundled TLC (`tla2tools.jar` +
`jdk-21/`):

| Model | Config | Result | Corresponds to |
|-------|--------|--------|----------------|
| `SmashRestoreRace` | `_atomic` | PASS (no error) | old models' atomic restore (why they missed the bug) |
| `SmashRestoreRace` | `_split_buggy` | **VIOLATED** NoStaleRead | the pre-fix mprotect-then-memcpy restore |
| `SmashRestoreRace` | `_procmem_fixed` | PASS (no error) | the shipped `restorePageContents` fix |
| `SmashSnapshotRace4` | `_fixAv` | PASS | compress-side PROT_NONE→madvise ordering (FixAv) |
| `SmashSnapshotRace4` | `_buggy` | **VIOLATED** SafetyInv | pre-fix compress-side store-loss / reader-zero |
| `SmashCore` | `SmashCore.cfg` (BuggyMode=FALSE) | runs; no violation over tens of millions of states | allocator↔compressor PROT/state desync |

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
