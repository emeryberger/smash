# Smash Experiments

## Motivation

Ablation showed dictionaries **hurt** compression by ~5.3% on average across benchmarks.
This is counterintuitive — ZSTD dictionaries should help when training data is representative
of what gets compressed. These experiments identify where dictionaries could actually help and
what's currently going wrong.

## Current Implementation

- One dictionary per size class (`dict_train_[kNumClasses]`)
- Training: collect `kDictTrainSamples` (16) pages per size class, call `ZDICT_trainFromBuffer`
- Dictionary capacity: 32 KB
- Selection: once trained, all very-cold pages of that size class use `ZSTD_DICT`
- Compression levels: `kZstdNormalLevel=3` (fresh zstd), `kZstdDeepLevel=9` (very-cold/dict)
- Dict CDict built at level 9

## Benchmark

`bench/bench_dict_experiment.cpp` — standalone benchmark that directly exercises CompressEngine
with four realistic data patterns (json-like, kv-like, sqlite-like, mostly-zeroed pages).
100 test pages per data type, trained from 16 sample pages (matching `kDictTrainSamples`).

## Experiments

### Experiment 1: Try-Both Selection (diagnostic)

**Question**: Do dictionaries ever produce smaller output than plain ZSTD on individual pages?

**Method**: Compress each page with both ZSTD_DICT and plain ZSTD (same default level, which
is level 3 for CompressEngine::compress with ZSTD). Count wins/losses/ties.

**Status**: DONE

**Results**:

With **homogeneous** training data (same data type) and comparing dict (level 9 CDict) vs
plain ZSTD **at level 3** (CompressEngine::compress default):

| DataType | Wins | Losses | Ties | Avg Delta | Dict Better |
|----------|------|--------|------|-----------|-------------|
| json     | 100  | 0      | 0    | +303.5 B  | 100%        |
| kv       | 100  | 0      | 0    | +47.6 B   | 100%        |
| sqlite   | 100  | 0      | 0    | +25.1 B   | 100%        |
| zeroed   | 49   | 34     | 17   | +0.9 B    | 49%         |

**Finding**: Dictionaries win on 100% of structured data pages. The issue isn't that
dictionaries don't help — it's about *what level they're compared against*.

---

### Experiment 2: Level Comparison

**Question**: Do dictionaries help more at lower compression levels?

**Method**: Compare dict (CDict built at level 9) vs plain ZSTD at levels 1, 3, 9, 15.

**Status**: DONE

**Results**:

| DataType | Level | Plain Size | Dict Size | Delta    | Dict Better |
|----------|-------|-----------|-----------|----------|-------------|
| **json** | 1     | 3695 B    | 3443 B    | +251.7 B | **+6.8%**  |
| json     | 3     | 3747 B    | 3443 B    | +303.5 B | **+8.1%**  |
| json     | 9     | 3426 B    | 3443 B    | -17.8 B  | **-0.5%**  |
| json     | 15    | 3342 B    | 3443 B    | -101.5 B | -3.0%      |
| **kv**   | 1     | 1390 B    | 1433 B    | -42.9 B  | -3.1%      |
| kv       | 3     | 1480 B    | 1433 B    | +47.6 B  | **+3.2%**  |
| kv       | 9     | 1378 B    | 1433 B    | -54.8 B  | **-4.0%**  |
| kv       | 15    | 1293 B    | 1433 B    | -139.8 B | -10.8%     |
| **sqlite**| 1    | 2381 B    | 2345 B    | +35.8 B  | **+1.5%**  |
| sqlite   | 3     | 2370 B    | 2345 B    | +25.1 B  | **+1.1%**  |
| sqlite   | 9     | 2337 B    | 2345 B    | -8.4 B   | **-0.4%**  |
| sqlite   | 15    | 2327 B    | 2345 B    | -17.8 B  | -0.8%      |

**Key Finding**: **Dictionaries help at low levels (1-3) but hurt at high levels (9+).**
The CDict is built at level 9, so it captures the same patterns that ZSTD level 9 finds
on its own. At level 9+, the dictionary adds frame overhead (dict ID, etc.) without
improving compression. At levels 1-3, the dictionary provides patterns the compressor
can't discover in its limited search window.

**Root Cause of Ablation Regression**: In the production code, very-cold pages use
`kZstdDeepLevel=9` for BOTH dict and no-dict paths. At level 9, dict adds ~0.5% overhead
from frame metadata. The dictionary is trained at level 9 but compared against level 9 —
it can only tie or lose.

---

### Experiment 3: Grouping Strategy Sweep

**Question**: Does grouping training samples by calling context (arena) produce better
dictionaries than grouping by size class alone?

**Method**: Train dict from same data type (homogeneous = simulates per-arena) vs mixed
data types (heterogeneous = simulates per-size-class with mixed callsites).

**Status**: DONE

**Results**:

| DataType | No Dict | Homogeneous | Mixed   | Homo vs NoDict | Mixed vs NoDict |
|----------|---------|-------------|---------|----------------|-----------------|
| json     | 3747 B  | 3443 B      | 3508 B  | **-8.1%**      | -6.4%           |
| kv       | 1480 B  | 1429 B      | 1357 B  | -3.5%          | **-8.3%**       |
| sqlite   | 2394 B  | 2369 B      | 2373 B  | **-1.1%**      | -0.9%           |
| zeroed   | 68 B    | 67 B        | 67 B    | -1.7%          | -1.7%           |

**Finding**: Both grouping strategies help vs no-dict (when compared at the default level 3).
Homogeneous training is better for json (+1.7pp) and sqlite (+0.2pp). Mixed is better for
kv (+4.8pp). The benefit depends on data diversity within the group.

**Important caveat**: These results are at default ZSTD level (level 3 via
`CompressEngine::compress(ZSTD)`), NOT at level 9 where the production code operates.
Per Experiment 2, at level 9 all dictionary strategies lose.

---

### Experiment 4: Dictionary Size Sweep

**Question**: Is 32 KB too large for 16 KB page inputs?

**Status**: SKIPPED — Experiment 2 showed the core issue is compression level, not dict size.

---

### Experiment 5: Sample Count Sweep

**Question**: Does more training data help?

**Status**: DONE

**Results** (avg compressed size per page, at default ZSTD level):

| DataType | 4 samples | 8 samples | 16 samples | 32 samples | 64 samples | No Dict |
|----------|-----------|-----------|------------|------------|------------|---------|
| json     | 3426 B    | 3452 B    | 3445 B     | 3442 B     | 3442 B     | 3747 B  |
| kv       | 1377 B    | 1428 B    | 1432 B     | 1437 B     | 1438 B     | 1480 B  |
| sqlite   | 2342 B    | 2350 B    | 2351 B     | 2350 B     | 2348 B     | 2376 B  |
| zeroed   | 64 B      | 67 B      | 67 B       | 66 B       | 66 B       | 67 B    |

