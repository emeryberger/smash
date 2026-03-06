# Smash: Technical Overview

Smash is a compression-aware userspace memory allocator. It transparently compresses cold pages to reduce resident set size (RSS), decompressing on demand when the application touches them again. It replaces `malloc`/`free` via dynamic interposition and is designed to be a drop-in replacement for any C/C++ program.

## Thesis

OS-level memory compression (zswap, zram, macOS compressed memory) treats all pages identically. Smash does better by exploiting application-level structure:

1. **Page homogeneity.** Each span contains objects of a single size class. Pages within a span hold similar data patterns, which compresses far better than pages containing a mix of object sizes and types.

2. **Metadata/data separation.** Allocation metadata (bitmaps, span descriptors, page tables, thread cache pointer arrays) lives in dedicated bootstrap memory, never mixed with user data. Data pages are pure user content and compress cleanly.

3. **Call-site locality.** Allocations from the same call site are routed to the same arena. Objects allocated together tend to have similar structure, further improving intra-page homogeneity.

4. **Selective compression.** Only cold pages are compressed. The hot working set runs at native speed with zero overhead. Per-size-class dictionaries exploit the homogeneity that the allocator creates.

## Architecture

```
┌─────────────────────────────────────────────────┐
│  alloc8 interposition layer                     │
│  (LD_PRELOAD / DYLD_INSERT_LIBRARIES)           │
├─────────────────────────────────────────────────┤
│  SmashHeap                                      │
│  malloc / free / memalign / getSize             │
├──────────────┬──────────────────────────────────┤
│ Thread Cache │ Slab[4 arenas × 36 classes]      │
│ (per-thread, │  ┌─ partial spans (bitmap alloc) │
│  per-class)  │  ├─ full spans                   │
│              │  └─ empty spans                  │
├──────────────┴──────────────────────────────────┤
│ PageMap (two-level radix tree: addr → Span*)    │
├─────────────────────────────────────────────────┤
│ VmRegion (~16 GB contiguous virtual reservation)│
│ PageStateTable (ACTIVE/MONITORING/COMPRESSED)   │
├──────────────────┬──────────────────────────────┤
│  FaultHandler    │  CompressorThread             │
│  (SIGSEGV/SIGBUS│  (parallel workers, chunk      │
│   signal handler)│   bitmap, adaptive algo)       │
├──────────────────┴──────────────────────────────┤
│  CompressStore (sharded, own mmap regions)       │
│  BootstrapAlloc (bump allocator for all metadata)│
└─────────────────────────────────────────────────┘
```

All internal data structures (spans, bitmaps, page map tables, thread caches, compression contexts, per-page state arrays) are allocated from `BootstrapAlloc`, a bump allocator backed by `mmap`. This avoids re-entrant calls to the managed heap, which is critical because the signal-based fault handler runs in contexts where `malloc` cannot be called.

---

## 1. Core Allocator

### 1.1 Size Classes

36 size classes with sub-linear spacing cover allocations from 16 bytes to 16,384 bytes:

| Range | Step | Classes |
|-------|------|---------|
| 16 -- 128 | 16 | 8 |
| 160 -- 256 | 32 | 4 |
| 320 -- 512 | 64 | 4 |
| 640 -- 1024 | 128 | 4 |
| 1280 -- 2048 | 256 | 4 |
| 2560 -- 4096 | 512 | 4 |
| 5120 -- 8192 | 1024 | 4 |
| 10240 -- 16384 | 2048 | 4 |

`sizeToClass()` uses a 65-entry direct lookup table for sizes up to 1024 bytes (single indexed load), and a log2 computation for larger sizes. All sizes are multiples of 16 (`kMinAlignment`).

### 1.2 Spans

A span is a contiguous run of pages dedicated to one size class. The `Span` descriptor is allocated from `BootstrapAlloc` and contains:

- `base`: start of the data pages
- `bitmap`: one bit per object slot (1 = free, 0 = allocated), also from bootstrap memory
- `object_size`, `object_count`, `allocated_count`: bookkeeping
- `arena_id`: which arena this span belongs to
- `free_hint_word`: accelerates bitmap scanning by remembering the last word with a free bit

