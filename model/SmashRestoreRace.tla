------------------------- MODULE SmashRestoreRace -------------------------
(*
 * SmashRestoreRace — models the decompress-on-fault RESTORE path and the
 * TOCTOU window that SmashSnapshotRace1-4 all missed.
 *
 * The earlier models (SmashSnapshotRace1-4) focused on the COMPRESS side: the
 * window between snapshot and madvise where a writer's store could be lost or
 * a reader could observe a kernel zero-page. Their fault handler (FIdle) modeled
 * decompress-restore as a SINGLE ATOMIC STEP:
 *
 *     pageVal := snapshot; pageBacking := BACKED; prot := RW   (all at once)
 *
 * The production C++ code did NOT do this atomically. handleFault() (and
 * prefetchAdjacent()) restored a compressed page as:
 *
 *     decompress(blob -> scratch)
 *     commitPages(page_addr)              \* mprotect(PROT_RW): page READABLE
 *     memcpy(page_addr, scratch, 4096)    \* decompressed data lands HERE
 *
 * Between the mprotect and the memcpy the page is READABLE but still holds the
 * OLD physical backing (zeros, if madvise(DONTNEED) dropped it; stale bytes
 * otherwise). The faulting thread holds the per-page lock, but a CONCURRENT
 * application reader doing an ordinary load does not fault (the page is now
 * PROT_RW) and does not take the per-page lock — so it reads the wrong data.
 * This is the root cause of the ~67% nondeterministic full-mode failure on
 * neuron-cc (walrus mod_parallel_pass: "overlapping memloc", BIR-verification,
 * scheduler, and llvm DenseMap assertions — all downstream of one page coming
 * back wrong).
 *
 * This model strips the compress side down to its essentials and focuses on the
 * restore path, parameterized by RestoreVariant:
 *
 *   "atomic"  — the OLD MODEL's behavior: restore is one atomic step. Used to
 *               demonstrate that the previous models could never have caught
 *               this bug (it passes).
 *   "split"   — the REAL pre-fix code: mprotect(RW) THEN memcpy, as two steps.
 *               Exposes the TOCTOU window. Expected to VIOLATE.
 *   "procmem" — the FIX (restorePageContents): write the decompressed bytes
 *               into the backing WHILE the page is still PROT_NONE (Linux
 *               /proc/self/mem FOLL_FORCE write), THEN flip to PROT_RW.
 *               Concurrent readers keep faulting on PROT_NONE until the data is
 *               in place. Expected to HOLD.
 *
 * Safety invariant (NoStaleRead): a reader that completes a load against a page
 * must observe the page's current logical value — never the stale/zero backing
 * that exists transiently during a restore.
 *)

EXTENDS Integers, FiniteSets, Sequences, TLC

CONSTANTS
    NumPages,       \* small (1 is enough to exhibit the bug)
    Readers,        \* model-value set, e.g. {r1, r2}
    MaxCompress,    \* how many compress->fault cycles to allow per page
    MaxReads,       \* per-reader load count cap
    RestoreVariant  \* "atomic" | "split" | "procmem"

NONE  == -1
Pages == 1..NumPages

VARIABLES
    pageVal,        \* logical (intended) content of each page
    backingVal,     \* what a hardware load actually returns from the page's
                    \* physical backing right now. Diverges from pageVal during
                    \* a split restore: prot becomes RW before backingVal is set.
    prot,           \* per-page protection: "RW" | "NONE"
    pageState,      \* "ACTIVE" | "COMPRESSED"
    snapshot,       \* per-page compressed copy (NONE when not compressed)
    plock,          \* per-page lock: "free" | "comp" | "fault"
    compressCount,  \* per-page compress cycles done (cap by MaxCompress)
    staleRead,      \* per-page ghost: TRUE if any reader observed backing
                    \* that did not match the page's logical value
    rcount,         \* per-reader completed loads
    pc,             \* per-process program counter
    fp              \* fault-handler local: page being restored

vars == << pageVal, backingVal, prot, pageState, snapshot, plock,
           compressCount, staleRead, rcount, pc, fp >>

ProcSet == Readers \cup {"comp", "fault"}

Init ==
    /\ pageVal        = [p \in Pages |-> 1]   \* nonzero "structured data"
    /\ backingVal     = [p \in Pages |-> 1]
    /\ prot           = [p \in Pages |-> "RW"]
    /\ pageState      = [p \in Pages |-> "ACTIVE"]
    /\ snapshot       = [p \in Pages |-> NONE]
    /\ plock          = [p \in Pages |-> "free"]
    /\ compressCount  = [p \in Pages |-> 0]
    /\ staleRead      = [p \in Pages |-> FALSE]
    /\ rcount         = [r \in Readers |-> 0]
    /\ fp             = 1
    /\ pc = [self \in ProcSet |->
                 IF self = "comp"  THEN "CLoop"
                 ELSE IF self = "fault" THEN "FIdle"
                 ELSE "RLoop"]

