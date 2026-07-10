# Benchmark Plan: Arena Segregation

## Goal
Demonstrate that smash's call-site-based arena routing produces page-level
content homogeneity that improves compression ratios vs glibc/jemalloc/mimalloc.

## Status

### Completed
- Per-arena thread cache lanes (kCacheLanes=4) — objects serve ONLY from the
  matching arena's lane, enforcing segregation
- Murmur3 hash finalizer to spread closely-spaced addresses across arenas
- bench_arena_segregation.cpp with 4 data types (JSON, floats, random, ints),
  hotness differentiation, anti-ICF and anti-tailcall measures

### Completed (2026-07-10)
- Return-address capture fixed for real: alloc8's app-facing wrapper entries
  (malloc/calloc/realloc/memalign/operator new/...) now store
  `__builtin_return_address(0)` into the initial-exec TLS hint
  `alloc8_caller_ra` (alloc8 PR #12); smash reads it via `appCallerRA()`.
  Capturing inside smash was unreliable — the frame count between the app and
  `SmashHeap::malloc` depends on inlining/LTO/tail-call decisions (measured on
  macOS Release: every RA reaching callsiteArena pointed into libsmash's own
  wrapper code → all call sites collapsed into one arena, homogeneity 0%).
  After the fix: homogeneity 100%, fully-mixed 0%, per-site page ratios
  json 0.026 / floats 0.037 / random 1.0 (correctly skipped) / ints 0.63.
  Verify RA arrival on any platform with `SMASH_ARENA_TRACE=1`.

### Not pursued
- Compile-time tagging alternative (source_location): unnecessary now that the
  wrapper-entry capture works, and it can't cover C callers or change malloc's
  ABI anyway. See "Compile-Time Tagging Design" below for the archived notes.

### Expected Results (once RA fix lands)
| Config | Homogeneity | Avg Ratio | Pages Compressible | RSS Reduction |
|--------|-------------|-----------|-------------------|---------------|
| glibc  | 0%          | 0.60      | 100% (mixed)      | 0%            |
| smash  | ~75%+       | ~0.35     | ~67% (skip random) | ~45-55%       |
| smash (no arena) | 0% | 0.60   | 100% (mixed)      | ~40%          |

The key differentiator: smash correctly SKIPS incompressible pages (site C
random data, ratio ~1.0) while compressing only the homogeneous compressible
pages (site A JSON at ratio ~0.01, site B floats at ratio ~0.15).
Without segregation, every page is a mix and achieves ~0.60 ratio — technically
below the 0.75 threshold, but the random bytes waste compressed-store space
and the compressor can't distinguish hot-random from cold-compressible.

## Compile-Time Tagging Design

```cpp
// C++20 source_location approach (zero-cost for the caller):
void* malloc(size_t size,
             std::source_location loc = std::source_location::current()) {
    uint32_t tag = hash(loc.file_name(), loc.line());
    uint8_t arena = tag & getArenaMask();
    ...
}
```

Pros:
- Exact call-site identity (file+line), no ambiguity
- Works across LTO, PGO, any optimization level
- Stable across ASLR (file paths don't change)
- Zero runtime cost (compiler inserts constants)

Cons:
- C++20 only (not available for C callers or pre-C++20 code)
- Changes malloc's ABI (extra parameter) — breaks LD_PRELOAD interposition
- All callers in the same file+line get the same arena (macro-expanded malloc
  wrappers would all hash to the wrapper's location, not the ultimate caller)

Hybrid approach:
- Use __builtin_return_address(0) for the interposed malloc (captures app site)
- Use source_location for direct calls from smash-aware C++20 code
- The key fix is just getting __builtin_return_address to work correctly,
  which means capturing it BEFORE LTO inlines callsiteArena into the entry point

## Running the Benchmark

```bash
# System malloc (baseline — no compression, shows page mixing):
./bench_arena_segregation

# Smash with segregation:
LD_PRELOAD=libsmash.so SMASH_COLD_TIMEOUT_SEC=2 ./bench_arena_segregation

# Smash WITHOUT arena routing (ablation):
LD_PRELOAD=libsmash.so SMASH_COLD_TIMEOUT_SEC=2 SMASH_ABLATION_NO_CALLSITE_ARENA=1 ./bench_arena_segregation
```