**Bitmap-based free tracking** is the critical design decision. Traditional allocators use inline freelists (writing next-pointers into freed objects), which contaminates data pages with metadata. Bitmaps keep data pages pure, costing only ~1 bit per slot in bootstrap memory and a `ctz` instruction per allocation.

### 1.3 Slab Allocator

Each `Slab` manages spans of one size class within one arena. It maintains three intrusive lists (partial, full, empty) and a per-slab spinlock. The allocation path takes the partial list first, falls back to empty/new spans:

```
allocate():   partial → pop slot → promote to full if needed
deallocate(): if was_full → move to partial; if now empty → move to empty
scavenge():   release empty spans back to VmRegion
```

Batch allocation (`allocateBatch`) and deallocation (`deallocateBatch`) amortize lock acquisition for thread cache refill/drain operations.

### 1.4 Arena-Based Allocation

Smash uses 4 arenas (configurable, must be power of 2). The total slab array is `Slab[4 arenas * 36 classes] = 144 slabs`, stored as a flat array indexed by `arena * kNumClasses + sc`.

Arena selection uses the call site address:

```cpp
static uint8_t callsiteArena() {
    uintptr_t ra = __builtin_return_address(1);
    ra ^= ra >> 16;
    return ra & (kNumArenas - 1);
}
```

This routes allocations from the same call site to the same arena. Pages within an arena contain data from similar call sites, producing more uniform content that compresses better. As a side effect, arenas also reduce lock contention: threads allocating from different call sites hit different slab locks.

### 1.5 Thread Cache

Each thread has a per-size-class cache holding up to 64 pointers (`kThreadCacheMaxPerClass`). The cache stores pointers in an array, not an inline freelist, preserving data page purity.

- **Allocate**: pop from cache; on miss, `refill()` batch-allocates 32 objects from the appropriate arena's slab.
- **Free**: push to cache; on full, `drain()` returns half the entries to their origin slabs.

`drain()` must route pointers back to the correct arena's slab. It looks up each pointer's span via the page map to get `arena_id`, buckets pointers by arena, and calls each arena's `deallocateBatch()`:

```cpp
void drain(uint8_t sc, Slab* all_slabs, PageMap* page_map) {
    void* buckets[kNumArenas][kThreadCacheBatchSize];
    size_t counts[kNumArenas]{};
    // Bucket by arena_id from span lookup
    for (each pointer) {
        Span* span = page_map->get(ptr);
        uint8_t arena = span->arena_id;
        buckets[arena][counts[arena]++] = ptr;
    }
    for (each arena with count > 0)
        all_slabs[arena * kNumClasses + sc].deallocateBatch(...);
}
```

Thread caches are pooled (`ThreadCachePool`) and recycled across thread lifetimes, all allocated from bootstrap memory.

### 1.6 Large Allocations

Allocations larger than `kMaxSmallSize` (16 KB) go through `LargeAlloc`. If compression is enabled and the allocation is at least `kLargeAllocVmThreshold` (1 MB), it is placed in the VmRegion so the compressor can track it. Smaller "large" allocations (16 KB -- 1 MB) use direct `mmap` because they tend to be frequently-accessed internal buffers that would cause decompression storms.

### 1.7 Page Map

A two-level radix tree maps any virtual address to its owning `Span*`. Assuming a 48-bit address space:

- Level 1: `2^(48 - 14 - 16) = 2^18 = 256K` entries, allocated eagerly from bootstrap (~2 MB)
- Level 2: `2^16 = 64K` entries each, allocated lazily on first touch

Lookup is O(1): two atomic pointer dereferences. No locks on the read path.

---

## 2. Virtual Memory Management

### 2.1 VmRegion

All compressible data pages come from a single contiguous virtual memory reservation (`kVmRegionSize` = ~16 GB on systems with 16 KB pages). On macOS this is a plain `mmap`; on Linux it would use `MAP_NORESERVE`.

