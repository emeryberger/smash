# Allocator Benchmark Results: neuron-cc test7_full

**Date:** 2026-06-01  
**Test:** `test7_fsdp_c474_b8_63l_135416_def_modular.hlo` (9.3 MB HLO)  
**Target:** trn2  
**Machine:** 192-core build instance  
**Build:** USE_TCMALLOC=OFF (libwalrus.so uses system allocator via PLT)

## Results

| Allocator | Wall Time | Peak RSS | Avg RSS | Notes |
|-----------|-----------|----------|---------|-------|
| glibc     | 372s      | 35,719 MB | 21,855 MB | System default |
| jemalloc  | 283s      | 18,766 MB | 11,726 MB | Lowest peak RSS |
| mimalloc  | 258s      | 21,003 MB | 13,651 MB | Fastest wall time |

## Analysis

- **glibc** has massive peak RSS (35.7 GB) — almost 2x jemalloc's peak
- **jemalloc** achieves 47% lower peak RSS than glibc (18.8 GB vs 35.7 GB)
- **mimalloc** is 31% faster than glibc (258s vs 372s) with 41% lower peak RSS

## Methodology

- RSS measured every 0.5s across all descendant processes (Python launcher + hlo2penguin + walrus workers)
- `LD_PRELOAD` used for jemalloc and mimalloc
- `PYTHONMALLOC=malloc` ensures Python uses the preloaded allocator
- All runs on same machine, sequential (no parallel interference)

## Allocator Versions

- **glibc:** system default (AL2023)
- **jemalloc:** 5.3.0 (built from source)
- **mimalloc:** 2.1.2 (built from source)

## Raw Data

Results directory: `smash/build/allocator_bench_20260601_193819/`

```
label,rc,wall_s,peak_mb,avg_mb
glibc,0,372,35719,21855
jemalloc,0,283,18766,11726
mimalloc,0,258,21003,13651
```

## Smash Results (LARGE_ONLY mode)

| Config | Wall Time | Peak RSS | Avg RSS | Notes |
|--------|-----------|----------|---------|-------|
| smash_profile | 516s | 31,160 MB | 20,555 MB | Profile generation run |
| smash_with_profile | 478s | 31,307 MB | 20,018 MB | Using saved profile |
| smash_aggressive | 470s | 32,804 MB | 21,149 MB | 1s cold timeout |

**Settings:** `SMASH_LARGE_ONLY=1`, `SMASH_COLD_TIMEOUT_SEC=5` (or 1 for aggressive)

### Smash Analysis

Smash LARGE_ONLY mode is **not competitive**:
- 69% slower than jemalloc (478s vs 283s)
- 67% higher peak RSS than jemalloc (31.3 GB vs 18.8 GB)
- Even worse than glibc on wall time (478s vs 372s)

**Root cause:** LARGE_ONLY mode only manages allocations > 16KB, missing the bulk of memory in the slab allocator. The overhead of smash's page tracking + compression doesn't pay off when most allocations bypass it.

## Smash Results (FULL mode)

**Date:** 2026-06-02

After fixing:
1. ~~tcmalloc in libwalrus.so~~ (USE_TCMALLOC=OFF)
2. ~~RTLD_DEEPBIND in JobRegistry.py~~ (disabled by default)
3. vm.max_map_count increased to 1,000,000

| Config | Wall Time | Peak RSS | Avg RSS | Notes |
|--------|-----------|----------|---------|-------|
| smash_full_48g_10s | ~468s | **17,780 MB** | 9,909 MB | Full mode, 48 GiB VM, 10s cold timeout |
| smash_profile_gen | ~573s | 17,381 MB | 10,492 MB | Profile generation (failed due to neuron-cc bug) |

**Settings:** `SMASH_VM_GIB=48`, `SMASH_COLD_TIMEOUT_SEC=10`, `SMASH_DEFER_MADVISE=1`

## Stability Test Results (2026-06-02)

**3 runs per allocator to test reliability:**

| Allocator | Run 1 | Run 2 | Run 3 | Pass Rate |
|-----------|-------|-------|-------|-----------|
| glibc | PASS (369s) | PASS (348s) | PASS (361s) | **3/3 (100%)** |
| jemalloc | PASS (284s) | PASS (275s) | PASS (287s) | **3/3 (100%)** |
| mimalloc | PASS (268s) | PASS (270s) | PASS (250s) | **3/3 (100%)** |
| tcmalloc | SEGFAULT | - | - | **0/1** |
| smash_full | PASS (696s) | FAIL | FAIL | **1/3 (33%)** |

**Key finding:** Baseline allocators (glibc, jemalloc, mimalloc) pass 100% of runs. Smash only passes 33%. The failures appear as neuron-cc internal errors but **only occur with smash**, suggesting smash causes memory corruption that manifests as compiler bugs.

**Smash failure errors:**
- Run 2: `custom_call.20_i65 is overlapping with must-pinned memloc DynamicDMAScratchLoc`
- Run 3: `post_sched failed` (22/24 modules passed)

**tcmalloc:** Crashes immediately on startup (SEGFAULT). Likely conflicts with neuron-cc's internal tcmalloc or other symbol issues.

### Conclusion

Smash full mode is **not production-ready** for neuron-cc. While it occasionally produces lower RSS than jemalloc, it has a 67% failure rate. Extensive binary-search debugging (2026-06-05) has narrowed down the issue:

### Full Mode Bug Investigation (2026-06-05)

**Test matrix (neuron-cc test7_full HLO):**