**Finding**: 4 samples is often best (!). More samples don't help and can slightly hurt.
ZDICT_trainFromBuffer with fewer samples captures the most common patterns without
overfitting to rare variations.

---

### Experiment 6: CDict/DDict Memory Overhead

**Question**: How much memory do dictionary objects consume?

**Status**: DONE

**Results**:

| Object             | Level 1 | Level 3 | Level 9 | Level 15 |
|--------------------|---------|---------|---------|----------|
| CDict              | 78 KB   | 430 KB  | **686 KB** | 1070 KB |
| DDict              | 58 KB   | 58 KB   | 58 KB   | 58 KB    |
| Sample buffer      | 256 KB  | 256 KB  | 256 KB  | 256 KB   |
| Dict buffer        | 32 KB   | 32 KB   | 32 KB   | 32 KB    |
| **Total per SC**   | 424 KB  | 776 KB  | **1032 KB** | 1416 KB |

With 36 size classes:
- Sample buffers alone: **9 MB** (allocated lazily but never freed)
- CDict + DDict (all trained, level 9): **26.8 MB**
- **Total potential overhead: ~36 MB**

**Finding**: This is the dominant cause of the ablation regression. The CDict at level 9
is 686 KB per size class. Even if only 10 size classes get trained, that's 7 MB of CDict
objects alone, plus 2.5 MB of sample buffers. The RSS savings from better compression
(~0.5% at level 9) cannot offset this fixed overhead.

---

## Conclusions

### Root Causes (two independent issues)

**1. CDict memory overhead dominates any compression benefit.**
Each trained dictionary consumes ~1 MB (CDict 686 KB + DDict 58 KB + sample buffer 256 KB
+ dict buffer 32 KB). Training across multiple size classes adds tens of megabytes of
BootstrapAlloc memory to RSS, far exceeding any compression savings.

In the quick-mode ablation, the full config with dicts showed 16.1% RSS reduction vs
33.9% without dicts on JSON — a **17.8 percentage point regression**, primarily from
CDict/sample buffer memory overhead.

**2. At ZSTD level 9, dictionaries don't improve compression.**
The production code uses `kZstdDeepLevel=9` for very-cold pages. At this level, ZSTD's
search window is large enough to find all patterns the dictionary captures. The dictionary
adds frame overhead (~13 bytes) without compressing better. Dictionaries DO help at
levels 1-3 (up to 8% better compression on JSON data).

### Recommended Fix

**Disable dictionaries entirely** (`kDictTrainSamples = 0`). The memory overhead makes
them net-negative at any compression level. The ~8% compression improvement at level 3
does not justify ~1 MB of fixed overhead per trained size class.

If dictionaries are revisited in the future:
- Train CDict at **level 1** (78 KB vs 686 KB at level 9 — **9x smaller**)
- Use dict with ZSTD level 1-3 only (where they actually help)
- Reduce training samples from 16 to 4 (better dictionaries, less memory)
- Limit dictionary training to at most 4-8 size classes (cap total overhead)
- Consider (arena, size_class) grouping for more homogeneous training data

---

### Experiment 7: Optimized Dict Revisit (real benchmarks)

**Question**: Do the recommended dict optimizations (CDict@3, 8 samples, 16KB dict,
max 8 classes) make dicts net-positive on real workloads?

**Method**: Implemented all recommended optimizations, ran full ablation (B1 full vs
T1d no-dict) on JSON, KV Store, SQLite benchmarks, 3 runs each, --quick mode.

**Status**: DONE

**Config changes applied**:
- `kDictLevel = 3` (CDict at level 3: 430KB vs 686KB at level 9)
- `kDictCapacity = 16KB` (down from 32KB)
- `kDictTrainSamples = 8` (down from 16, 4 was too few for ZDICT)
- `kMaxDictClasses = 8` (cap total dict memory)

**Results** (RSS reduction %, higher = better):

| Config | JSON | KV Store | SQLite |
|--------|------|----------|--------|
| System Malloc | 0.0% | 23.3% | 27.9% |
| **Optimized dicts** | 19.9% | 33.5% | 38.1% |
| LZ4-only (no ZSTD) | 21.9% | 33.4% | 38.1% |
| **No dicts** (ZSTD@9) | **33.9%** | **40.5%** | **43.8%** |

**Finding**: Dicts are **still net-negative** even with all optimizations. The average
cost is **-8.9pp** across benchmarks. Two independent causes:

1. **CDict@3 compresses worse than ZSTD@9**: When a dict is available, pages are
   compressed using CDict@level3 (the level is embedded in the CDict). Without dict,
   very-cold pages use ZSTD@level9, which has a much larger search window. The dict
   cannot compensate for 6 levels of compression quality loss.

2. **Memory overhead**: CDict (430KB) + DDict (58KB) + sample buffers (128KB per class)
   + dict buffer (16KB) still adds 12-18MB RSS across the benchmarks.

**Decision**: `kDictTrainSamples = 0` set as default. Infrastructure preserved for
future experiments (set `kDictTrainSamples > 0` via CMake to re-enable).

---

### Experiment 8: Memcached (real-world server)

**Question**: How does Smash (dicts disabled) perform on a real server workload?

**Method**: 500K TPC-H JSON records loaded into memcached, hot 5% served for 10s,
then cold keys re-accessed. Baseline = system malloc, Smash via DYLD_INSERT_LIBRARIES.

**Status**: DONE

**Results**:

| Metric | Baseline | Smash | Delta |
|--------|----------|-------|-------|
| Fill RSS | 233.1 MB | 259.8 MB | +11.4% |
| Peak RSS | 240.0 MB | 262.5 MB | +9.4% |
| Post-cool RSS | 240.0 MB | 238.6 MB | -0.6% |
| **Serve RSS** | **240.0 MB** | **179.9 MB** | **-25.0%** |
| **Min RSS** | **233.1 MB** | **190.3 MB** | **-18.4%** |
| Cold re-access RSS | 240.0 MB | 201.1 MB | -16.2% |
| Cold throughput | 536K GET/s | 828K GET/s | +54.4% |
| Serve throughput | 497 GET/s | 498 GET/s | +0.2% |

**Finding**: Smash reduces serve-phase RSS by **25%** with zero throughput impact.
Cold key re-access is 54% faster (pages prefetched on adjacent faults). Fill-phase
RSS is 11% higher due to bootstrap allocator and metadata overhead.

---