Pages are committed on demand (`mmap` + `PROT_READ|PROT_WRITE`) and decommitted after compression (`madvise(MADV_FREE_REUSABLE)` on macOS). The bump pointer (`next_page_`) is an atomic `fetch_add` for lock-free allocation. Released pages go to a spinlock-protected free list for reuse.

A single contiguous region simplifies all address checks to a bounds test (`addr >= base && addr < base + total_pages * kPageSize`) and makes page index computation trivial (`(addr - base) / kPageSize`).

### 2.2 Page State Machine

Every page in the VmRegion has an `atomic<uint8_t>` state. Transitions use CAS for safe coordination between the compressor thread, fault handler, and application threads:

```
EMPTY ──commit──→ ACTIVE ──monitor──→ ACTIVE_MONITORING
                    ↑                        │
                    │                   compress tick
                    │                        ↓
                    │                   COMPRESSING
                    │                        │
                    │                   store blob
                    │                        ↓
                    └───── fault ────── COMPRESSED
```

- **EMPTY**: Virtual reservation only, no physical backing.
- **ACTIVE**: Committed, `PROT_READ|PROT_WRITE`. Normal operation.
- **ACTIVE_MONITORING**: `PROT_READ` only. Write faults set the accessed flag and restore `PROT_READ|PROT_WRITE`.
- **COMPRESSING**: Compressor thread is actively working on this page. Faults during this state restore the page immediately.
- **COMPRESSED**: Data stored in `CompressStore`, physical backing decommitted, page is `PROT_NONE`. Any access triggers decompression.

Per-page spinlocks (`PageLockTable`) serialize state transitions. The compressor uses `lock()` during compression; the fault handler uses `lock()` during decompression; prefetch uses `tryLock()` to avoid deadlock.

---

## 3. Compression System

### 3.1 Compressor Thread

A background coordinator thread wakes every `kCompressIntervalMs` (1000 ms) and runs three phases across the VmRegion:

**Phase 1 -- Access Tracking.** For each ACTIVE or ACTIVE_MONITORING page, check the `accessed_[]` flag (set by the fault handler on write-fault). If accessed, reset cold count to zero. If not, increment cold count (up to 255).

**Phase 2 -- Compression.** For each page with `cold_count >= kColdTicks` (2), attempt compression. The per-page compression sequence:

1. Acquire per-page lock.
2. Check adaptive skip (sliding-window stats for this size class).
3. Set state to COMPRESSING.
4. `mprotect(PROT_READ)` -- get consistent snapshot.
5. `memcpy` page into scratch buffer.
6. Zero freed slots in the scratch buffer (deferred zero-on-free).
7. `mprotect(PROT_NONE)` -- make inaccessible.
8. Compress scratch buffer (algorithm depends on cold duration).
9. If ratio < 75%: store compressed blob, decommit physical backing, set state to COMPRESSED.
10. Otherwise: restore page, set state to ACTIVE.

**Phase 3 -- Monitoring.** Set remaining ACTIVE pages (not pinned by syscalls) to `PROT_READ` and state ACTIVE_MONITORING. This enables write-fault access tracking for the next tick. Contiguous pages are batched into a single `mprotect` call.

### 3.2 Parallel Workers

The coordinator thread partitions the page range across `kCompressorWorkers` (2) worker threads. Each worker has its own:

- LZ4 compression state
- ZSTD CCtx (compression context)
- Page scratch buffer (one page)
- Compression output buffer
- Per-size-class `SizeClassStats` ring buffers

All three phases are embarrassingly parallel across disjoint page ranges. Per-page locks protect individual state transitions. The coordinator dispatches work via a generation counter; helpers spin-wait for new generations and signal completion via per-helper atomic counters.

A **chunk bitmap** (`live_chunks_[]`) accelerates scanning. Each bit represents one page in a 64-page chunk; the bitmap is rebuilt at tick start. Iteration uses `__builtin_ctzll` to jump directly to set bits, reducing scan cost from O(committed_pages) to O(active_pages).

### 3.3 Compression Algorithms

