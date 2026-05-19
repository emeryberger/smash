# Smash + MongoDB (WiredTiger) Integration

## Summary

MongoDB 8.0 uses WiredTiger (WT) as its storage engine. WT manages an in-memory B-tree cache with background eviction threads. Smash can interpose on WT's malloc calls via `DYLD_INSERT_LIBRARIES` (macOS) since the official ARM64 binary ships with `allocator=system`.

## Key Findings

### 1. Hardened Runtime Blocks DYLD Injection

The official MongoDB macOS binary has `flags=0x10000(runtime)` (hardened runtime), which causes macOS to strip `DYLD_INSERT_LIBRARIES` before the process starts. **Fix:** re-sign with ad-hoc entitlements:

```bash
codesign --force --sign - --entitlements mongod_entitlements.plist mongod
```

Required entitlements: `allow-dyld-environment-variables`, `disable-library-validation`, `allow-unsigned-executable-memory`.

### 2. Syscall Interposers Break kevent-Based Networking

MongoDB's ASIO event loop uses `kevent`/`kevent64`. Smash's syscall wrappers (`retryWithDecompress`) interfere with these calls once Phase 2/3 activates, causing connection timeouts. **Workaround:** `SMASH_DEFER_PHASES_MS` delays mprotect-based monitoring long enough for data loading to complete.

### 3. WiredTiger Architecture: Two-Level Allocation

WT separates per-page allocations:
- **`WT_PAGE` struct** (~200 bytes): control metadata, `read_gen` (LRU counter), flags, pointers. Eviction threads read this on every walk pass.
- **`page->dsk` buffer** (4–32 KB): serialized on-disk page image holding actual row/column data. Only accessed when app threads read/write document content.

Eviction threads walk `WT_PAGE` headers to assess LRU priority but do NOT read `dsk` data buffers unless actively evicting. This means **data buffers DO go cold** when documents aren't accessed — we just couldn't detect it previously because mprotect was also trapping header accesses.

### 4. Eviction Thread Behavior

- Default: 4 threads (`eviction=(threads_min=4,threads_max=4)`)
- Walk ~300 pages per tree per pass, reading `WT_PAGE.read_gen`
- Activated when cache usage exceeds `eviction_target` (default 80%)
- When cache is well under target: threads idle (just check pressure periodically)
- `read_gen` uses special values: `EVICT_SOON(1)`, `WONT_NEED(2)`, `START_VALUE(100)`

### 5. No Custom Allocator API

WT has no `WT_ALLOCATOR` extension point. All memory goes through libc `malloc`/`free`. LD_PRELOAD/DYLD_INSERT is the correct interposition strategy.

## Strategies

### Strategy A: Large Cache (No Eviction Pressure)

**Hypothesis:** If WT's cache is larger than the dataset, eviction threads idle. `dsk` buffers for unaccessed documents go truly cold. Smash compresses them.

**Configuration:**
- `--wiredTigerCacheSizeGB 2` with a ~200 MB dataset → cache never hits 80% trigger
- `SMASH_NO_MONITOR=1` — skip mprotect (avoids kevent/networking conflict)
- `SMASH_DEFER_PHASES_MS=30000` — let networking stabilize before compression

**Expected result:** After the defer period + cold timeout, Smash should see `dsk` buffers as cold (no accesses since data loading finished) and compress them.

**Risk:** Even without eviction pressure, WT may still periodically touch pages via:
- Checkpoint thread (every 60s, reads dirty page data)
- Statistics collection (touches page metadata)
- Session sweeper (metadata only)

### Strategy B: SMASH_LARGE_ONLY Targeting dsk Buffers

**Hypothesis:** WT's `dsk` buffers are large (4–32 KB), while `WT_PAGE` structs and other metadata are small (~200 bytes). Using `SMASH_LARGE_ONLY=1` lets system malloc handle the small metadata allocations (which eviction threads frequently touch), while Smash manages only the large `dsk` buffers (which truly go cold).