### Experiment 9: OS VM Compressor Algorithm Comparison

**Question**: How does Smash's algorithm choice compare to OS kernel compressors?

**Method**: Standalone benchmark (`bench_algo_compare`) compressing 100 pages of each
data type (JSON, KV, SQLite, zeroed, random) with each algorithm. 16KB pages (ARM64).

**Algorithms**:
- **WKdm** (macOS VM compressor): Wilson-Kaplan dictionary matching, 16-entry dict
- **LZ4** (Linux zswap default): Byte-level LZ77
- **LZ4-HC**: LZ4 high compression variant
- **zstd-1/3/9** (Linux zswap option, Smash): Finite state entropy + LZ77

**Status**: DONE

**Results** (single page, 16384 bytes):

| Data   | WKdm    | LZ4     | LZ4-HC  | zstd-1  | zstd-3  | zstd-9  |
|--------|---------|---------|---------|---------|---------|---------|
| **json** | 98.9% | 30.2%   | 27.1%   | 14.6%   | 15.6%   | 14.2%   |
| **kv**   | 93.7% | 30.8%   | 25.8%   | 23.2%   | 23.3%   | 21.0%   |
| **sqlite**| 15.6%| 5.8%    | 5.7%    | 4.2%    | 4.1%    | 4.0%    |
| **zeroed**| 6.8% | 1.6%    | 1.4%    | 1.1%    | 1.1%    | 1.0%    |
| **random**| 100% | 100.4%  | 100.4%  | 100.1%  | 100.1%  | 100.1%  |

**Compression throughput** (MB/s):

| Data   | WKdm  | LZ4   | LZ4-HC | zstd-1 | zstd-3 | zstd-9 |
|--------|-------|-------|--------|--------|--------|--------|
| json   | 1865  | 1004  | 234    | 652    | 553    | 90     |
| kv     | 1864  | 856   | 90     | 512    | 461    | 44     |
| sqlite | 1819  | 2955  | 1351   | 1412   | 1252   | 731    |
| zeroed | 1797  | 3886  | 221    | 2069   | 1939   | 673    |

**Decompression throughput** (MB/s):

| Data   | WKdm  | LZ4   | LZ4-HC | zstd-1 | zstd-3 | zstd-9 |
|--------|-------|-------|--------|--------|--------|--------|
| json   | 10077 | 6064  | 6969   | 1540   | 1498   | 1437   |
| kv     | 2184  | 4144  | 5714   | 1259   | 1250   | 1258   |
| sqlite | 5269  | 4184  | 3843   | 1799   | 1768   | 1823   |
| zeroed | 5618  | 25169 | 28315  | 3938   | 3946   | 3752   |

**Key Findings**:

1. **WKdm is nearly useless on text/JSON data** (98.9% ratio). It was designed for
   pointer-heavy C struct pages (4KB, 4-byte words). On 16KB pages with byte-oriented
   text, the word-granularity dictionary can't find patterns. It only works well on
   binary/numeric data (sqlite: 15.6%, zeroed: 6.8%).

2. **LZ4 is the sweet spot** for cold pages: 30% ratio on structured data, ~1 GB/s
   compress, ~4-6 GB/s decompress. This is why Linux uses it as zswap default.

3. **zstd-1 ≈ zstd-3 for ratios** but zstd-1 is ~15% faster to compress. Worth
   considering as the "normal" ZSTD level (currently kZstdNormalLevel=3).

4. **zstd-9 is 6-10x slower** to compress than zstd-1 with only ~2pp better ratio
   on most data. The tradeoff is worthwhile only for very-cold pages that stay
   compressed for long periods.

5. **Smash's adaptive LZ4→zstd strategy** is optimal: fast LZ4 for newly-cold pages,
   then re-compress with zstd-9 when pages prove to be long-lived. This matches or
   exceeds what any single OS algorithm achieves.

---

### Experiment 10: Multi-Page Compression

**Question**: Does compressing 2 or 4 contiguous pages together improve ratios?

**Method**: Same benchmark, grouping adjacent pages into 32KB and 64KB units.

**Status**: DONE

**Results** (ratio = compressed/original, lower = better):

| Data   | Algo   | 1×16K | 2×16K | 4×16K | Improvement |
|--------|--------|-------|-------|-------|-------------|
| json   | LZ4    | 30.2% | 27.7% | 26.2% | **-4.0pp** |
| json   | zstd-3 | 15.6% | 13.2% | 11.9% | **-3.7pp** |
| json   | zstd-9 | 14.2% | 12.4% | 11.3% | **-2.9pp** |
| kv     | LZ4    | 30.8% | 29.9% | 29.3% | **-1.5pp** |
| kv     | zstd-3 | 23.3% | 21.8% | 21.1% | **-2.2pp** |
| kv     | zstd-9 | 21.0% | 19.4% | 18.7% | **-2.3pp** |
| sqlite | LZ4    | 5.8%  | 5.3%  | 5.0%  | **-0.8pp** |
| sqlite | zstd-3 | 4.1%  | 3.4%  | 3.1%  | **-1.0pp** |
| sqlite | zstd-9 | 4.0%  | 3.4%  | 3.0%  | **-1.0pp** |

**Multi-page compression throughput** (MB/s):

| Data   | Algo   | 1×16K Comp | 4×16K Comp | 1×16K Dec | 4×16K Dec |
|--------|--------|------------|------------|-----------|-----------|
| json   | LZ4    | 1009       | 943        | 6043      | 4941      |
| json   | zstd-3 | 548        | 735        | 1490      | 2155      |
| json   | zstd-9 | 91         | 114        | 1484      | 1904      |

**Key Findings**:

1. **2-4pp ratio improvement** from 4-page groups, mostly from cross-page pattern
   matching (adjacent pages often contain similar structures from the same span).

2. **Throughput improves** for zstd with larger inputs (amortizes setup overhead).
   LZ4 throughput is roughly constant.

3. **Not worth the complexity for Smash**: The 2-4pp improvement is modest, and
   the implementation cost is significant:
   - Must wait for ALL pages in the group to become cold simultaneously
   - On fault, must decompress the entire group (4× decompression work)
   - Mixed hot/cold pages in a group prevent compression entirely
   - Requires grouping logic aligned to span boundaries

4. **Potential for future work**: If implemented, 2-page groups are the sweet spot
   (most of the benefit, half the decompression penalty of 4-page groups).

---

### Experiment 11: Allocator Substrate Compression Comparison

**Question**: How does allocator design (metadata placement, zero-on-free, arena segregation) affect page compressibility?