\* ===== Compressor =====================================================
\* Compress an ACTIVE page: snapshot it, mark COMPRESSED, drop backing
\* (madvise) and mprotect NONE. We model the compress side as already-correct
\* (FixAv ordering: PROT_NONE before madvise) since this model is about the
\* RESTORE side. The compressor takes the per-page lock for the whole op.
CLoop ==
    /\ pc["comp"] = "CLoop"
    /\ IF \E p \in Pages : compressCount[p] < MaxCompress /\ pageState[p] = "ACTIVE"
          THEN /\ \E p \in Pages :
                    /\ compressCount[p] < MaxCompress
                    /\ pageState[p] = "ACTIVE"
                    /\ plock[p] = "free"
                    /\ plock' = [plock EXCEPT ![p] = "comp"]
                    /\ snapshot' = [snapshot EXCEPT ![p] = pageVal[p]]
                    /\ pageState' = [pageState EXCEPT ![p] = "COMPRESSED"]
                    /\ prot' = [prot EXCEPT ![p] = "NONE"]
                    \* madvise drops backing to a kernel zero page; harmless
                    \* because prot is already NONE (any access faults).
                    /\ backingVal' = [backingVal EXCEPT ![p] = 0]
                    /\ compressCount' = [compressCount EXCEPT ![p] = compressCount[p] + 1]
                    /\ pc' = [pc EXCEPT !["comp"] = "CUnlock"]
          ELSE /\ pc' = [pc EXCEPT !["comp"] = "Done"]
               /\ UNCHANGED << plock, snapshot, pageState, prot, backingVal,
                               compressCount >>
    /\ UNCHANGED << pageVal, staleRead, rcount, fp >>

CUnlock ==
    /\ pc["comp"] = "CUnlock"
    /\ \E p \in Pages : plock[p] = "comp"
    /\ \E p \in Pages : plock[p] = "comp" /\ plock' = [plock EXCEPT ![p] = "free"]
    /\ pc' = [pc EXCEPT !["comp"] = "CLoop"]
    /\ UNCHANGED << pageVal, backingVal, prot, pageState, snapshot,
                    compressCount, staleRead, rcount, fp >>

Compressor == CLoop \/ CUnlock

\* ===== Fault handler (decompress-on-fault restore) ====================
\* Picks a COMPRESSED page, takes the per-page lock, and restores it. The
\* restore is modeled in steps that differ by RestoreVariant. The critical
\* question: is the page ever PROT_RW while backingVal still holds the stale
\* (zero) bytes?
FIdle ==
    /\ pc["fault"] = "FIdle"
    /\ \E p \in Pages :
         pageState[p] = "COMPRESSED" /\ plock[p] = "free"
    /\ \E p \in Pages :
         /\ pageState[p] = "COMPRESSED" /\ plock[p] = "free"
         /\ plock' = [plock EXCEPT ![p] = "fault"]
         /\ fp' = p
    /\ pc' = [pc EXCEPT !["fault"] =
                 IF RestoreVariant = "atomic"  THEN "FAtomic"
                 ELSE IF RestoreVariant = "split" THEN "FSplitProtect"
                 ELSE "FProcmemWrite"]   \* "procmem"
    /\ UNCHANGED << pageVal, backingVal, prot, pageState, snapshot,
                    compressCount, staleRead, rcount >>

\* --- "atomic": old model. Everything in one step. No window. ---
FAtomic ==
    /\ pc["fault"] = "FAtomic"
    /\ LET p == fp IN
         /\ prot'       = [prot EXCEPT ![p] = "RW"]
         /\ backingVal' = [backingVal EXCEPT ![p] = snapshot[p]]
         /\ pageVal'    = [pageVal EXCEPT ![p] = snapshot[p]]
         /\ pageState'  = [pageState EXCEPT ![p] = "ACTIVE"]
         /\ snapshot'   = [snapshot EXCEPT ![p] = NONE]
    /\ pc' = [pc EXCEPT !["fault"] = "FUnlock"]
    /\ UNCHANGED << plock, compressCount, staleRead, rcount, fp >>

\* --- "split": the REAL pre-fix code. mprotect(RW) FIRST, memcpy SECOND. ---
\* Between these two steps the page is RW but backingVal is still the stale
\* (zero) value. A concurrent reader can load it.
FSplitProtect ==
    /\ pc["fault"] = "FSplitProtect"
    /\ LET p == fp IN
         /\ prot' = [prot EXCEPT ![p] = "RW"]   \* commitPages(): READABLE now
         \* backingVal NOT updated yet — still the stale zero from madvise.
    /\ pc' = [pc EXCEPT !["fault"] = "FSplitCopy"]
    /\ UNCHANGED << pageVal, backingVal, pageState, snapshot, plock,
                    compressCount, staleRead, rcount, fp >>

