# Clang Thread Safety Analysis

Smash's locking is statically checked with
[clang -Wthread-safety](https://clang.llvm.org/docs/ThreadSafetyAnalysis.html).
The lock primitives carry capability attributes (`src/util/thread_safety.h`,
no-ops under gcc), so clang proves at compile time that shared state guarded by
a lock is only touched while that lock is held, and that lock/unlock pairs
balance on every path.

## Running

```bash
# inside the build container (clang 21):
bash tools/ts_check_all.sh    # runs -Wthread-safety on every compiled TU
```

It reuses the exact flags from `build_bench2/compile_commands.json`, swapping
`g++ -c` for `clang++ -fsyntax-only -Wthread-safety`.

## Current result

**0 thread-safety warnings** across the compiled translation units
(`smash_heap.cpp`, `linux_syscall_wrappers.cpp`).

## What is checked

- `Spinlock` is a `CAPABILITY("mutex")`; `lock/unlock/tryLock` are
  `ACQUIRE/RELEASE/TRY_ACQUIRE`; `LockGuard` is a `SCOPED_CAPABILITY` (RAII).
- `Slab::lockSlab/unlockSlab`, `LargeAlloc::lockAlloc/unlockAlloc` —
  `ACQUIRE/RELEASE` (manual hand-off locks).
- `CompressStore::bumpAlloc(Shard&, …)` is `REQUIRES(shard.lock)`: clang
  verifies both callers (`store`, `release`) hold the shard lock — this
  enforces the "caller must hold the shard lock" contract that was previously
  only a comment.

## Documented limitations (clang cannot model these)

- **Per-page locks** (`PageLockTable::lock(page_idx)`): an *array of locks
  indexed at runtime*. The analysis cannot distinguish `lock(5)` from
  `lock(6)`, so per-page lock discipline (the compressor↔fault-handler
  coordination) is **not** statically checkable. It is covered instead by the
  TLA+ models (`SmashRestoreRace`, `SmashSnapshotRace4`) and the multithreaded
  `test_contention` runtime test.
- **`SmashHeap::lock/unlock`** acquire the whole slab array in a loop — the same
  "locks in a loop" limitation — so they are marked `NO_THREAD_SAFETY_ANALYSIS`.
  They are exact mirror images and short-circuit identically, so the pairing is
  balanced by construction.

Together with the TLA+ models and `test_contention`, this gives three
independent layers of assurance on the concurrency: static (clang, for
fixed-identity locks), model-checked (TLA+, for the page state machine), and
runtime (the contention stress test).
