# Smash: Compression-Aware Memory Allocator

## Context

OS-level compression caches (zswap, zram, macOS compressed memory) treat all pages identically — they can't exploit application structure. Smash is a userspace memory allocator that does better by:
- **Page homogeneity**: same size class per span → uniform data patterns → higher compression ratios
- **Metadata/data separation**: hot metadata (bitmaps, span descriptors, page tables) lives in dedicated pages, never mixed with user data → data pages are pure and more compressible; metadata stays cache-resident
- **Selective compression**: only compress cold pages; use per-size-class dictionaries that exploit homogeneity
- **Platform-efficient fault handling**: userfaultfd on Linux (dedicated thread, no signal restrictions); Mach exception ports on macOS (also dedicated thread); VEH on Windows

Uses alloc8 (`../alloc8/`) for malloc interposition across all three platforms.

## Architecture Overview

```
┌─────────────────────────────────────────────┐
│  alloc8 interposition layer                 │
│  (LD_PRELOAD / DYLD_INSERT / Detours)       │
├─────────────────────────────────────────────┤
│  SmashHeap                                  │
│  malloc / free / memalign / getSize         │
├──────────────┬──────────────────────────────┤
│ Thread Cache │  SlabAllocator (per class)   │
│ (per-thread, │  ┌─ partial spans            │
│  per-class)  │  ├─ full spans               │
│              │  └─ empty spans              │
├──────────────┴──────────────────────────────┤
│  PageMap (addr → Span*)   │  Span metadata  │
│  (radix tree)             │  (bitmap-based)  │
├───────────────────────────┴─────────────────┤
│  VmRegion (single large virtual reservation) │
│  PageStateTable (ACTIVE/COMPRESSED/EMPTY)    │
├─────────────────────────────────────────────┤
│  FaultHandler          │  CompressorThread   │
│  (userfaultfd/mach/    │  (background scan,  │
│   VEH)                 │   LZ4/zstd)         │
├────────────────────────┴────────────────────┤
│  CompressStore (dedicated mmap regions)      │
│  BootstrapAlloc (bump allocator for metadata)│
└─────────────────────────────────────────────┘
```

## Directory Layout

```
smash/
├── CMakeLists.txt
├── include/smash/
│   ├── smash.h                  # Public API (optional app hints)
│   └── config.h                 # Compile-time tuning knobs
├── src/
│   ├── smash_heap.h/.cpp        # SmashHeap class + ALLOC8_REDIRECT
│   ├── core/
│   │   ├── size_classes.h       # Size class table + mapping
│   │   ├── bootstrap_alloc.h/.cpp  # Bump allocator on mmap for internal metadata
│   │   ├── span.h/.cpp          # Span descriptor (bitmap-based free tracking)
│   │   ├── page_map.h/.cpp      # Two-level radix tree: addr → Span*
│   │   ├── slab.h/.cpp          # Per-size-class span manager
│   │   ├── large_alloc.h/.cpp   # mmap-backed large allocations
│   │   └── thread_cache.h/.cpp  # Thread-local allocation cache
│   ├── vm/
│   │   ├── platform_mem.h/.cpp  # mmap/VirtualAlloc wrappers (never calls malloc)
│   │   ├── vm_region.h/.cpp     # Large virtual reservation (1TB MAP_NORESERVE)
│   │   ├── page_state.h/.cpp    # Per-page state machine (atomic uint8_t array)
│   │   ├── fault_handler.h      # Abstract fault handler interface
│   │   ├── fault_handler_linux.cpp   # userfaultfd (fallback: SIGSEGV)
│   │   ├── fault_handler_macos.cpp   # Mach exception ports (fallback: SIGSEGV)
│   │   └── fault_handler_windows.cpp # Vectored Exception Handling
│   ├── compress/
│   │   ├── compress_store.h/.cpp     # Storage for compressed blobs (own mmap)
│   │   ├── compress_engine.h/.cpp    # LZ4/zstd compress/decompress + dictionaries
│   │   ├── access_tracker.h/.cpp     # Cold page detection
│   │   └── compressor_thread.h/.cpp  # Background scan-and-compress loop
│   └── util/
│       ├── bitops.h             # ctz, popcount, bitmap scan
│       ├── spinlock.h           # Lightweight lock
│       └── intrusive_list.h     # Intrusive doubly-linked list
├── tests/
│   └── (unit + integration tests per component)
└── bench/
    └── (throughput, compression ratio, RSS reduction)
```