Three algorithms, selected based on cold duration:

| Cold Duration | Algorithm | Typical Speed | Use Case |
|--------------|-----------|---------------|----------|
| 2+ ticks (2s) | LZ4 | ~5 GB/s decompress | Recently cold, may be accessed again soon |
| 60+ ticks (1min) | zstd level 3--9 | ~1 GB/s decompress | Long-cold, worth spending time for ratio |
| 60+ ticks + dict | zstd + dictionary | ~1 GB/s + better ratio | Size class has trained dictionary |

Algorithm is encoded in the top 2 bits of `CompressedPageInfo::comp_size`, leaving 30 bits for the compressed size (sufficient for pages up to 16 KB).

All compression and decompression contexts are pre-allocated from bootstrap memory using `ZSTD_customMem` with callbacks that route to `BootstrapAlloc`. This is critical: decompression occurs in signal handler context where `malloc` cannot be called.

### 3.4 Sliding-Window Compression Stats

Per-size-class `SizeClassStats` tracks the last 64 compression ratios (0--255 scale) in a ring buffer. `shouldSkip()` returns true when enough samples exist (>= 32) and the average ratio is below 5%. This prevents the compressor from wasting cycles on incompressible size classes while allowing recovery when data patterns change.

### 3.5 Zero-on-Free

Freed object slots are zeroed to improve compression ratios. Zero runs compress extremely well under LZ4 (approaching 100:1 for pages of mostly-freed slots).

Two strategies are used depending on object size:

- **Eager zeroing (objects <= 128 bytes):** `memset(ptr, 0, object_size)` in `free()`, immediately after span lookup. The memory is cache-hot from the caller's last write, so the zeroing is nearly free.

- **Deferred zeroing (objects > 128 bytes):** The compressor's `zeroFreeSlots()` zeros free slots in the scratch buffer just before compression. It walks the span's bitmap to identify free slots overlapping the page, then zeros them using `ntZeroMemory()` -- non-temporal stores via `__builtin_nontemporal_store` that emit ARM64 `stnp` instructions, avoiding cache pollution for cold data.

The 128-byte threshold was chosen empirically: unconditional zeroing of large objects (e.g., 16 KB) caused a 7000% regression in `free()` latency. The threshold + deferred approach eliminates the regression while achieving equivalent compression improvement.

### 3.6 Dictionary Training

When `kDictTrainSamples` (16) pages of the same size class have been compressed, the coordinator trains a zstd dictionary from those samples. Workers atomically claim sample slots during compression; the coordinator calls `ZDICT_trainFromBuffer` after each tick if enough samples have accumulated.

Dictionaries exploit the structural regularity within a size class: objects of the same size from the same arena tend to share common byte patterns (headers, vtable pointers, zero-padded fields). A trained dictionary can improve compression ratio by 20--40% on homogeneous pages.

Pre-built `ZSTD_CDict` and `ZSTD_DDict` objects are shared across workers (CDicts are thread-safe for compression; DDicts are used with per-slot DCtxes for decompression).

### 3.7 CompressStore

Compressed blobs are stored in `CompressStore`, a bucket-based allocator backed by its own `mmap` regions (separate from `VmRegion` to avoid recursive compression). Bucket sizes are powers of 2 from 64 bytes to 16,384 bytes.

The store is **sharded** 8 ways by `page_idx % kCompressStoreShards`. Each shard has independent free lists, bump regions, and a spinlock. This eliminates the single-lock bottleneck that would otherwise serialize all compression and decompression operations.

---

## 4. Fault Handling

### 4.1 Signal Handler

On macOS and Linux, Smash installs a `SIGSEGV`/`SIGBUS` signal handler with `SA_SIGINFO | SA_NODEFER`. When an application thread accesses a protected page:

