# External-Mapping Tracking (`SMASH_TRACK_EXTERNAL=1`)

Standard smash compresses pages within its own `MAP_ANON` arena. Application code that calls `mmap()` / `mach_vm_allocate()` directly bypasses malloc and so escapes the compressor. The mmap and Mach VM interposers in `smash_heap.cpp` (macOS) and `linux_syscall_wrappers.cpp` (Linux) register such mappings with the VmRegion's external-page hash so the compressor's tick can walk them. **Opt-in**: set `SMASH_TRACK_EXTERNAL=1` to enable the registration path; default off.

## VmRegion full+tracking hybrid (`vm/vm_region.h`)

In full mode VmRegion keeps the contiguous bump-arena (indices `0..contig_pages_-1`) AND a tracking hash for external pages (indices `contig_pages_..contig_pages_+kTrackMaxPages-1`). `total_pages_ = contig_pages_ + kTrackMaxPages` so `PageStateTable` / `PageLockTable` cover both ranges; `committedPages()` returns the high-water across both; `pageAddress(idx)`, `pageIndex(addr)`, and `contains(addr)` route on idx range / address range. The compressor's existing tick / dispatch logic processes external pages without modification — they're just pages with high indices.

Cost: ~1 MB extra bootstrap memory (track hash + reverse map + page-state slots for `kTrackMaxPages = 128 K` external slots).

## Filter rules (deliberately strict)

- `mmap` only registers `MAP_ANON | PROT_WRITE` mappings. File-backed mappings are skipped — compressing them would break `msync` semantics and the OS already evicts them. Read-only mappings are skipped — no dirty bits, no compression value.
- `mach_vm_allocate` / `vm_allocate` only register when `target == mach_task_self()`. Cross-task allocations belong to children.
- `BootstrapAlloc`-routed `mmap` calls happen before `g_smash_vm_region` is set, so they're never tracked. Smash's own contiguous reservation uses `PROT_NONE` and is filtered by the writable-only rule.
- On `munmap` / `mach_vm_deallocate` we mark pages EMPTY before the actual unmap so the compressor stops scanning them. Compressed-buffer leakage on unmap is possible (we don't free the associated compressed bytes); bounded by workload churn.

## Why opt-in

Single-trial Firefox 5-tab Wikipedia at 90 s (full smash + DEFER 30 s + all-procs) showed the registration path possibly regressing stability (33-35 s lifetime vs 61 s with tracking off), but the variance across nominally-identical configs is too high to claim a regression with confidence — FIREFOX_STUDY's "10/10 alive at 60 s" baseline used N=10. Conservative default is off; targets with controlled allocation patterns (e.g. redb-style workloads) can opt in. The interposers themselves still install regardless of the env var, so the runtime cost when off is one branch per `mmap` / `mach_vm` call.

The "compressed=266K pages = 4.2 GB" figure observed under tracking-on on Firefox is misleading: most of those are virtual-address-space artifacts (SpiderMonkey + Skia reserve large `MAP_ANON` ranges that never fault in; the compressor processes zero pages to ~30-byte buffers). A correct Firefox-RSS measurement needs virt-vs-RSS reconciliation in the SIGUSR2 stats handler before claiming any Firefox win.