## Implementation Phases

> **Status: All phases implemented and verified.** 12/12 tests pass. Evaluation benchmarks show 17-39% RSS reduction across JSON, SQLite, DuckDB, and Memcached workloads.

### Phase 1: Core Allocator (no compression) ✓

A working size-class-segregated allocator with metadata/data separation, interposed via alloc8. No compression yet — this establishes the structural invariants.

**1a. Build system + bootstrap**
- `CMakeLists.txt`: C++20, pull alloc8 from `../alloc8` (local) or git, set `-fno-builtin-malloc` etc., build `libsmash.so/.dylib`
- `src/util/platform_mem.h`: Thin wrappers around `mmap(MAP_PRIVATE|MAP_ANON)` / `VirtualAlloc`. Only raw OS calls, never malloc.
- `src/core/bootstrap_alloc.h`: Bump allocator on a 64MB mmap region for all internal metadata. `owns(ptr)` for free-path detection. Solves the bootstrap problem (smash replaces malloc but needs memory for its own structures).
- `src/util/bitops.h`, `spinlock.h`, `intrusive_list.h`

**1b. Size classes**
- `src/core/size_classes.h`: ~40 classes with sub-linear spacing (step 16 up to 128, step 32 up to 256, ..., step 2048 up to 16384). Compile-time table. `sizeToClass()` via small lookup table for ≤1024, log2 for larger. `kMaxSmallSize = 16384` (configurable).

**1c. Spans + page map (metadata separation enforced)**
- `src/core/span.h`: Span descriptor allocated from **BootstrapAlloc** (metadata region), not inline with data. Fields: `base`, `page_count`, `size_class`, `object_count`, `free_bitmap*` (bitmap also in metadata region), `first_free_hint`, intrusive list node. `allocate()` scans bitmap via ctz. `deallocate()` clears bit.
- `src/core/page_map.h`: Two-level radix tree. Level 1: 1M entries (8MB from bootstrap). Level 2: 64K entries each, lazy. O(1) lookup: `page_map[addr >> (PAGE_SHIFT + L2_BITS)][addr >> PAGE_SHIFT & L2_MASK]`.

**Key decision — bitmap vs inline freelist**: Bitmaps keep data pages untouched (no next-pointers written into freed objects). This is critical: inline freelists would contaminate data pages with metadata, ruining both the separation invariant and compressibility. The cost is ~1 bit per slot of overhead in metadata pages and a bitmap scan on alloc (~3 instructions with ctz).

**1d. Slab allocator**
- `src/core/slab.h`: Per-size-class. Maintains intrusive lists of partial/full/empty spans. `allocate()`: pop from partial, bitmap alloc, promote to full if needed. `deallocate()`: clear bit, demote from full to partial, detect empty. `scavenge()`: return empty spans' pages to OS.

**1e. Large allocations**
- `src/core/large_alloc.h`: Sizes > kMaxSmallSize go to mmap directly. Span descriptor (from bootstrap) tracks base and size. Page map entries set for the entire range.

**1f. Thread cache**
- `src/core/thread_cache.h`: Per-thread, per-size-class cache. Stores pointers in a small array (not inline freelists) to avoid writing into data pages. On alloc miss, refill batch from slab. On free when full, drain batch to slab. Initialized via alloc8's `threadInit()` hook, cleaned up via `threadCleanup()`.

