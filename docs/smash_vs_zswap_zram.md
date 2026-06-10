# Smash vs. zswap vs. zram

## What Smash actually is

Unlike zswap and zram, Smash is **not a kernel feature and not a swap backend**. It's an LD_PRELOAD-able userspace allocator (built on alloc8) that:

- Interposes on `malloc`/`free`/`new`/`delete` and routes large allocations through its own `MAP_ANON` arena.
- Tracks per-page coldness — by default via `mprotect(PROT_READ)` plus a SIGSEGV/SIGBUS handler that catches writes; optionally via Linux soft-dirty (`SMASH_SOFTDIRTY=1`), which is faster and lighter (see "Soft-dirty in Smash" below).
- After a page goes untouched for `SMASH_COLD_TIMEOUT_SEC`, the compressor thread evaluates ROI, picks LZ4 or zstd, and stores the compressed blob in a sharded buffer.
- On access, the SIGSEGV/SIGBUS handler decompresses synchronously and restores PROT_RW.
- Per-bucket profile (arena × size class) drives algorithm choice and skips uncompressible buckets.

The interesting comparison isn't "Smash vs. swap"; it's **where each layer sits and what each can see**.

## Where each lives in the stack

| Layer | Granularity | Triggered by | Visibility | Spill path |
|------|------|------|------|------|
| **zswap** | Page (kernel LRU) | Memory pressure (`kswapd`) | All anon pages, system-wide | Compressed-then-disk swap |
| **zram (as swap)** | Page (kernel LRU) | Memory pressure (`kswapd`) | All anon pages, system-wide | None — hard cap |
| **Smash** | Page within app's heap arena | Wall-clock idle time per page | Only the app it's preloaded into | None — process must hold the working set in RAM |

## Where Smash wins over zswap/zram

- **No swapfile, no root, no kernel config.** A team can `LD_PRELOAD=libsmash.so PYTHONMALLOC=malloc SMASH_LARGE_ONLY=1 ./neuron-cc` on a shared build host without touching `/proc/sys` or asking SRE to enable zswap.
- **Application-aware coldness.** Smash compresses based on per-page idle time, not on global memory pressure. Pages can be compressed **while there's still tons of free RAM** — useful for build hosts where you want to fit more concurrent compilations on one box even though no individual job is under pressure.
- **Per-(call site, size class) bucketing.** zswap/zram compress at the page level with one algorithm; Smash routes pages from the same call site into the same arena so similar data ends up adjacent (large ratio improvement on JSON/IR/zero-padded structures), and learns per-bucket which buckets aren't worth compressing.
- **Skip-uncompressible-bucket heuristic.** zswap stores the page anyway and wastes the cycles; Smash declines and leaves the page hot.
- **Dictionary training (zstd).** Per-bucket dictionaries can lift ratios well beyond what zswap's stateless per-page compression achieves on highly templated data.

## Where zswap wins (decisively, for general use)

- **System-wide.** All processes benefit from one config; Smash has to be preloaded per process and runs an extra compressor thread inside each.
- **Disk fallback.** zswap spills to actual swap when RAM gets tight — Smash cannot. If your dataset is bigger than RAM, Smash OOMs as helplessly as plain malloc.
- **Kernel LRU is the right oracle for global memory pressure.** "What's coldest in the system right now?" is genuinely a kernel-level question. Smash's per-process wall-clock idle time is a coarse proxy and wrong under load (a page can be 5s idle in a process that itself is just descheduled, and shouldn't be compressed).
- **Mature.** Integrates with cgroup memory accounting, PSI, OOM heuristics. Smash predates none of that machinery.

## Where zram-as-swap is mostly worse than zswap