1. Signal fires with faulting address in `siginfo_t::si_addr`.
2. Handler calls `CompressorThread::handleFault(addr)`.
3. Based on page state:
   - **COMPRESSED**: Decompress blob into a fault slot buffer, commit the physical page, copy data back, release the compressed blob, set state to ACTIVE, prefetch adjacent pages.
   - **COMPRESSING**: The compressor is mid-compression on this page. Commit physical backing (the compressor already has a copy in its scratch buffer), set state to ACTIVE.
   - **ACTIVE_MONITORING**: Write fault during access tracking. Restore `PROT_READ|PROT_WRITE`, set accessed flag, set state to ACTIVE.
   - **ACTIVE**: Race condition -- Phase 3's batched `mprotect(PROT_READ)` can overwrite a per-page `PROT_RW` restoration. Simply restore `PROT_READ|PROT_WRITE`.
4. Handler returns. Execution resumes at the faulting instruction.

If the address is not in VmRegion, the handler chains to the previous signal handler.

The handler periodically checks (`ensureInstalled()` on each tick) whether another library has overwritten the signal handler, and re-installs itself if needed, chaining to the new handler.

### 4.2 Per-Fault-Slot DCtx

Decompression requires a `ZSTD_DCtx`. Multiple threads can fault simultaneously, and the compressor's prefetch also decompresses. To avoid data races, Smash maintains 32 `FaultSlot` structures, each with its own pre-allocated `ZSTD_DCtx*` and page-sized scratch buffer. Slots are claimed via atomic CAS on a `used` flag.

### 4.3 Prefetching

After decompressing a faulted page, `prefetchAdjacent()` proactively decompresses up to `kPrefetchWindow` (2) pages in each direction, but only within the same span (same size class, likely related data). This exploits spatial locality: if the application is scanning through an array, adjacent pages are likely to be accessed soon.

Prefetch uses `tryLock()` on adjacent pages to avoid deadlock with the compressor thread.

---

## 5. Syscall Compatibility

### 5.1 The Kernel EFAULT Problem

When the kernel accesses userspace buffers during syscalls (`read`, `write`, `kevent`, `recv`, `send`, etc.), it does not trigger `SIGSEGV`. Instead, `copyin()`/`copyout()` fails and the syscall returns `EFAULT`. This means Smash's `PROT_READ` monitoring and `PROT_NONE` compression can silently break any syscall that touches managed pages.

### 5.2 Syscall Interposition

Smash interposes 20 syscall-level and buffered I/O functions via `__DATA,__interpose` (macOS) / `LD_PRELOAD` (Linux):

**Syscalls:** `read`, `write`, `pread`, `pwrite`, `readv`, `writev`, `recv`, `send`, `recvfrom`, `sendto`, `recvmsg`, `sendmsg`, `poll`, `kevent`

**Buffered I/O:** `fread`, `fgets`, `fgetc`, `getc`, `fwrite`, `fflush`

Each wrapper follows the same pattern:

1. `warmPages()`: touch each managed page in the buffer range to trigger the fault handler and restore `PROT_READ|PROT_WRITE`.
2. `pinPages()`: increment per-page atomic pin count. Pinned pages are skipped by the compressor's monitoring and compression phases.
3. Call the real syscall via the `.original` field of the interpose struct.
4. `unpinPages()`: decrement pin counts.

For buffered I/O functions (`fread`, `fgets`, etc.), both the user buffer and the `FILE` struct's internal buffer (`stream->_bf._base`) are warmed and pinned.

### 5.3 DYLD Interposition Limitations

On macOS, `__DATA,__interpose` only intercepts cross-dylib calls. Intra-libSystem calls are invisible:

- `fread()` internally calls `read()` within the same dylib -- our `smash_read` interpose never fires.
- `getc_unlocked` is a macro that calls `__srget` for buffer refills, all within libSystem.
- C++ `std::cin` goes through `getc_unlocked` -> `__srget` -> `read()`, all intra-libSystem.

**Workaround:** `pinStdioBuffers()` permanently pins `stdin`, `stdout`, and `stderr` FILE structs and their internal buffers at the first compressor tick. This prevents the compressor from ever protecting the pages that libc's internal I/O routines touch.

### 5.4 Calling Original Functions