FSplitCopy ==
    /\ pc["fault"] = "FSplitCopy"
    /\ LET p == fp IN
         /\ backingVal' = [backingVal EXCEPT ![p] = snapshot[p]]  \* memcpy
         /\ pageVal'    = [pageVal EXCEPT ![p] = snapshot[p]]
         /\ pageState'  = [pageState EXCEPT ![p] = "ACTIVE"]
         /\ snapshot'   = [snapshot EXCEPT ![p] = NONE]
    /\ pc' = [pc EXCEPT !["fault"] = "FUnlock"]
    /\ UNCHANGED << prot, plock, compressCount, staleRead, rcount, fp >>

\* --- "procmem": the FIX. Populate backing WHILE PROT_NONE, THEN flip RW. ---
\* /proc/self/mem write lands the data while prot is still NONE, so any
\* concurrent reader still faults; only after the data is in place do we
\* make the page readable.
FProcmemWrite ==
    /\ pc["fault"] = "FProcmemWrite"
    /\ LET p == fp IN
         /\ backingVal' = [backingVal EXCEPT ![p] = snapshot[p]]  \* via /proc/self/mem, prot still NONE
         /\ pageVal'    = [pageVal EXCEPT ![p] = snapshot[p]]
    /\ pc' = [pc EXCEPT !["fault"] = "FProcmemProtect"]
    /\ UNCHANGED << prot, pageState, snapshot, plock, compressCount,
                    staleRead, rcount, fp >>

FProcmemProtect ==
    /\ pc["fault"] = "FProcmemProtect"
    /\ LET p == fp IN
         /\ prot'      = [prot EXCEPT ![p] = "RW"]   \* now readable, data already present
         /\ pageState' = [pageState EXCEPT ![p] = "ACTIVE"]
         /\ snapshot'  = [snapshot EXCEPT ![p] = NONE]
    /\ pc' = [pc EXCEPT !["fault"] = "FUnlock"]
    /\ UNCHANGED << pageVal, backingVal, plock, compressCount, staleRead,
                    rcount, fp >>

FUnlock ==
    /\ pc["fault"] = "FUnlock"
    /\ LET p == fp IN plock' = [plock EXCEPT ![p] = "free"]
    /\ pc' = [pc EXCEPT !["fault"] = "FIdle"]
    /\ UNCHANGED << pageVal, backingVal, prot, pageState, snapshot,
                    compressCount, staleRead, rcount, fp >>

FaultHandler == FIdle \/ FAtomic \/ FSplitProtect \/ FSplitCopy
                \/ FProcmemWrite \/ FProcmemProtect \/ FUnlock

\* ===== Reader =========================================================
\* A reader issues an ordinary load. If the page is PROT_NONE it faults (the
\* action is disabled until the fault handler makes the page readable — we do
\* NOT model the reader itself driving the fault handler; the compressor's
\* fault handler is the one that restores). If the page is readable (RW), the
\* load returns backingVal[p] — whatever the physical backing currently holds.
\* The reader does NOT take the per-page lock (it's a hardware load).
\*
\* staleRead flips TRUE if the observed backing differs from the page's logical
\* value: the reader saw stale/zero data where structured data should be.
RLoop(self) ==
    /\ pc[self] = "RLoop"
    /\ IF rcount[self] < MaxReads
          THEN pc' = [pc EXCEPT ![self] = "RLoad"]
          ELSE pc' = [pc EXCEPT ![self] = "Done"]
    /\ UNCHANGED << pageVal, backingVal, prot, pageState, snapshot, plock,
                    compressCount, staleRead, rcount, fp >>

RLoad(self) ==
    /\ pc[self] = "RLoad"
    /\ \E p \in Pages :
         /\ prot[p] = "RW"    \* PROT_NONE => hardware fault; load is disabled
         /\ staleRead' = [staleRead EXCEPT ![p] =
                             staleRead[p] \/ (backingVal[p] # pageVal[p])]
    /\ rcount' = [rcount EXCEPT ![self] = rcount[self] + 1]
    /\ pc' = [pc EXCEPT ![self] = "RLoop"]
    /\ UNCHANGED << pageVal, backingVal, prot, pageState, snapshot, plock,
                    compressCount, fp >>

Reader(self) == RLoop(self) \/ RLoad(self)

\* ===== Spec ===========================================================
Stutter == (\A self \in ProcSet : pc[self] \in {"Done","FIdle"})
           /\ ~(\E p \in Pages : pageState[p] = "COMPRESSED")
           /\ UNCHANGED vars

Next == \/ (\E self \in Readers : Reader(self))
        \/ Compressor
        \/ FaultHandler
        \/ Stutter

Spec == /\ Init /\ [][Next]_vars
        /\ \A self \in Readers : WF_vars(Reader(self))
        /\ WF_vars(Compressor)
        /\ WF_vars(FaultHandler)

\* ===== Safety invariant ===============================================
\* No reader ever observes physical backing that disagrees with the page's
\* logical value. Under "split" this is violated in the mprotect-RW -> memcpy
\* window. Under "atomic" and "procmem" it holds.
NoStaleRead == \A p \in Pages : ~staleRead[p]

=============================================================================