The Chris Down article (https://chrisdown.name/2026/03/24/zswap-vs-zram-when-to-use-what.html) is the right read on this:

- zram-swap is a hard-capped compressed RAM block device with no spillover. Once it's full you're back to OOM or LRU inversion (the kernel sees zram-swap as "fast swap" and may swap pages there *instead of* evicting genuinely cold pages out to disk swap, so the actually-cold stuff stays in RAM uncompressed while warmer-but-compressible stuff sits compressed in zram).
- Upstream is moving away from it.
- The remaining good fits are the ones the article lists: no-disk embedded systems, security-sensitive setups that can't write to disk, very memory-constrained boxes where giving up disk-swap-fallback is acceptable.

## Can they play together?

**zswap + Smash: yes, additive and useful.** Different layers, different triggers:

- Smash compresses **app-internal cold pages** while plenty of RAM is free, based on app-level idleness. The page never enters the kernel's LRU because the app keeps the VM mapping (just protected). zswap/swap sees normal RW pages.
- When system pressure hits, zswap kicks in on whatever's still hot in the working set. Smash-decompressed pages enter zswap's pool just like any other anon page; Smash-compressed pages are PROT_NONE and won't be picked for swap-out (they look like reserved-but-unused memory).
- The two compressors don't fight: Smash compressed it to zstd-9, the kernel never sees that buffer.

The realistic risk is **double-compression of bookkeeping** — Smash's own metadata arena (BootstrapAlloc) is plain RW memory and is a candidate for zswap if it goes cold. That's fine, just slightly wasteful; zswap will skip pages whose ratio is poor (LZ4 default).

**zram-swap + Smash: pointless.** Both compress in RAM with no spill path. Stacking them just means the same byte gets compressed twice (Smash compresses the cold page; the page never enters zram because Smash holds the mapping; if it does, zram compresses an already-compressed buffer to roughly the same size). Use zswap if you want a swap-side companion to Smash.

## Where Smash actually slots in

Best case for Smash specifically:

- **Long-running build/compile workloads on shared hosts** (neuron-cc, big C++ builds) where you can't touch kernel config but want to land more concurrent jobs.
- **Workloads with structurally homogeneous data** (graph IRs, model weights, JSON-ish protocol buffers) where call-site segregation + dict training beats per-page LZ4.
- **Diagnostic / opt-in deployment**: `LD_PRELOAD=libsmash.so` is gradual; zswap is a host-wide commitment.

Worst case:

- Memory-pressure-bound workloads with hot working sets — Smash's wall-clock cold timeout will mis-classify and thrash.
- Large heaps with random access (graph DBs) — page faults to decompress on every access kill latency.
- Anything where the dataset exceeds RAM — Smash has nowhere to spill, zswap does.

## Soft-dirty in Smash (verified 2026-05-31)

The user is right: Linux's soft-dirty PTE-bit tracking (https://docs.kernel.org/admin-guide/mm/soft-dirty.html) is implemented in Smash but is **off by default**. It's gated behind `SMASH_SOFTDIRTY=1` in `src/compress/compressor_thread.h:2421`. When enabled, it fully replaces Phase 3 (`compressor_thread.h:2930` skips the `dispatch(3)` mprotect storm).

### Why soft-dirty is faster than the default PROT_READ scheme

The default Phase 3 walks every ACTIVE page and calls `mprotect(PROT_READ)` on it. Each write to a PROT_READ page raises SIGSEGV; the fault handler restores PROT_RW for *that one page* (= one mprotect per write). Costs:

| Cost | PROT_READ default | Soft-dirty |
|------|------|------|
| Per page per tick | One `mprotect` over the whole region | Zero |
| On write | Kernel fault → SIGSEGV → user handler → `mprotect(PROT_RW)` for one page | Kernel sets PTE bit 55, no fault |
| VMA fragmentation | Each per-page `mprotect(PROT_RW)` splits the VMA, eventually hitting `vm.max_map_count = 65530` | None — soft-dirty is a PTE attribute |
| Tick-end work | None (state stays in PROT_READ until reset by next fault) | One `write` to `/proc/self/clear_refs` (kernel zaps PTEs once for the whole process) |
| Tick-start work | None | `pread` on `/proc/self/pagemap` — 8 bytes per page, sequential, kernel streams from the page-table walk |

### Measured

A microbenchmark (`bench/bench_softdirty_vs_protread.cpp`) compares per-tick cost on a single contiguous region. Three schemes:

- **(A) soft-dirty**: `pread` of pagemap + `write` clear_refs.
- **(B) PROT_READ best case**: one big `mprotect(PROT_READ)` per tick, no faults charged. This is the *theoretical* mprotect floor — it ignores the cost the SIGSEGV handler pays per write, which is the real Smash cost.
- **(C) PROT_READ + per-page faults**: one `mprotect(PROT_READ)` over the region, then realistic write density triggers per-page mprotects through the fault handler. **This is what Smash actually pays in production.**

Run on Linux x86_64 in the neuron-cc build container, AMD ICX-class CPU:

| Workload | (A) soft-dirty | (B) mprotect best-case | (C) mprotect + per-page faults | A vs C | VMAs added |
|---|---|---|---|---|---|
| 100K pages, 0.1% writes | 3.4 ms/tick | 0.5 ms/tick | 0.9 ms/tick | 0.27× (mprotect wins) | 200 |
| 100K pages, 2% writes   | 2.9 ms/tick | 0.5 ms/tick | 19 ms/tick | **6.7×** | 4 000 |
| 1M pages, 0.1% writes   | 28 ms/tick  | 5.3 ms/tick | 9.8 ms/tick | 0.35× | 2 000 |
| 1M pages, 1% writes     | 52 ms/tick  | 5.6 ms/tick | 102 ms/tick | **2.0×** | 20 000 |
| 250K pages, 5% writes   | 10 ms/tick  | 1.5 ms/tick | 119 ms/tick | **11.4×** | 25 000 |

The crossover sits around **0.5–1% write density**: below that the mprotect path is cheap enough that pagemap-read overhead loses; above it, the per-page fault storm dominates and soft-dirty wins by 2–11×. Critically, the VMA count grows ~linearly with writes — at 5% density on 250K pages we already added 25K VMAs in a single tick. Long-running compilation hits `vm.max_map_count = 65530` quickly under the default scheme, while soft-dirty stays at one VMA forever.

So the user's instinct was right *for the workload that matters* (real Smash with real fault traffic). It's not unconditionally faster — for tiny workloads with very sparse writes the mprotect path is genuinely cheaper because pagemap read scales with region size, not with write density.

### Trade-offs to know about

1. **Phase 3 currently does double duty.** Besides write tracking, the default Phase 3 mprotect-to-PROT_READ also enables the snapshot path used by `FixAv` verify-then-flip in the compress phase (`compressor_thread.h:133-135`). With soft-dirty on, the read snapshot has to come from elsewhere; Smash's tick orders Phase 1 / soft-dirty-read **before** Phase 2 (`compressor_thread.h:2876-2886`) so the dirty bits are read into `accessed_[]` before compression decides what to compress.
2. **`/proc/self/clear_refs` is process-wide.** Writing `4` clears soft-dirty for *all* anonymous PTEs in the process, not just Smash's region. If the host process has its own soft-dirty consumer (rare in production but real for sandboxes / tracers like CRIU), Smash will clobber it.
3. **`/proc/self/pagemap` requires CAP_SYS_ADMIN on hardened kernels** (containerised environments with strict `unprivileged_userfaultfd`/pagemap policies). Smash's `ensureSoftDirtyFds()` (`compressor_thread.h:2431`) currently falls back silently if the open fails — worth surfacing as a warning before rolling soft-dirty as the default.
4. **No native macOS equivalent.** Soft-dirty is Linux-only.
5. **Per-tick cost grows with region size, not with work.** The pagemap read is O(committed pages), not O(active pages). Workloads that legitimately have 100K committed but 99% truly cold pages pay the soft-dirty read cost on every tick whereas the mprotect path pays nothing once everything's in PROT_READ steady state. This is a real argument for keeping the mprotect path available.

### Recommendation

Flip the default on Linux to `SMASH_SOFTDIRTY=1` when `/proc/self/{pagemap,clear_refs}` are openable; keep PROT_READ Phase 3 as the explicit fallback (`SMASH_SOFTDIRTY=0`). The crossover analysis above says soft-dirty wins for the workloads that hurt Smash today (large heaps, dense writes, VMA-fragmentation-bound) and loses only for tiny / very-cold workloads where the absolute cost is small either way. The VMA-explosion failure mode alone (the source comments at `compressor_thread.h:2407` already flag the `vm.max_map_count` hazard) is reason enough.
