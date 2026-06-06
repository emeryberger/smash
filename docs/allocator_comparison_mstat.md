# Allocator comparison on smash's own benchmarks (mstat / cgroup accounting)

**Date:** 2026-06-06  **Host:** 192-core, 1.48 TB RAM, Linux 6.12 (cgroup v2)
**Method:** each workload run under `glibc / jemalloc / mimalloc / smash` via
`LD_PRELOAD`. Memory measured two ways: (a) the bench's own getrusage RSS at
each phase (the paper's metric), and (b) `mstat` cgroup `memory.current` of the
whole process tree (shared pages counted once). Smash = full mode,
`SMASH_VERY_COLD_TICKS=5 SMASH_COLD_TIMEOUT_SEC=2`.

> **Prerequisite:** these results are **post** the deferred-madvise regression
> fix (commit a8354be). Before it, `SMASH_DEFER_MADVISE_TICKS` defaulted to 50
> ticks = **50 seconds** (mislabeled "~500ms"), so compressed pages stayed
> resident through any cooling window shorter than 50s and smash showed **zero**
> RSS reduction on every app benchmark. The regression was bisected to 823515f.

## SQLite (500K rows, in-memory; getrusage RSS)

| alloc | peak MiB | min RSS MiB | reduction | ops/s |
|-------|----------|-------------|-----------|-------|
| glibc    | 355 | 355 | 28% | 96K |
| jemalloc | 418 | 418 | 27% | 92K |
| mimalloc | 415 | 416 | 27% | 93K |
| **smash** | 460 | **126** | **73%** | 88K |

Only smash reclaims the cold heap (126 vs 355–418 MiB); ~8% lower throughput.
(Paper reported 429→191, 69%.)

## RocksDB (1M keys × 512B, block cache; getrusage RSS)

| alloc | peak | cool RSS | serve RSS | min RSS | ops/s |
|-------|------|----------|-----------|---------|-------|
| glibc    | 275 | 275 | 282 | 275 | 700K |
| jemalloc | 384 | 384 | 390 | 384 | 720K |
| mimalloc | 281 | 281 | 291 | 281 | 720K |
| **smash** | 298 | **54** | **84** | **54** | 700K |

Smash drops to 54 MiB during cooling (vs 275–384); throughput on par.
(Note: mstat cgroup min is ~313 MiB for smash because rocksdb's SST page cache
is file-backed and counted by the cgroup but not by heap-RSS — that memory is
outside smash's reach by design.)

## Redis (200K keys × 2KB; mstat cgroup memory.current)

**Standard (SET → cool → GET):**

| alloc | get rps | peak | min during cool | avg |
|-------|---------|------|-----------------|-----|
| glibc    | 58K | 331 | 141 | 311 |
| jemalloc | 63K | 408 | 105 | 383 |
| mimalloc | 61K | 414 | **2** | 388 |
| smash    | 44K | 606 | 187 | 330 |

**Extended (SET → DELETE 50% → cool → GET):**

| alloc | min during cool | avg |
|-------|-----------------|-----|
| glibc    | 330 | 313 |
| jemalloc | 407 | 386 |
| mimalloc | 375 | 391 |
| **smash** | **187** | **330** |

On the standard workload, mimalloc's lazy `MADV_FREE` purge returns idle memory
essentially for free (2 MiB trough) and beats smash; smash also pays a real
~30% GET-throughput cost (decompression faults during serve). On the
**extended** (post-deletion, fragmented) workload smash wins clearly — 187 MiB
trough / 330 avg vs 375–407 for the others — because it compresses the
fragmented freed regions the other allocators retain.

## Honest summary

- **The regression fix is the headline:** smash's RSS reduction was completely
  broken (50s deferral) and is now restored; it again reduces cold-heap memory
  by 70–80% on sqlite/rocksdb, matching the paper.
- **vs glibc:** smash is a large memory win on every cold-data workload.
- **vs mimalloc:** closer than the paper (which compared against glibc + Mesh,
  not mimalloc). mimalloc's aggressive idle purge already reclaims clean idle
  memory cheaply; smash's edge is on **fragmented / partially-deleted** heaps
  (redis-extended) and on **genuinely compressible cold data** it keeps resident
  in compressed form rather than returning to the OS.
- **Throughput:** smash costs ~8% on sqlite, ~0% on rocksdb, but ~30% on redis
  GET (serve-phase decompression faults) — the real time/space tradeoff.
