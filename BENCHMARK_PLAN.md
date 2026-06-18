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

### In Progress
- Fix __builtin_return_address depth: with LTO, the inlined callsiteArena sees
  the wrong return address. Fix: capture RA at the malloc entry point (before
  inlining) and use it for arena routing on both fast and slow paths.
- Compile-time tagging alternative: use __builtin_FILE()/__builtin_LINE() or
  a source_location parameter to tag call sites at compile time (no runtime
  stack walk needed). This would be passed via a defaulted parameter:
  `void* malloc(size_t size, std::source_location loc = std::source_location::current())`
  Limitation: only works for C++20 callers; C code and legacy binaries can't
  use this. A hybrid approach: use source_location when available, fall back to
  __builtin_return_address otherwise.

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
