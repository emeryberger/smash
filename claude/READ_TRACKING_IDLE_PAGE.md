# Fault-free read tracking via idle-page tracking — design + hard constraints

## Goal
Today smash detects READS by escalating a cold page to mprotect(PROT_NONE)
(escalateToDeepMonitoring, compressor_thread.h:1423) so the next read SIGSEGVs
into the fault handler. That's an mprotect to arm + a protection fault PER READ
on read-hot pages — pure overhead just to learn "still being read." A page may
only be compressed when it is cold to BOTH reads and writes (any access to a
compressed page = decompress fault), so read-coldness genuinely must be
established before compressing. Soft-dirty (write-only) is necessary but NOT
sufficient — it can't see reads. Hence: track reads fault-free via the PTE
Accessed (young) bit, which is what the kernel's idle-page-tracking exposes.

## HARD CONSTRAINTS discovered (verified on this host, 6.12 amzn2023)
1. CONFIG_IDLE_PAGE_TRACKING is NOT set here (only CONFIG_PAGE_IDLE_FLAG=y).
   /sys/kernel/mm/page_idle/bitmap does not exist → unavailable on this kernel.
2. idle-page-tracking is PFN-INDEXED: you read/write /sys/kernel/mm/page_idle/
   bitmap by page-frame number, and map virtual→PFN via /proc/self/pagemap.
3. UNPRIVILEGED PROCESSES CANNOT USE IT. Verified: euid != 0, and
   /proc/self/pagemap returns pfn=0x0 (redacted since kernel ~4.0 without
   CAP_SYS_ADMIN). No PFN ⇒ can't index the bitmap. The bitmap itself is also
   root-only. mincore() is available but reports RESIDENCY, not the accessed
   bit — insufficient for read tracking.
   ⇒ Smash's production model (unprivileged + LD_PRELOAD) CANNOT use idle-page
   tracking. It is only usable when smash runs as root / CAP_SYS_ADMIN on a
   kernel with CONFIG_IDLE_PAGE_TRACKING=y.

## Decision: build it GATED with a runtime probe + clean fallback (user opt: "1")
- Probe at startup: open /sys/kernel/mm/page_idle/bitmap (O_RDWR) AND confirm
  /proc/self/pagemap yields a non-zero PFN for a known-resident page. If either
  fails → idle read-tracking is OFF, fall back to today's PROT_NONE-fault method
  unchanged. Gate behind SMASH_IDLE_READ_TRACK (default AUTO = use iff probe
  passes; =0 force off; =1 force on + warn if probe fails).
- When active: replace escalateToDeepMonitoring's PROT_NONE arm with the idle
  protocol — mark candidate cold pages idle (write their PFN bit), and on the
  next tick read the bitmap: bit still idle ⇒ not read since ⇒ truly read-cold,
  eligible to compress; bit cleared ⇒ was read ⇒ reset cold_count, no fault paid.
  This removes BOTH the per-page mprotect and the per-read protection fault.
- PFN cost: pagemap read already happens for soft-dirty (readSoftDirty); reuse
  that batched pread to also harvest PFNs (same 8-byte entries) so the marginal
  cost is one extra bitmap pread/pwrite per tick, no per-page syscalls.

## Honest expectation
On this host and any unprivileged deployment this path is a NO-OP (probe fails →
fallback). It pays off only for root/CAP_SYS_ADMIN deployments on idle-tracking
kernels. So it is NOT the general answer to the read-fault cost — it is an
opportunistic acceleration where the platform allows. The portable lever for the
common case remains the soft-dirty-ROI work (claude/SOFTDIRTY_ROI_DESIGN.md):
it doesn't eliminate read detection but reduces how often we pay it by being
smarter about WHICH cold pages to attempt. Recommend landing the gated idle path
for capable hosts, but not expecting it to move numbers on this benchmark host.

## mincore as an alternative — INVESTIGATED, NOT VIABLE (verified on this host)
User asked twice about mincore() instead of kernel idle-page tracking. Tested empirically:
- mincore() reports **residency only** (bit0 = in core), NOT the accessed/young bit.
  Pre-2019 kernels leaked the referenced bit; CVE-2019-5489 closed that, so modern
  kernels (incl. this 6.12) expose residency only. Confirmed: a faulted page reads
  resident=1 regardless of whether it was subsequently accessed.
- To use residency AS an access proxy you'd have to force eviction (MADV_PAGEOUT /
  MADV_FREE) then detect re-faulting via mincore. Two killers: (1) needs swap or
  memory pressure to actually evict — this host has Swap: 0, and MADV_FREE/COLD did
  NOT drop residency without pressure (verified); (2) it would EVICT the very pages
  smash wants resident, adding page-out/in churn ON TOP of compression churn —
  counterproductive, fighting the allocator's purpose.
- VERDICT: mincore is strictly worse than both alternatives for read tracking.
  vs idle-tracking: idle reads the young bit non-destructively; mincore needs
  destructive eviction. vs PROT_NONE faults (current): faults give exact immediate
  read signal with no eviction. mincore's only orthogonal use: spotting pages the
  kernel ALREADY evicted so smash can skip compressing them (minor RSS refinement,
  not read tracking). Not pursuing.
- ADVANTAGE mincore has over idle-tracking: it's UNPRIVILEGED (works without
  CAP_SYS_ADMIN). But that doesn't rescue it — residency-without-pressure is
  uninformative, and forcing pressure is self-defeating. Unprivileged + useless
  beats privileged + useful only if it's actually useful.

## Validation
- Where probe fails (here): assert behavior is byte-identical to today (fallback),
  16/16 ctest, no perf change. That's the main thing testable on this host.
- Where probe passes (would need a root run on an idle-tracking kernel — may not
  be reachable in this environment): A/B the read-fault count on a read-hot
  workload (bench_zipf_reaccess --write-pct 20: lots of reads, few writes) —
  expect protection-fault count to drop toward zero for read-hot pages.