**1g. SmashHeap + alloc8 integration**
- `src/smash_heap.h/.cpp`: Implements alloc8 interface (`malloc`, `free`, `memalign`, `getSize`, `lock`, `unlock`, `threadInit`, `threadCleanup`). Fast path: `sizeToClass` → thread cache → slab. Uses `ALLOC8_REDIRECT_WITH_THREADS(SmashRedirect)`.
- Pattern follows `alloc8/examples/simple_heap/simple_heap.cpp`

**Milestone**: smash works as a drop-in malloc replacement (`LD_PRELOAD`/`DYLD_INSERT_LIBRARIES`). No compression, but metadata and data live in separate pages.

### Phase 2: Virtual Memory Management + Fault Handling ✓

Migrate data pages into a single large virtual reservation. Add page state tracking and fault interception infrastructure.

**2a. Virtual region**
- `src/vm/vm_region.h`: Reserve ~1TB via `mmap(MAP_NORESERVE)` (Linux) or plain `mmap` (macOS). Commit/decommit pages on demand. Single contiguous range simplifies all address checks to a bounds test.

**2b. Page state table**
- `src/vm/page_state.h`: `atomic<uint8_t>` per page in the reservation. States: `EMPTY`, `ACTIVE`, `COMPRESSED`. Allocated from bootstrap. CAS for thread-safe transitions.

**2c. Fault handler (platform abstraction)**
- `src/vm/fault_handler.h`: Interface: `registerRange()`, `protectPage()`, `unprotectPage()`, `start()`, `stop()`, + callback.
- **Linux** (`fault_handler_linux.cpp`): `userfaultfd` + dedicated poll thread. `UFFDIO_REGISTER` the VmRegion. On fault → `UFFDIO_COPY` decompressed page. Fallback to `mprotect(PROT_NONE)` + `SIGSEGV` handler if userfaultfd unavailable.
- **macOS** (`fault_handler_macos.cpp`): Mach exception port for `EXC_MASK_BAD_ACCESS` with dedicated `mach_msg()` receiver thread. Decompress page, `vm_protect()`, reply to resume. Fallback to `SIGSEGV` handler.
- **Windows** (`fault_handler_windows.cpp`): `VirtualProtect(PAGE_NOACCESS)` + `AddVectoredExceptionHandler()`. On `EXCEPTION_ACCESS_VIOLATION`, decompress and `VirtualProtect(PAGE_READWRITE)`.

**2d. Integrate with slab allocator**
- Modify slab to allocate spans from VmRegion instead of raw mmap. Set page states to ACTIVE on commit, EMPTY on decommit.

**Milestone**: all data pages come from VmRegion. Fault handler can protect/unprotect pages and invoke a callback on access.

### Phase 3: Compression Engine ✓

**3a. Compression store**
- `src/compress/compress_store.h`: Variable-size blob allocator backed by its own mmap regions (NOT from VmRegion — avoids recursive compression). Region-chained bump allocator with free list for released blobs.

**3b. Compress/decompress engine**
- `src/compress/compress_engine.h`: Wraps LZ4 (speed-default) and zstd (ratio when cold). Pre-allocated contexts to avoid malloc during compress/decompress. Dictionary support for Phase 5.

**3c. Compressor thread**
- `src/compress/compressor_thread.h`: Background thread. Loop: sleep → scan VmRegion for cold ACTIVE pages → compress each → store in CompressStore → protect page (PROT_NONE) → set state to COMPRESSED. On fault callback: look up CompressedPage by page index → decompress → unprotect → set state to ACTIVE.
- `compressed_pages_[]` array (indexed by page index in VmRegion, allocated from bootstrap) maps page → CompressedPage handle.

**Milestone**: end-to-end compression works. Allocate data, let it go cold, observe RSS drop. Access compressed data, observe transparent decompression.

### Phase 4: Access Tracking + Cold Page Detection ✓