**Configuration:**
- `SMASH_LARGE_ONLY=1` — only intercept allocations > 16 KB
- `--wiredTigerCacheSizeGB 2` — keep cache under pressure threshold
- `SMASH_NO_MONITOR=1` — avoid mprotect on network buffers
- `SMASH_DEFER_PHASES_MS=30000` — networking stability

**Expected result:** Smash's VmRegion only contains large buffers (primarily `dsk` page images). Eviction thread walks only touch system-malloc'd WT_PAGE headers, never entering Smash's page-state table. Cold `dsk` buffers get compressed.

**Risk:** WT's default `leaf_page_max` is 32 KB but `internal_page_max` is 4 KB. Many allocations may be exactly at the 16 KB boundary. May need to lower the threshold.

## Experimental Results (2026-05-18)

**Setup:** 200K docs × 1024B values, WT cache = 2 GB (well above ~200 MB dataset), cool = 45s.

| Config | Fill RSS | Cool RSS | Compressed Pages | Outcome |
|--------|----------|----------|-----------------|---------|
| Baseline (system malloc) | 448 MB | 496 MB | N/A | Steady — no compression |
| Strategy A (full Smash) | 489 MB | 474 MB | 0 | RSS drops 100 MB from zeroing, NOT compression |
| Strategy B (LARGE_ONLY) | 514 MB | **CRASH** | 0 | Bus error when Phase 2 mprotects `dsk` buffers |

### Analysis

**Strategy A:** Smash correctly manages 18K pages. After the defer period, ~9K pages go EMPTY (freed by WT reorganization). The 100 MB RSS drop (573 → 474) is Smash's deferred `ntZeroMemory` + `MADV_FREE_REUSABLE` on freed pages — an OS-level page reclamation benefit, not compression. WT's background threads keep all ACTIVE pages warm, so `compressed=0`.

**Strategy B:** Crashed with Bus Error 10 at ~t=20s (when `SMASH_DEFER_PHASES_MS` expired). Root cause: Phase 2 calls `mprotect(PROT_NONE)` on pages being compressed. WT's eviction thread (or checkpoint thread) then accesses that page and hits an unmapped-page fault that the Smash fault handler cannot catch (it may be on a thread without a registered handler, or the page state machine race is lost). `SMASH_NO_MONITOR=1` only disables Phase 3 (PROT_READ monitoring), NOT Phase 2's mprotect during compression.

### Root Cause (Definitive)

Smash's compression model fundamentally requires `mprotect(PROT_NONE)` on compressed pages so that subsequent access triggers the SIGSEGV handler for decompression. This is incompatible with WiredTiger because:

1. **WT has uncoordinated background threads** that may access any cached page at any time (eviction walk, checkpoint, session sweeper).
2. **There is no way to "pin" a page** from WT's perspective — Smash cannot signal to WT "don't touch this page, I'm compressing it."
3. **The page-state CAS + per-page lock** in Smash protects against concurrent *Smash operations* (compressor vs fault handler), but not against external threads that access raw memory without going through Smash's API.

Even if a page's `dsk` buffer hasn't been read by application threads in minutes, WT's checkpoint thread may reconcile it to disk at any time (reading the buffer contents), and eviction threads may inspect its `memory_footprint` field — both of which will fault if the page is PROT_NONE.

## Conclusion

MongoDB (WiredTiger) is **not compatible with Smash** without modifications to either:

1. **WiredTiger** — add a "page pinning" API or "external memory advisor" callback that Smash can use to coordinate mprotect with WT's page lifecycle.
2. **Smash** — implement a non-mprotect compression mode (e.g., replace page contents in-place with compressed data + header, without changing page protections). This would require a fundamentally different decompression trigger mechanism.

Neither approach is trivial. The mprotect-based fault-to-decompress model is architecturally incompatible with any system that has background threads performing unsolicited memory reads on managed pages.