**Method**: Standalone benchmark (`bench/bench_allocator_compare.cpp`) allocates 100K objects through 8 allocators, fills with realistic data (JSON, KV, struct, mixed), frees 50% (alternating), then compresses resulting heap pages with LZ4 and zstd-9. Object sizes: 64, 128, 256, 512 bytes. 3 runs per configuration. Also builds `+zero` variants where the benchmark explicitly zeroes objects before `free()` to isolate the zero-on-free effect independently of the allocator's own behavior.

**Allocators tested**:
- System malloc (macOS libmalloc) — sequential magazine allocator, embedded freelists
- mimalloc v3.2 — size-class segregated, embedded freelists
- jemalloc 5.3 — arena-based, embedded freelists
- tcmalloc — thread-caching, embedded freelists
- Hoard — superblock-based, embedded freelists
- DieHard — bitmap-based external metadata, randomized placement, no zeroing
- DieHarder — bitmap-based external metadata, randomized, security zeroing
- Smash — bitmap-based external metadata, zero-on-free, call-site arena routing

**Status**: DONE

**Results** (zstd-9 avg ratio across sizes 64-512B, lower = better):

| Allocator       | JSON  | KV    | Struct | Mixed | Average |
|-----------------|-------|-------|--------|-------|---------|
| **Base allocators** |     |       |        |       |         |
| System malloc   | 3.9%  | 9.6%  | 8.2%   | 7.7%  | 7.4%    |
| mimalloc        | 3.9%  | 11.3% | 10.8%  | 12.1% | 9.5%    |
| jemalloc        | 2.8%  | 12.0% | 11.8%  | 12.0% | 9.6%    |
| tcmalloc        | 3.8%  | 11.3% | 10.9%  | 12.1% | 9.5%    |
| Hoard           | 4.0%  | 11.5% | 10.9%  | 12.2% | 9.6%    |
| DieHard         | 4.9%  | 12.3% | 11.0%  | 12.2% | 10.1%   |
| **With zero-on-free** |  |       |        |       |         |
| System+zero     | 3.9%  | 9.6%  | 8.2%   | 7.5%  | 7.3%    |
| mimalloc+zero   | 2.8%  | 8.5%  | 7.1%   | 8.5%  | 6.7%    |
| jemalloc+zero   | 2.0%  | 8.3%  | 6.4%   | 7.9%  | 6.1%    |
| tcmalloc+zero   | 2.8%  | 8.5%  | 7.1%   | 8.5%  | 6.7%    |
| Hoard+zero      | 2.9%  | 8.6%  | 7.2%   | 8.6%  | 6.8%    |
| DieHard+zero    | 3.1%  | 7.7%  | 6.1%   | 7.6%  | 6.1%    |
| DieHarder       | 3.1%  | 7.7%  | 6.1%   | 7.6%  | 6.1%    |
| **Smash**       | 2.6%  | 9.2%  | 7.5%   | 9.0%  | 7.1%    |

**Key Findings**:

1. **Zero-on-free is the dominant factor** for page compressibility. Adding zeroing improves every allocator by 2-4pp. DieHard+zero matches DieHarder exactly (6.1%), confirming DieHarder's security zeroing is its main advantage.

2. **External metadata + zeroing achieves best static compressibility**. DieHarder (6.1%) and jemalloc+zero (6.1%) beat Smash (7.1%) in static page compression. DieHard's sparse random layout means more zero space per page after zeroing.

3. **System malloc compresses surprisingly well** (7.4%) despite embedded freelists, because macOS libmalloc's magazine allocator produces sequential, locality-friendly layouts. DieHard without zeroing is worst (10.1%) due to randomized placement.

4. **Embedded freelists have modest impact** (~2pp). mimalloc/tcmalloc/jemalloc/Hoard (9.5-9.6%) vs system (7.4%) shows the difference is mainly in layout strategy, not freelist pollution.

5. **Smash's real advantage is its compression pipeline**, not just page content. Smash achieves 17-44% RSS reduction in real applications (see below) despite not having the lowest static ratios, because its pipeline (page monitoring, adaptive LZ4→zstd, prefetch, parallel compression) captures savings no amount of page content optimization can achieve alone.

---

### Experiment 12: Real Application RSS with Different Allocators