**4a. Access tracker**
- `src/compress/access_tracker.h`: Determines which pages are cold.
- **Linux**: soft-dirty bits (`/proc/self/clear_refs` + `/proc/self/pagemap`). Low overhead, kernel-supported.
- **macOS**: `mprotect(PROT_READ)` to detect writes; on write fault, mark accessed and restore `PROT_READ|PROT_WRITE`. Coordinated with compression fault handler.
- **Windows**: `QueryWorkingSetEx` to check page access.
- Clock algorithm: 2-bit per page (accessed-this-tick, accessed-last-tick). Pages cold for 2+ ticks are compression candidates.

**4b. Compression policy**
- Configurable via `include/smash/config.h`:
  - `SMASH_COMPRESS_INTERVAL_MS` (default 1000): scan frequency
  - `SMASH_COLD_TICKS` (default 2): ticks before page is cold
  - `SMASH_RSS_THRESHOLD`: compress more aggressively above this
  - `SMASH_MIN_COMPRESS_RATIO` (default 0.75): don't store if compressed size > 75% of original

**Milestone**: only genuinely cold pages get compressed. Hot working set remains uncompressed with zero overhead.

### Phase 5: Optimization ✓

**5a. Per-size-class dictionaries**: After enough pages of a size class accumulate, train a zstd dictionary on sample pages. Homogeneous pages of the same size class compress dramatically better with a tuned dictionary.

**5b. Adaptive algorithm selection**: LZ4 for pages under memory pressure (speed). zstd level 1-3 for normal cold pages. zstd level 9+ for very cold pages (not accessed for minutes).

**5c. Batch operations**: Compress multiple contiguous cold pages in one pass. Batch `madvise(MADV_DONTNEED)` for contiguous decommits.

**5d. Prefetching**: On fault, proactively decompress adjacent compressed pages (spatial locality). Amortizes fault overhead.

### Phase 6: Testing + Benchmarking ✓

**Unit tests**: size class mapping, bootstrap alloc, slab alloc/free cycles, page map correctness, fault handler protect/unprotect/fault cycle, compression roundtrip, thread cache under contention.

**Integration tests**: thousands of malloc/free with varying sizes and threads. Interposition test: run a real program under `LD_PRELOAD`/`DYLD_INSERT_LIBRARIES`.

**Benchmarks**:
- Throughput: malloc/free pairs/sec vs system malloc, jemalloc, mimalloc
- Compression ratio: by size class, with/without dictionaries
- RSS reduction: allocate large dataset, idle, measure RSS over time
- Latency: p50/p99/p999 for alloc/free (especially decompression-on-fault latency)

## Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Free tracking | Bitmap in metadata pages | Keeps data pages pure; critical for compression. ~1 bit/slot overhead. |
| Thread cache storage | Pointer array (not inline freelist) | Inline freelists write metadata into data pages, contaminating them. |
| Virtual region | Single 1TB reservation | O(1) address-to-span lookup; pages reassignable between size classes. |
| Fault handler (Linux) | userfaultfd preferred | Dedicated handler thread avoids signal-context restrictions. |
| Fault handler (macOS) | Mach exception port preferred | Dedicated thread, like userfaultfd. SIGSEGV fallback. |
| Compression store | Own mmap regions | Can't use smash (circular) or system malloc (replaced). |
| Internal metadata | Bootstrap bump allocator | No dependency on the managed heap. 64MB initial, expandable. |
| Compression default | LZ4 | Decompression at ~5 GB/s minimizes fault latency. zstd for cold pages. |

## Verification Plan

1. **Phase 1**: `LD_PRELOAD=./libsmash.so ls` (or any program) completes without crash. Run alloc8's existing test harness against smash.
2. **Phase 2**: Allocate memory, `mprotect(PROT_NONE)` a page, access it, verify fault handler fires and unprotects.
3. **Phase 3**: Allocate 1GB, stop touching it, observe RSS drop after compressor thread runs. Touch it again, verify data integrity.
4. **Phase 4**: Verify that only cold pages are compressed (hot working set never faults).
5. **Phase 5**: Measure compression ratio with/without dictionaries on homogeneous pages.
6. **End-to-end**: Run Redis or a similar real application under smash, measure RSS reduction vs baseline.