On macOS, `dlsym(RTLD_NEXT)` returns the wrapper itself (dyld patches all GOTs). The correct pattern reads the `.original` field from the interpose struct:

```cpp
extern "C" ssize_t smash_read(int fd, void* buf, size_t count);
SMASH_INTERPOSE(smash_read, read);

extern "C" ssize_t smash_read(int fd, void* buf, size_t count) {
    warmPages(buf, count); pinPages(buf, count);
    ssize_t ret = smash_interpose_smash_read.original(fd, buf, count);
    unpinPages(buf, count);
    return ret;
}
```

---

## 6. Initialization and Lifecycle

### 6.1 Bootstrap

`SmashHeap` is constructed as a singleton by alloc8's interposition framework. Construction order:

1. `PageMap::init()` -- allocate L1 table from bootstrap.
2. `VmRegion::init()` -- reserve virtual address space. If this fails, fall back to Phase 1 mode (no compression).
3. `PageStateTable::init()`, `PageLockTable::init()` -- per-page state arrays.
4. `CompressStore::init()` -- allocate initial mmap regions for each shard.
5. `CompressEngine::init()` -- pre-allocate LZ4 state, ZSTD CCtx, ZSTD DCtx.
6. `CompressorThread::init()` -- allocate per-page metadata, chunk bitmap, per-worker state, fault slot buffers and DCtxes.
7. Initialize all 144 slabs (4 arenas * 36 classes) with arena IDs and compression hooks.
8. `LargeAlloc::init()` with VmRegion and page state pointers.
9. Allocate `g_page_pins` array from bootstrap.

### 6.2 Deferred Compression Start

Compression is not started during construction. On macOS with `DYLD_INSERT_LIBRARIES`, smash is loaded before `_objc_init` completes. Starting compression threads at that point crashes the ObjC runtime.

Instead, `threadInit()` counts calls via an atomic counter. The first call is the main thread during early init. Compression starts on the second call, which only happens after the runtime is fully initialized.

### 6.3 Graceful Degradation

If `VmRegion::init()` fails (e.g., insufficient virtual address space), Smash falls back to a pure allocator without compression. All slabs are initialized without VmRegion or PageState pointers, and `LargeAlloc` uses direct `mmap`. The allocator still benefits from size-class segregation, bitmap-based tracking, thread caching, and arena-based allocation.

---

## 7. Concurrency Design

Smash uses fine-grained locking throughout. There is no global heap lock.

| Resource | Lock Type | Granularity |
|----------|-----------|-------------|
| Slab | Spinlock | Per-slab (per arena * per size class = 144) |
| Page state | atomic CAS | Per-page |
| Page lock | atomic_flag | Per-page |
| CompressStore | Spinlock | Per-shard (8 shards) |
| VmRegion free list | Spinlock | Single (cold path) |
| VmRegion bump ptr | atomic fetch_add | Lock-free |
| PageMap read | atomic load | Lock-free |
| PageMap L2 create | Spinlock | Single (cold path) |
| LargeAlloc | Spinlock | Single |
| Thread cache | None | Per-thread (no sharing) |
| BootstrapAlloc | atomic CAS per region | Lock-free fast path, spinlock on expand |
| Accessed flags | atomic store/load | Per-page, relaxed ordering |
| Page pins | atomic fetch_add/sub | Per-page, relaxed ordering |
| Fault slots | atomic CAS | Per-slot (32 slots) |

The typical allocation path touches zero shared locks: thread cache hit is entirely thread-local. On cache miss, only one slab lock (out of 144) is acquired. On cache drain, arena-bucketed batch deallocation acquires at most `kNumArenas` slab locks.

---