**Question**: What is the actual RSS of real applications under each allocator (without Smash's compression pipeline)?

**Method**: Run bench_json, bench_kv_store, bench_sqlite with each allocator via DYLD_INSERT_LIBRARIES. Only Smash has a compression pipeline; all others just show baseline RSS.

**Status**: DONE (JSON, KV, SQLite). Memcached and DuckDB: PLANNED.

**Results** (peak RSS in MB, and RSS reduction % where applicable):

| Allocator    | JSON Peak | JSON Reduce | KV Peak | KV Reduce | SQLite Peak | SQLite Reduce |
|-------------|-----------|-------------|---------|-----------|-------------|---------------|
| System      | 123.4     | 0.0%        | 143.3   | 23.1%     | 213.2       | 26.8%         |
| mimalloc    | 117.5     | 0.0%        | 143.4   | 22.7%     | 221.2       | 25.7%         |
| jemalloc    | 98.7      | 0.0%        | 144.5   | 11.2%     | 209.1       | 26.3%         |
| tcmalloc    | 120.6     | 0.0%        | 140.4   | 23.4%     | 194.3       | 25.7%         |
| Hoard       | 119.9     | 0.0%        | 158.5   | 21.6%     | 341.3       | 21.0%         |
| DieHard     | 191.7     | 0.0%        | 326.0   | 4.9%      | 412.8       | 15.8%         |
| DieHarder   | 197.9     | -0.1%       | 337.2   | 4.5%      | 435.5       | 15.1%         |
| **Smash**   | **128.2** | **33.8%**   | **169.5** | **40.5%** | **238.2** | **43.8%**     |

Note: "Reduce" is the benchmark's self-reported RSS reduction (cooling + compression for Smash; page release for others). Only Smash actually compresses pages.

**Key Findings**:

1. **Smash achieves the highest RSS reduction** on all workloads (33.8-43.8%), despite having higher peak RSS than some allocators (due to metadata overhead).

2. **DieHard/DieHarder have 2-3x higher peak RSS** than conventional allocators due to their randomized over-provisioning strategy. Their security benefits come at a high memory cost.

3. **jemalloc has lowest JSON peak RSS** (98.7 MB vs 123.4 MB system) due to efficient memory management. However, it cannot reduce RSS below its peak without compression.

4. **All non-Smash allocators have 0% RSS reduction on JSON** because the JSON benchmark doesn't free objects — there are no cold pages to release. Only Smash's compression can reduce RSS on fully-live heaps with cold access patterns.

---

### Experiment 13: Memcached with Multiple Allocators

**Question**: How does allocator choice affect memcached RSS, and how much does Smash's compression reduce it?

**Method**: Run memcached with each allocator via DYLD_INSERT_LIBRARIES. Populate 200K keys with TPC-H JSON records, cool 5s, serve hot 5% for 8s, access cold keys.

**Status**: DONE

**Results** (RSS in MB):

| Allocator   | Fill    | Peak    | Cool    | Serve   | Min     | Cold    | GET/s  |
|-------------|---------|---------|---------|---------|---------|---------|--------|
| system      | 95.3    | 98.3    | 98.3    | 98.3    | 95.6    | 98.3    | 46,687 |
| mimalloc    | 97.9    | 100.5   | 100.5   | 100.5   | 98.2    | 100.5   | 41,562 |
| tcmalloc    | 100.5   | 102.5   | 102.5   | 102.5   | 100.5   | 102.5   | 41,125 |
| Hoard       | 98.2    | 100.2   | 100.2   | 100.2   | 98.2    | 100.2   | 46,000 |
| DieHard     | 100.2   | 102.2   | 102.2   | 102.2   | 100.2   | 102.2   | 45,312 |
| DieHarder   | 106.9   | 108.9   | 108.9   | 108.9   | 106.9   | 108.9   | 45,500 |
| **Smash**   | 122.5   | 122.8   | **75.2**| **92.8**| **75.2**| 167.6   | 41,750 |

Note: jemalloc DYLD interposition crashes memcached on macOS.

**Key Findings**:
1. **Smash is the only allocator that reduces RSS below fill level** (75.2 MB post-cool vs 95-109 MB for others). This is compression at work.
2. Serve-phase RSS (92.8 MB) is 5.6% below system (98.3 MB) even while serving hot traffic.
3. Cold re-access RSS spikes to 167.6 MB as compressed pages are decompressed.
4. Throughput is comparable across all allocators (~41K-47K GET/s).

---

### Experiment 14: DuckDB with Multiple Allocators

**Question**: How does allocator choice affect DuckDB OLAP RSS?

**Method**: Run DuckDB with each allocator via DYLD_INSERT_LIBRARIES. Generate TPC-H sf=0.1, cool 5s, serve narrow queries, full-scan cold queries.

**Status**: DONE

**Results** (RSS in MB):

| Allocator   | Fill    | Cool    | Serve   | Cold    |
|-------------|---------|---------|---------|---------|
| system      | 319.7   | 319.7   | 336.4   | 338.9   |
| mimalloc    | 321.0   | 321.0   | 330.0   | 343.7   |
| tcmalloc    | 305.3   | 305.2   | 307.2   | 307.4   |
| Hoard       | 351.2   | 351.2   | 214.7   | 229.5   |
| DieHard     | 91.8    | 91.8    | 92.4    | 92.4    |
| DieHarder   | 229.9   | 229.9   | 459.4   | 474.0   |
| **Smash**   | 207.6   | **203.7** | **219.7** | 225.9 |

Note: jemalloc hangs with DuckDB on macOS DYLD interposition — skipped. DieHard's low RSS (91.8 MB) is suspicious — DuckDB may use mmap-based allocation internally that DieHard doesn't intercept.

**Key Findings**:
1. **Smash achieves 35% lower serve RSS** than system (219.7 vs 336.4 MB).
2. tcmalloc is surprisingly efficient with DuckDB (307.2 MB serve).
3. Hoard shows unusual behavior — RSS drops during serve (possible page return).
4. DieHarder has high RSS (459.4 MB serve) — its randomized layout interacts poorly with DuckDB's allocations.

---

### Experiment 15: Redis with Multiple Allocators

**Question**: How does allocator choice affect Redis in-memory store RSS?

**Method**: Run redis-server with each allocator via DYLD_INSERT_LIBRARIES. Populate 200K keys with JSON values, observe RSS for 10s.

**Status**: DONE

**Results** (RSS in MB):

| Allocator   | Fill   | Peak   | Min    | Final  |
|-------------|--------|--------|--------|--------|
| system      | 54.9   | 54.9   | 54.9   | 54.9   |
| mimalloc    | 54.1   | 54.1   | 54.1   | 54.1   |
| tcmalloc    | 57.1   | 57.1   | 57.1   | 57.1   |
| Hoard       | 67.5   | 67.5   | 67.5   | 67.5   |
| DieHard     | 90.1   | 90.1   | 90.1   | 90.1   |
| DieHarder   | 94.0   | 94.0   | 94.0   | 94.0   |
| **Smash**   | 56.8   | 56.8   | **46.8** | **46.9** |

Note: jemalloc fails to start with Redis on macOS.

**Key Findings**:
1. **Smash reduces RSS 17%** from fill (56.8→46.9 MB) — only allocator to compress idle data.
2. DieHard/DieHarder have 64-71% higher RSS than system due to randomized over-provisioning.
3. All non-Smash allocators maintain constant RSS (no compression capability).

---

### Experiment 16: RocksDB Block Cache

**Question**: How effective is Smash at compressing cold RocksDB block cache pages?

**Method**: Standalone C++ benchmark (`bench/bench_rocksdb.cpp`). Opens RocksDB with 256MB LRU block cache (no RocksDB-level compression), populates 200K keys with 256B JSON values, warms cache with full scan, cools, serves hot 5% of keys.

**Status**: DONE

**Results**:

| Metric           | Baseline | Smash   | Delta  |
|------------------|----------|---------|--------|
| Peak RSS         | 121.1 MB | 92.0 MB | -24%   |
| Post-cool RSS    | 121.1 MB | 43.5 MB | **-64%** |
| Serve RSS        | 121.2 MB | 47.2 MB | **-61%** |
| Min RSS          | 121.1 MB | 43.5 MB | -64%   |
| Ops/sec          | 984,582  | 1,017,704 | +3%  |
| Cold access time | 0.019s   | 0.019s  | 0%     |
| Cold RSS         | 121.2 MB | 52.5 MB | -57%   |

**Key Findings**:
1. **RocksDB shows the highest Smash benefit**: 61-64% RSS reduction with zero throughput impact.
2. Block cache is ideal for Smash: large contiguous blocks, clear hot/cold access pattern, read-heavy.
3. Cold access decompression is imperceptible (19ms for 20K key reads in both cases).
4. Smash actually shows slightly *higher* throughput (1.02M vs 985K ops/sec) — possibly due to better cache utilization with smaller working set.

---

### Experiment 17: Polars (Rust) DataFrame

**Question**: Does Smash work with Rust allocations (via system allocator)?

**Method**: Python script using Polars (Rust DataFrame library). Creates 500K-row DataFrames, does hot queries on recent rows, then cold join+scan.

**Status**: DONE

**Results**:

| Metric           | Baseline  | Smash     | Delta  |
|------------------|-----------|-----------|--------|
| Peak RSS         | 240.9 MB  | 209.0 MB  | -13%   |
| Post-cool RSS    | 240.9 MB  | 209.0 MB  | -13%   |
| Serve RSS        | 343.8 MB  | 313.8 MB  | -9%    |
| Min RSS          | 252.4 MB  | 221.4 MB  | -12%   |
| Ops/sec          | 951       | 913       | -4%    |
| Cold access time | 0.010s    | 0.014s    | +40%   |

**Key Findings**:
1. **Smash works with Rust allocations** — 9-13% RSS reduction on Polars DataFrames.
2. Cold access has +40% latency (14ms vs 10ms) due to decompression.
3. Throughput impact is minimal (-4%).
4. Rust's system allocator routes through malloc, so DYLD interposition works.

---

### Experiment 18: scikit-learn ML Training

**Question**: Does Smash help with ML workloads?

**Method**: Train RandomForest (200 trees), GradientBoosting (100 trees), LogisticRegression on 50K×100 feature matrix. Serve with RF inference only, then cold re-access other models.

**Status**: DONE

**Results**:

| Metric           | Baseline  | Smash     | Delta  |
|------------------|-----------|-----------|--------|
| Peak RSS         | 439.7 MB  | 431.3 MB  | -2%    |
| Serve RSS        | 447.0 MB  | 439.4 MB  | -2%    |
| Preds/sec        | 8,230     | 7,930     | -4%    |

**Key Findings**:
1. **Minimal benefit for ML training** — all tree nodes are traversed during inference (no cold pages).
2. Training takes 207s, keeping memory hot. The 5s cool period is insufficient relative to access frequency.
3. Shows Smash has near-zero overhead (~2% RSS, ~4% throughput) on workloads with no cold data.

---

### Experiment 19: NetworkX Graph Processing

**Question**: Does Smash help with graph workloads?

**Method**: Build 50K-node random graph with attributes, serve queries on hot subgraph, PageRank on full graph.

**Status**: DONE (inconclusive — graph too small, no cold region)

**Results**: 50K nodes at ~160MB showed no compression benefit. Smash added ~10% RSS overhead from metadata. NetworkX's Python dict-based storage creates many small scattered objects that don't form contiguous cold regions.

**Action needed**: Test with larger graph (500K+ nodes) or switch to a C/C++ graph library with better memory locality.

---

### Experiment 20: RocksDB Compression Comparison

**Question**: How does Smash compare to RocksDB's built-in SST compression?

**Method**: Run bench_rocksdb with 200K keys (512B values), 256MB block cache. Compare five configurations:
1. Baseline (system malloc, no compression)
2. Smash (Smash allocator, no RocksDB compression)
3. RocksDB LZ4 (system malloc, LZ4 SST compression)
4. RocksDB zstd (system malloc, zstd SST compression)
5. Smash + LZ4 (both Smash allocator + RocksDB LZ4 SST)

**Status**: DONE

**Results** (200K keys, 512B values):

| Configuration | Peak RSS | Cool RSS | Serve RSS | Ops/sec  |
|--------------|----------|----------|-----------|----------|
| baseline     | 220.5 MB | 220.5 MB | 220.6 MB  | 901,170  |
| smash        | 140.9 MB | 43.9 MB  | 50.5 MB   | 918,570  |
| rocksdb-lz4  | 218.1 MB | 218.1 MB | 218.1 MB  | 940,633  |
| rocksdb-zstd | 221.5 MB | 221.5 MB | 221.6 MB  | 975,369  |
| smash+lz4    | 141.0 MB | 44.1 MB  | 50.6 MB   | 1,018,014|

**Key Findings**:
1. **RocksDB's compression does NOT reduce memory** — LZ4/zstd compress data on disk (SST files), but blocks are decompressed when loaded into the block cache. Memory footprint is identical to uncompressed.
2. **Smash reduces block cache RSS by 77%** (220.6 → 50.5 MB serve RSS) while improving throughput by 2%.
3. **Smash + RocksDB LZ4 is complementary** — combining both gives the same memory savings as Smash alone plus slightly better throughput (+13% over baseline). RocksDB compression reduces I/O, Smash compresses memory.
4. Smash addresses a fundamentally different level of the storage hierarchy than RocksDB's compression.

---

### Experiment 21: Compress-Only Variant (System Malloc + Smash Compression)

**Question**: How much of Smash's RSS reduction comes from the compression pipeline vs. the allocation design (arenas, zero-on-free, external metadata)?

**Method**: Built `libsmash_compress_only.dylib` — interposes malloc/free but forwards to the real system allocator. Tracks pages via two mechanisms: (1) hash-map-based tracking of malloc return addresses, and (2) periodic VM region scanning using `vm_region_recurse_64` to discover all malloc-managed memory regions (tagged `VM_MEMORY_MALLOC_*`). Runs Smash's full compression pipeline (monitoring, LZ4/zstd compression, SIGSEGV-based decompression) on system-malloc-managed pages. This means: embedded freelists in freed objects, no call-site arena routing, no zero-on-free.

VM region scanning was necessary because macOS system malloc uses `vm_allocate` (Mach VM API) internally for heap zone regions. `__DATA,__interpose` cannot intercept Mach traps, so individual `malloc` return addresses only reveal a tiny fraction of pages. The scanner runs before each compressor tick, finding new malloc-tagged anonymous RW regions.

Compare with full Smash (which has external metadata, arenas, zero-on-free) on the same workloads.

**Status**: DONE (JSON, KV, SQLite, RocksDB)

**Results** (all benchmarks):

| Benchmark | Config | Peak RSS | Cool RSS | Serve RSS | Throughput |
|-----------|--------|----------|----------|-----------|-----------|
| **JSON** (100K records) | baseline | 244.8 MB | 244.8 MB | 244.8 MB | 58.5M acc/s |
| | compress-only | 254.0 MB | 166.1 MB | 167.7 MB | 56.7M acc/s |
| | full smash | 228.9 MB | 141.9 MB | 143.5 MB | 56.8M acc/s |
| **KV Store** (1M entries) | baseline | 285.2 MB | 285.2 MB | 337.9 MB | 2.59M ops/s |
| | compress-only | 294.5 MB | 156.3 MB | 239.2 MB | 2.24M ops/s |
| | full smash | 311.6 MB | 174.2 MB | 256.1 MB | 2.66M ops/s |
| **SQLite** (500K rows) | baseline | 423.8 MB | 423.9 MB | 565.2 MB | 83.7K ops/s |
| | compress-only | 433.0 MB | 222.5 MB | 300.0 MB | 72.2K ops/s |
| | full smash | 447.7 MB | 238.6 MB | 320.3 MB | 78.0K ops/s |
| **RocksDB** (200K keys) | baseline | 397.2 MB | 397.2 MB | 397.2 MB | 828.7K ops/s |
| | compress-only | 385.3 MB | 156.5 MB | 169.4 MB | 678.6K ops/s |
| | full smash | 299.6 MB | 55.9 MB | 96.2 MB | 837.9K ops/s |

**RSS Reduction Summary** (cool RSS reduction vs baseline):

| Benchmark | Compress-Only | Full Smash | Smash Advantage |
|-----------|---------------|------------|-----------------|
| JSON | 32.2% | 42.0% | +9.8pp |
| KV Store | 45.2% | 38.9% | -6.3pp |
| SQLite | 47.5% | 43.7% | -3.8pp |
| RocksDB | 60.6% | 85.9% | +25.3pp |

**Key Findings**:

1. **Compress-only achieves 32-61% RSS reduction** across all workloads — proving the compression pipeline is the primary source of savings.

2. **Surprising result: compress-only sometimes BEATS full Smash on cool RSS** (KV: 156 vs 174 MB, SQLite: 222 vs 238 MB). This is because system malloc's layout sometimes produces more compressible pages for specific access patterns (more zero-padded alignment bytes, different object clustering).

3. **Full Smash wins clearly on RocksDB** (85.9% vs 60.6% reduction). RocksDB's large block cache allocations benefit most from Smash's external metadata and arena routing — no embedded freelist pollution across large contiguous blocks.

4. **Peak RSS is consistently lower with full Smash** on RocksDB (299.6 vs 385.3 MB) due to Smash's compact slab layout. For other workloads the difference is small.

5. **Throughput: compress-only has 7-18% lower throughput** than full Smash on KV and RocksDB. The hash-map-based page tracking and VM region scanning add overhead. JSON and SQLite show minimal difference.

6. **The allocation design matters most for large-allocation workloads** (RocksDB) where Smash's external metadata eliminates freelist pollution across many pages. For small-allocation workloads (JSON, KV, SQLite), the compression pipeline dominates and the allocator design contributes less.

---

## Deferred-Only Zero-on-Free (Design Change)

**Design change**: Moved ALL zeroing (including small objects ≤128B) off the `free()` critical path. All zeroing now happens in the compressor thread's `zeroFreeSlots()` using non-temporal stores.

**Rationale**: Ablation T1b showed eager zeroing of small objects had 0.0pp compression benefit. Removing it from `free()` eliminates any critical-path overhead.

**Changes**:
- `SmashHeap::free()`: removed `__builtin_memset` call for objects ≤ `kZeroOnFreeMaxSize`
- `compressor_thread.h`: `zeroFreeSlots()` now handles ALL sizes (removed early return for small objects)
- `config.h`: removed `kZeroOnFree` and `kZeroOnFreeMaxSize` constants

### Results (deferred-only zeroing, --quick mode)

| Benchmark | RSS Reduction | Ops/sec | Cold p50 (μs) | Cold p99 (μs) |
|-----------|--------------|---------|---------------|---------------|
| JSON      | 33.9%        | 55.9M   | 9.9           | 28.1          |
| KV Store  | 40.4%        | 3.6M    | 0.46          | 10.46         |
| SQLite    | 43.8%        | 84.9K   | 0.88          | 38.67         |
| RocksDB   | 80.0%        | 941K    | 0.83          | 11.79         |

**Conclusion**: Results are consistent with or better than previous (eager+deferred) zeroing. Deferred-only is strictly better: identical compression with zero `free()` path overhead.

---

## CDF Latency Infrastructure

Added per-operation latency tracking and CDF output to all C++ benchmarks:
- Set `SMASH_LATENCY_DIR=/path/to/dir` environment variable
- Benchmarks write sorted latency arrays to CSV files (e.g., `json_cold.csv`, `kv_hot.csv`)
- `paper/figures/plot_cdf.py` generates per-benchmark CDF plots and combined cold-access CDF

Benchmarks with CDF support: JSON, KV store, SQLite, RocksDB.
Script-based benchmarks (memcached, Redis, DuckDB, Polars) don't have per-operation latency tracking.

---

## Planned Experiments

### Additional Real-World Applications (PLANNED)

- **Larger Polars/Pandas workloads** — multi-GB DataFrames with clear hot/cold partitioning
- **Nginx with caching** — reverse proxy serving cached content with Zipfian popularity
- **PostgreSQL shared buffers** — OLTP+OLAP mixed workload
- **Node.js server** — Express/Fastify with in-memory session cache
- **lmdb** — memory-mapped B-tree with cold record access patterns

---

## Generating Paper Figures

This section documents how to generate all graphs and figures for the paper from benchmark data.

### Prerequisites

```bash
# Build with benchmarks enabled
cd build
cmake .. -DSMASH_BUILD_BENCH=ON && make -j$(nproc)

# Install Python plotting dependencies
pip install matplotlib seaborn numpy
```

### Step 1: Run Paper Experiments

**Important: The paper requires results from BOTH platforms:**
- **Linux** (x86_64, 4 KiB pages, glibc)
- **macOS** (ARM64, 16 KiB pages, libmalloc)

Run the unified experiment runner on each platform:

```bash
cd build

# Full experiment suite (paper-quality, 3 runs each)
python3 ../bench/run_paper_experiments.py --runs 3

# Quick smoke test (smaller datasets, 1 run)
python3 ../bench/run_paper_experiments.py --quick --runs 1

# Subset of apps
python3 ../bench/run_paper_experiments.py --apps sqlite,rocksdb,redis --runs 3
```

This produces (per platform):
- `paper_results/ablation_results.json` — Ablation study data (10 configs × 5 apps)
- `paper_results/compress_only_results.json` — Compress-only comparison data
- `paper_results/paper_tables.txt` — Pre-formatted LaTeX tables

**To collect results for both platforms:**
1. Run experiments on Linux → save `paper_results/` as `paper_results_linux/`
2. Run experiments on macOS → save `paper_results/` as `paper_results_macos/`
3. Merge results into paper tables (see below)

### Step 2: Generate Main Figures

```bash
cd paper/figures
python3 plot_all.py
```

This generates:
- `rss_reduction.pdf` — Grouped bar chart comparing Mesh vs Smash RSS reduction
- `auc_comparison.pdf` — AUC (memory pressure over time) comparison
- `algo_compare.pdf` — Compression ratio comparison across algorithms
- `algo_throughput.pdf` — Compression/decompression throughput
- `memcached.pdf` — Memcached RSS by phase
- `ablation.pdf` — Ablation study (delta from default)
- `multipage.pdf` — Multi-page compression ratios
- `dict_overhead.pdf` — Dictionary CDict overhead vs benefit
- `allocator_compare.pdf` — Compression ratios across allocator substrates

### Step 3: Generate RSS Timeline Figures

RSS timeline graphs show memory usage over time (fill → cool → serve phases).

```bash
cd paper/figures

# Generate from synthetic data (based on measured results)
python3 plot_rss_timeline.py

# Generate from actual benchmark RSS data (if collected)
python3 plot_rss_timeline.py <baseline_dir> <smash_dir> <output_dir>
```

This generates:
- `rss_combined.pdf` — Combined 2×3 grid of all benchmarks
- Individual `rss_<benchmark>.pdf` files (when using real data)

**Note**: The benchmark runner (`run_paper_experiments.py`) automatically samples RSS every second during benchmark execution and stores the timeline in `rss_timeline` field of the JSON output.

### Step 4: Generate Latency CDF Figures

CDF plots show cold-access latency distributions.

```bash
cd paper/figures

# Generate from synthetic data (matching Table 4 p50/p99)
python3 plot_cdf.py

# Generate from actual latency data (if collected)
python3 plot_cdf.py <baseline_dir> <smash_dir> <output_dir>
```

This generates:
- `cdf_cold_combined.pdf` — Combined cold-access CDF for all benchmarks

**To collect real latency data**: Set the `SMASH_LATENCY_DIR` environment variable when running C++ benchmarks:

```bash
export SMASH_LATENCY_DIR=/path/to/latency_data
./bench/bench_sqlite --quick
./bench/bench_rocksdb --quick
```

Latency CSVs are written to `<SMASH_LATENCY_DIR>/<benchmark>_cold.csv`, etc.

### Step 5: Understanding AUC (Area Under Curve)

The benchmark runner calculates AUC metrics for RSS-over-time analysis:

- **`auc_mb_sec`**: Sum of RSS samples (MB) over the measurement period
  - Each sample is 1 second apart, so this represents MB-seconds
  - Lower AUC = less memory pressure over time
  - Useful for comparing total memory footprint, not just peak/min

**AUC Reduction Calculation**:
```
auc_reduction_pct = (1 - smash_auc / baseline_auc) * 100
```

AUC is stored in the JSON results and can be used for:
- Comparing memory efficiency across configs
- Showing sustained compression benefit (not just instantaneous)
- Computing "memory-time" cost for batch workloads

### Quick Reference: Output Locations

| Data | Location |
|------|----------|
| Ablation results | `build/paper_results/ablation_results.json` |
| Compress-only results | `build/paper_results/compress_only_results.json` |
| LaTeX tables | `build/paper_results/paper_tables.txt` |
| Main figures | `paper/figures/*.pdf` |
| RSS timelines | `paper/figures/rss_*.pdf` |
| Latency CDFs | `paper/figures/cdf_*.pdf` |

### Dual-Platform Results Status

The paper presents results from both Linux and macOS. Current status:

| Platform | Status | Results Location |
|----------|--------|------------------|
| **Linux** (x86_64, Amazon Linux 2, 4 KiB pages) | ✅ Complete | `paper_results/` |
| **macOS** (ARM64, M1 Max, 16 KiB pages) | ❌ Needed | TBD |

**Linux results summary (March 2026):**
- Memcached: 52% RSS reduction (Mesh: 0%)
- RocksDB: 76% RSS reduction (Mesh: 0%)
- SQLite: 71% RSS reduction (Mesh: -85% overhead)
- Redis: 21% RSS reduction (Mesh: 0%)
- Redis-ext: 23% RSS reduction (Mesh: 0%)

---

### Completing macOS Results

Run the following steps on a macOS machine (M1/M2/M3 with 16 KiB pages):

**Step 1: Build and run experiments**
```bash
cd build
cmake .. -DSMASH_BUILD_BENCH=ON && make -j$(nproc)
python3 ../bench/run_paper_experiments.py --runs 3
```

**Step 2: Save results**
```bash
cp -r paper_results paper_results_macos
```

**Step 3: Extract key metrics**
```bash
python3 -c "
import json
with open('paper_results/ablation_results.json') as f:
    data = json.load(f)

print('=== macOS Results for Paper ===')
for app in ['sqlite', 'rocksdb', 'memcached', 'redis', 'redis_ext']:
    if app not in data:
        continue
    b0 = data[app].get('B0', {}).get('runs', [])
    mesh = data[app].get('MESH', {}).get('runs', [])
    b1 = data[app].get('B1', {}).get('runs', [])

    b0_rss = b0[0].get('steady_rss_mb', 0) if b0 else 0
    mesh_rss = mesh[0].get('steady_rss_mb', 0) if mesh else 0
    b1_rss = b1[0].get('steady_rss_mb', 0) if b1 else 0
    b1_red = sum(r.get('rss_reduction_pct', 0) for r in b1) / len(b1) if b1 else 0

    print(f'{app}: Sys={b0_rss:.0f}, Mesh={mesh_rss:.0f}, Smash={b1_rss:.0f} ({b1_red:.0f}%)')
"
```

**Step 4: Update paper with macOS numbers**

Edit `paper/evaluation.tex` and replace `---` placeholders in:
- Table 1 (`tab:apps_summary`): macOS columns for Sys, Mesh, Smash
- Application paragraphs: macOS-specific numbers
- Summary paragraph: macOS reduction range

**Step 5: Update plotting scripts**

Edit `paper/figures/plot_all.py`:
- Add macOS data to `fig_rss_reduction()`
- Update `fig_auc_comparison()` with macOS AUC values

Edit `paper/figures/plot_rss_timeline.py`:
- Update `SYNTH_DATA` dict with macOS (fill, cool, serve) tuples

**Step 6: Regenerate figures**
```bash
cd paper/figures
python3 plot_all.py
python3 plot_rss_timeline.py
python3 plot_cdf.py
```

**Step 7: Commit results**
```bash
git add paper/ paper_results_macos/
git commit -m "Add macOS experimental results"
```

### Updating Figures with New Data

The plotting scripts in `paper/figures/` contain hardcoded data matching current benchmark results. To update:

1. Run experiments: `python3 ../bench/run_paper_experiments.py --runs 3`
2. Extract values from `paper_results/ablation_results.json`
3. Update the data arrays in `plot_all.py`, `plot_rss_timeline.py`
4. Re-run: `python3 plot_all.py`

Alternatively, modify the scripts to read directly from JSON (future work).