| Configuration | Pass Rate | Finding |
|--------------|-----------|---------|
| SMASH_LARGE_ONLY=1 | 3/3 (100%) | Large-only mode works |
| SMASH_NO_COMPRESSOR=1 | 3/3 (100%) | Allocation path is correct |
| SMASH_COLD_TICKS=9999 | 3/3 (100%) | No compression works |
| SMASH_NO_MONITOR=1 | 1/3 (33%) | Compression path has bug |
| SMASH_USE_LZ4=1 | 1/2 (50%) | Bug not zstd-specific |
| SMASH_DEFER_PHASES_MS=30000 | 1/2 (50%) | Timing doesn't help |
| SMASH_FIXAV=1 | 0/3 (0%) | Strictest ordering is WORSE |
| Default full mode | ~1/3 (33%) | Base failure rate |

**Root cause analysis:**
- Bug is in the compression/decompression path, NOT allocation
- Bug is NOT specific to zstd (LZ4 also fails)
- Bug is NOT fixed by strictest memory ordering (FIXAV=1)
- Bug is NOT timing-related (DEFER_PHASES doesn't help)

### ROOT CAUSE FOUND & FIXED (2026-06-05)

**The bug: a TOCTOU window in the decompress-on-fault restore path.**

`handleFault()` (and `prefetchAdjacent()`) restored a compressed page with this
sequence (`compressor_thread.h`):

```cpp
decompress(blob → scratch_buf);
commitPages(page_addr);                 // mprotect(PROT_RW): page is now READABLE
memcpy(page_addr, scratch_buf, 4096);   // decompressed data arrives HERE
```

Between the `mprotect` and the `memcpy`, the page is **readable but does not yet
contain the decompressed bytes**. The faulting thread holds the per-page lock,
but a *concurrent* application thread doing an ordinary load on the same page
does **not** fault (the page is now `PROT_RW`) and does **not** take the per-page
lock — so it reads whatever the physical backing currently holds: zeros (if the
backing was dropped via `madvise(DONTNEED)`) or stale bytes. That silently
corrupts the application's data, surfacing later as the assorted nondeterministic
internal errors above.

This single window explains **every** observation:

| Observation | Why |
|---|---|
| No-compression configs → 100% | No decompress, no window |
| LZ4 and zstd both fail | Bug is in the restore path, not the codec |
| **FIXAV → 0/3 (worse)** | FIXAV `madvise`s backing immediately on compress, so the window *always* exposes a zero-fill page |
| Default deferMadvise → ~33% | Backing kept ~500 ms, so the window exposes correct data for recently-compressed pages and zeros only for swept ones |
| **NO_DECOMMIT → OOM, not corruption** | Backing never dropped → window exposes correct original data; RSS just explodes |
| Fails in walrus `mod_parallel_pass` | Needs concurrent threads sharing a page — exactly multithreaded BIR passes |

**The fix (`restorePageContents()`):** populate the page's physical backing
*before* it becomes readable. On Linux we `pwrite()` the decompressed bytes
through `/proc/self/mem` while the page is still `PROT_NONE` (the kernel's
mem-file access uses `FOLL_FORCE`, honoring the VMA's `VM_MAYWRITE` rather than
the PTE protection, so the store lands and faults in fresh backing), then flip
to `PROT_RW`. Concurrent readers keep faulting on `PROT_NONE` and block on the
per-page lock until the data is in place. Non-Linux falls back to the legacy
commit-then-copy. Verified safe with a standalone repro; all 16 smash unit
tests pass.

**Post-fix verification (2026-06-05):** full mode (`SMASH_VM_GIB=48
SMASH_COLD_TIMEOUT_SEC=10 SMASH_DEFER_MADVISE=1`) on test7_full now passes
**5/5** consecutive runs (681s, 556s, 693s, 703s, 588s), versus the pre-fix
~1/3 rate. All 16 smash unit tests also pass.

| Config | Pre-fix | Post-fix |
|--------|---------|----------|
| full_default | ~1/3 (33%) | **5/5 (100%)** |

**Current recommendation:** `SMASH_LARGE_ONLY=1` remains the conservative
production default until the fixed full mode accumulates more soak time across
additional HLOs, but full mode is now functionally correct on test7_full.

### Full Mode Analysis

**Smash full mode achieves lower peak RSS than jemalloc:**
- Peak RSS: 17.8 GB vs jemalloc's 18.8 GB (**5% reduction**)
- Wall time: 468s vs jemalloc's 283s (66% slower)
- Average RSS: 9.9 GB vs jemalloc's 11.7 GB (**15% reduction**)

The RSS reduction comes from:
- Arena routing concentrating similar allocations on the same pages
- Cold page compression (zstd) during compilation pauses
- Deferred zeroing of freed slots for better compression ratios

**Trade-off:** Smash adds significant wall-time overhead (66% slower than jemalloc) due to:
- Compression/decompression cycles
- Fault handling for compressed pages
- Additional bookkeeping for page state tracking

### Comparison Summary

| Allocator | Wall Time | Peak RSS | vs jemalloc Peak | Notes |
|-----------|-----------|----------|------------------|-------|
| glibc     | 372s      | 35,719 MB | +90% | System default |
| mimalloc  | 258s      | 21,003 MB | +12% | Fastest |
| jemalloc  | 283s      | 18,766 MB | baseline | Best RSS without smash |
| **smash full** | **468s** | **17,780 MB** | **-5%** | Lowest peak RSS |
| smash LARGE_ONLY | 478s | 31,307 MB | +67% | Not competitive |

## Conclusion

Smash full mode achieves the lowest peak RSS of all tested allocators, beating jemalloc by 5%. However, the 66% wall-time overhead may not be acceptable for all use cases. The sweet spot depends on whether memory or time is the binding constraint.