## 8. Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Free tracking | Bitmap in metadata pages | Keeps data pages pure; critical for compression. ~1 bit/slot overhead. |
| Thread cache storage | Pointer array | Inline freelists contaminate data pages with metadata. |
| Virtual region | Single contiguous reservation | O(1) bounds check, trivial page indexing, pages reusable across size classes. |
| Internal metadata | Bootstrap bump allocator | No dependency on managed heap; safe in signal handler context. Never frees. |
| Compression default | LZ4 | ~5 GB/s decompression minimizes fault latency. |
| Arenas | 4, hash of return address | Concentrates similar data; reduces lock contention; power-of-2 for mask. |
| Zero-on-free | Threshold at 128 bytes | Small objects: eager (cache-hot). Large objects: deferred (avoids regression). |
| CompressStore sharding | 8 shards by page_idx | Eliminates single-lock bottleneck during parallel compression. |
| Fault DCtx | Per-slot (32 slots) | Eliminates data race between concurrent faulting threads. |
| Large alloc VM threshold | 1 MB | Avoids decompression storms from frequently-accessed internal buffers. |

---

## 9. Configuration

All compile-time constants live in `include/smash/config.h`:

| Constant | Default | Description |
|----------|---------|-------------|
| `kPageSize` | 16384 (macOS ARM64) | System page size |
| `kMaxSmallSize` | 16384 | Boundary between slab and large allocation |
| `kNumClasses` | 36 | Number of size classes |
| `kMinAlignment` | 16 | Minimum allocation alignment |
| `kNumArenas` | 4 | Call-site arenas (power of 2) |
| `kTargetObjectsPerSpan` | 64 | Target objects per span |
| `kMaxSpanPages` | 8 | Maximum pages per span |
| `kBootstrapInitialSize` | 64 MB | Initial bootstrap allocator region |
| `kThreadCacheMaxPerClass` | 64 | Thread cache capacity per class |
| `kThreadCacheBatchSize` | 32 | Refill/drain batch size |
| `kCompressIntervalMs` | 1000 | Compressor tick interval |
| `kColdTicks` | 2 | Ticks before LZ4 compression |
| `kVeryColdTicks` | 60 | Ticks before zstd/dict escalation |
| `kMinCompressRatio` | 0.75 | Only store if compressed < 75% of original |
| `kPrefetchWindow` | 2 | Pages prefetched each direction on fault |
| `kDictTrainSamples` | 16 | Pages before dictionary training |
| `kZstdNormalLevel` | 3 | Zstd compression level for cold pages |
| `kZstdDeepLevel` | 9 | Zstd level for very cold pages |
| `kCompressorWorkers` | 2 | Parallel compression workers |
| `kCompressStoreShards` | 8 | CompressStore lock shards |
| `kChunkSize` | 64 | Pages per chunk in scan bitmap |
| `kZeroOnFree` | true | Enable zero-on-free |
| `kZeroOnFreeMaxSize` | 128 | Eager zeroing threshold (bytes) |
| `kLargeAllocVmThreshold` | 1 MB | Minimum large alloc for VmRegion placement |
| `kVmMaxPages` | 1M | Maximum pages in VmRegion |

---

## 10. Measured Results

Benchmarks on macOS ARM64 with `DYLD_INSERT_LIBRARIES`:

| Workload | RSS Reduction | Throughput |
|----------|--------------|------------|
| JSON (cJSON DOM, hot/cold access patterns) | 17.1% | 55.5M accesses/sec |
| SQLite (in-memory DB, mixed operations) | 38.8% | 90,580 ops/sec |
| DuckDB (TPC-H, analytical queries) | ~30% | All phases complete |
| Memcached (key-value, cold data eviction) | 33% | Normal throughput |

The optimized compressor (arenas, deferred zeroing, parallel workers, per-fault-slot DCtx) improved compression ratios 2--3 percentage points over baseline Smash while fixing a DuckDB stability issue (the baseline crashed during the serve phase due to the DCtx data race).

---

## 11. Dependencies

- **alloc8**: Malloc interposition framework providing `ALLOC8_REDIRECT_WITH_THREADS` for cross-platform `malloc`/`free` replacement.
- **LZ4 v1.9.4**: Fast compression (~5 GB/s decompression) for recently-cold pages.
- **Zstandard v1.5.6**: Dictionary compression for very-cold pages, with `ZSTD_customMem` for bootstrap-backed context allocation.

Both LZ4 and Zstandard are fetched via CMake `FetchContent`.
