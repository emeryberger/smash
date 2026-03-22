# userfaultfd vs mprotect Analysis

## Root Cause: Why userfaultfd MODE_MISSING is ~100x Slower

### The Problem

When we register the entire VmRegion with `UFFDIO_REGISTER_MODE_MISSING`, userfaultfd
catches **ALL** first-touch page faults, not just compressed pages.

Every new allocation triggers this slow path:
1. Thread blocks on page fault
2. Handler thread wakes up (context switch)
3. Callback fills zero buffer for EMPTY pages
4. `UFFDIO_COPY` ioctl installs page (syscall)
5. Original thread wakes up (context switch)

With kernel-native page faults (no userfaultfd):
1. Thread faults → kernel allocates zero page directly → thread continues

**Result**: Fill time went from **1.7s** (mprotect) to **>120s** (userfaultfd, timed out)

### Why Signal-Based mprotect is Better for Smash

| Aspect | mprotect (signals) | userfaultfd MODE_MISSING |
|--------|-------------------|-------------------------|
| Normal allocations | Kernel handles directly (fast) | Goes through userspace (slow) |
| Compressed pages | SIGSEGV → decompress | userfaultfd → decompress |
| First-touch overhead | **Zero** | **~100x slower** |

Signal-based mprotect is **targeted**:
- Only PROT_NONE pages trigger SIGSEGV (compressed, compressing)
- Only PROT_READ pages trigger SIGSEGV for writes (monitoring)
- Normal allocations use kernel's native zero-fill path

## Technical Details

### Issue 1: PROT_NONE Blocking userfaultfd

Initial problem: Compression code did:
```c
mprotect(PROT_NONE)  // Make page inaccessible during compression
MADV_DONTNEED        // Release physical page
```

PROT_NONE remained after MADV_DONTNEED, so access triggered SIGSEGV (protection
violation) instead of userfaultfd (missing page). userfaultfd was never invoked.

**Fix**: Add `mprotect(PROT_RW)` after `MADV_DONTNEED` for compressed pages.

### Issue 2: Handler Thread Startup Race

The handler thread was started AFTER registering the region:
```c
registerRegion(...)  // Faults now go to userfaultfd
start(...)           // Handler thread starts AFTER
```

Any fault between register and start caused deadlock.

**Fix**: Start handler thread BEFORE registering region.

### Issue 3: Whole-Region Registration Performance

Even with fixes, registering the entire VmRegion made ALL first-touch allocations
go through userfaultfd, which is fundamentally slow.

## Per-Page Registration Attempt (Not Working)

Per-page registration was implemented to avoid first-touch overhead:
1. Register only compressed pages with userfaultfd
2. Unregistered pages use kernel's native zero-fill path
3. Compressed pages get decompressed via userfaultfd handler

**Implementation:**
```cpp
// After compression:
vm::protectPages(page_addr, kPageSize, true, true);  // PROT_RW
fault_handler_uffd_->registerPage(page_addr);        // Register
vm::decommitPages(page_addr, kPageSize);             // MADV_DONTNEED
```

**Result:** Userfaultfd never receives faults despite pages being registered.
The signal handler catches all faults (pages appear to remain PROT_NONE).

**Isolated tests pass:** Same protection sequence works in standalone test programs.
60000+ sparse page registrations work correctly in isolation.

**Suspected cause:** Interaction between smash's complex memory management,
multiple protection transitions, and VmRegion's MAP_NORESERVE setup. The exact
cause requires kernel-level debugging beyond the scope of this implementation.

## Alternative Approaches (Not Implemented)

1. **memfd + UFFD_FEATURE_MINOR_SHMEM** (Linux 5.13+): Use shared memory with minor
   fault mode. Pages exist but need data population. More suitable for Smash but
   requires VmRegion rewrite.

2. **UFFD_FEATURE_SIGBUS** (Linux 5.11+): Generate SIGBUS instead of blocking.
   Could allow hybrid approach.

## Conclusion

**Signal-based (mprotect) is the correct approach for Smash** because:
1. Normal allocations don't trigger any faults (kernel handles directly)
2. Only compressed/monitored pages incur fault handling overhead
3. Works on all platforms (Linux, macOS)
4. No kernel version requirements

userfaultfd MODE_MISSING is designed for **on-demand paging** where pages start
as missing (e.g., post-copy live migration). Smash's model is different: pages
start as present, get compressed (missing), then decompressed on access.

The code and infrastructure for userfaultfd remain in the codebase (behind
`SMASH_USE_USERFAULTFD`) for future experiments with per-page registration or
minor fault mode.
