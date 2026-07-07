------------------------- MODULE SmashExternalRace -------------------------
(*
 * SmashExternalRace — models the full-mode external-page munmap-vs-compress
 * race that crashes CompressorThread::compressPage (root-caused 2026-06-24,
 * full-mode rocksdb SIGSEGV). Companion to model/SmashExternalRace.lean, which
 * carries the machine-checked proof (zero axioms).
 *
 * In full mode smash tracks application-direct mmap regions as EXTERNAL pages
 * so the compressor can compress them. A worker compresses a page by
 * SNAPSHOTTING it: memcpy(worker.page_buf, page_addr, kPageSize), where
 * page_addr = VmRegion::track_reverse_[idx]. When the app munmap()s that
 * region, deregisterLinuxExternalRange sets PageState=EMPTY, untracks, and
 * calls real_munmap — but WITHOUT taking the per-page lock. So:
 *
 *     Worker:  tryLock(idx) ; state==ACTIVE ; state := COMPRESSING
 *     munmap:  state := EMPTY (NO LOCK) ; untrack ; real_munmap(addr)
 *     Worker:  memcpy(buf, page_addr, 4096)   <-- reads unmapped page -> SIGSEGV
 *
 * Variant:
 *   "unlocked" : PRE-FIX — munmap mutates state + unmaps WITHOUT the per-page
 *                lock. NoUnmappedRead is VIOLATED.
 *   "locked"   : FIX — munmap must acquire the per-page lock before clearing
 *                state / unmapping. NoUnmappedRead HOLDS.
 *
 * Deadlock note (user-flagged): the locked fix serializes munmap-untrack vs the
 * worker SNAPSHOT (a plain memcpy — no TLB-shootdown IPI held by either side)
 * and releases the lock before real_munmap, so it does NOT reintroduce the
 * documented TLB-shootdown-vs-page-lock deadlock (compressor_thread.h:1925).
 *
 * Safety invariant (NoUnmappedRead): a worker never snapshots an unmapped page.
 *
 * TLC results (run with `-deadlock`; this is a terminating model, not reactive):
 *   _unlocked_buggy.cfg : Invariant NoUnmappedRead is VIOLATED (crash trace).
 *   _locked_fixed.cfg   : No error — NoUnmappedRead AND MutualExclusion hold.
 * This agrees with SmashExternalRace.lean (machine-checked, zero axioms).
 *)

EXTENDS TLC

CONSTANTS Variant   \* "unlocked" | "locked" | "extent"

VARIABLES
    pstate,   \* "ACTIVE" | "COMPRESSING" | "EMPTY"
    lock,     \* "free" | "worker" | "munmapper"
    mapped,   \* BOOLEAN — physical backing still mmap'd?
    wpc,      \* "WIdle" | "WSnapshot" | "WDone"
    mpc,      \* "MIdle" | "MUnmap" | "MDone"
    revZeroed,\* BOOLEAN — track_reverse_[idx] cleared UNDER lock (extent variant)
    badRead   \* BOOLEAN ghost — worker read an UNMAPPED page (the crash)

vars == << pstate, lock, mapped, wpc, mpc, revZeroed, badRead >>

Init ==
    /\ pstate    = "ACTIVE"
    /\ lock      = "free"
    /\ mapped    = TRUE
    /\ wpc       = "WIdle"
    /\ mpc       = "MIdle"
    /\ revZeroed = FALSE
    /\ badRead   = FALSE

\* --- Compressor worker ------------------------------------------------------

\* compressPage entry (deferred mode): tryLock + verify ACTIVE, mark COMPRESSING.
WorkerLock ==
    /\ wpc = "WIdle"
    /\ lock = "free"
    /\ pstate = "ACTIVE"
    /\ lock'   = "worker"
    /\ pstate' = "COMPRESSING"
    /\ wpc'    = "WSnapshot"
    /\ UNCHANGED << mapped, mpc, revZeroed, badRead >>

\* The snapshot read memcpy(buf, page_addr, kPageSize). If the page was
\* unmapped, this is a wild read -> badRead (the SIGSEGV). Then unlock.
\* EXTENT variant: page_addr comes from track_reverse_; if that slot was zeroed
\* under the lock (revZeroed), page_addr is null and the worker SKIPS the read.
\* So a bad read needs unmapped AND ~revZeroed.
WorkerSnapshot ==
    /\ wpc = "WSnapshot"
    /\ badRead' = (badRead \/ (~mapped /\ (Variant # "extent" \/ ~revZeroed)))
    /\ lock'    = "free"
    /\ wpc'     = "WDone"
    /\ UNCHANGED << pstate, mapped, mpc, revZeroed >>

\* --- munmap interposer ------------------------------------------------------

\* deregisterLinuxExternalRange: set EMPTY + untrack.
\*   unlocked: fires regardless of the lock (the bug).
\*   locked:   only when the lock is free; acquires it (the fix).
\* "locked" and "extent" both take the per-page lock before clearing state.
\* "extent" additionally zeroes track_reverse_[idx] (revZeroed) under that lock —
\* the load-bearing safety step — while flipping extent.live is routing-only and
\* not modeled as a separate variable (it does not gate the worker's read).
MunmapClear ==
    /\ mpc = "MIdle"
    /\ IF Variant \in {"locked", "extent"}
         THEN /\ lock = "free"
              /\ lock' = "munmapper"
         ELSE /\ lock' = lock
    /\ pstate'    = "EMPTY"
    /\ revZeroed' = (Variant = "extent")   \* clear reverse slot under lock
    /\ mpc'       = "MUnmap"
    /\ UNCHANGED << mapped, wpc, badRead >>

\* real_munmap: tear down the mapping. locked/extent release the lock here
\* (no IPI held under the lock -> deadlock-free).
MunmapUnmap ==
    /\ mpc = "MUnmap"
    /\ mapped' = FALSE
    /\ lock'   = IF Variant \in {"locked", "extent"} THEN "free" ELSE lock
    /\ mpc'    = "MDone"
    /\ UNCHANGED << pstate, wpc, revZeroed, badRead >>

Next ==
    \/ WorkerLock
    \/ WorkerSnapshot
    \/ MunmapClear
    \/ MunmapUnmap
    \* Stutter when both actors are done so TLC has a terminal-state self-loop.
    \/ (wpc = "WDone" /\ mpc = "MDone" /\ UNCHANGED vars)

Spec == Init /\ [][Next]_vars

\* --- Safety -----------------------------------------------------------------

\* The worker never read an unmapped page.
NoUnmappedRead == badRead = FALSE

\* Type-correctness sanity.
TypeOK ==
    /\ pstate \in {"ACTIVE", "COMPRESSING", "EMPTY"}
    /\ lock   \in {"free", "worker", "munmapper"}
    /\ mapped \in BOOLEAN
    /\ wpc \in {"WIdle", "WSnapshot", "WDone"}
    /\ mpc \in {"MIdle", "MUnmap", "MDone"}
    /\ revZeroed \in BOOLEAN
    /\ badRead \in BOOLEAN

\* Mutual exclusion (locked variant): the worker's snapshot critical section and
\* munmap's unmap critical section never overlap. Checked as an invariant for
\* the locked cfg; expected to fail for the unlocked cfg (which is the point).
MutualExclusion == ~(wpc = "WSnapshot" /\ mpc = "MUnmap")

=============================================================================
