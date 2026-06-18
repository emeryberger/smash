------------------------------ MODULE SmashCore ------------------------------
(*
 * PlusCal model of Smash's per-page state machine, extended to model
 * the kernel's per-page mprotect bits as a *separate* state machine
 * from smash's PageState. The two-state-machine view is needed to
 * surface bugs that consist of state/protection desynchronization
 * across the boundary (e.g., the May-2026 freelist-PROT_NONE bug).
 *
 * Actors:
 *   - AppThread:    read/write pages (faults on protected pages),
 *                   periodically frees a page (pushes to decommit queue).
 *   - Compressor:   3-phase background tick + Phase B (deferred reclaim).
 *   - Decommit:     the background decommit thread that drains the
 *                   decommit queue (madvise DONTNEED) and pushes to
 *                   the per-shard freelist.
 *   - Allocator:    pops freelist (or bump-allocates) and returns the
 *                   range to the application.
 *
 * Two state machines per page:
 *   - state[p]   : smash PageState — EMPTY, ACTIVE, ACTIVE_MONITORING,
 *                  COMPRESSING, COMPRESSED, COMPRESSED_SHADOW.
 *   - pageProt[p]: kernel mprotect bits — PROT_RW, PROT_READ, PROT_NONE.
 *
 * Cross-machine safety: when state[p] = ACTIVE the application may
 * legitimately access page p, so pageProt[p] must be PROT_RW. The
 * May-2026 bug violates exactly this invariant on the freelist-pop
 * path: state goes EMPTY -> ACTIVE on allocation but pageProt stays
 * PROT_NONE because processDecommitEntry didn't call commitPages
 * before pushing the run onto the freelist.
 *
 * BuggyMode constant selects the buggy behavior: when TRUE,
 * processDecommitEntry skips the leading commitPages call (matches
 * pre-fix smash). When FALSE, the fix is in place.
 *)

EXTENDS Integers, FiniteSets, Sequences, TLC

CONSTANTS
    NumPages,       \* e.g., 2
    ColdThreshold,  \* ticks before compression eligible (e.g., 2)
    MaxReleases,    \* per-page bound on free->realloc cycles (state-space cap)
    BuggyMode       \* TRUE => model the pre-fix processDecommitEntry

Pages == 1..NumPages
MaxCold == ColdThreshold + 1

(*--algorithm SmashCore

variables
    state       = [p \in Pages |-> "ACTIVE"],
    pageProt    = [p \in Pages |-> "PROT_RW"],
    lock        = [p \in Pages |-> "free"],
    coldCount   = [p \in Pages |-> 0],
    accessed    = [p \in Pages |-> FALSE],
    hasBlob     = [p \in Pages |-> FALSE],
    hasPhysical = [p \in Pages |-> TRUE],
    \* Number of free->realloc cycles consumed by each page (bounded).
    releases    = [p \in Pages |-> 0],
    \* MPSC decommit queue: set of pages pending processDecommitEntry.
    decommitQ   = {},
    \* Freelist: pages that are EMPTY and ready for allocator pop.
    freeList    = {};

define
    IncCold(c) == IF c < MaxCold THEN c + 1 ELSE MaxCold

    \* Safety invariants
    BlobIntegrity ==
        \A p \in Pages :
            /\ (state[p] = "COMPRESSED")  => (hasBlob[p] /\ ~hasPhysical[p])
            /\ (state[p] = "ACTIVE")      => (~hasBlob[p] /\ hasPhysical[p])
            /\ (state[p] = "COMPRESSING") => (~hasBlob[p] /\ hasPhysical[p])
            /\ (state[p] = "COMPRESSED_SHADOW") => (hasBlob[p] /\ hasPhysical[p])
            \* EMPTY pages carry no compressed blob and have no physical
            \* backing (decommit thread MADV_DONTNEED'd them).
            /\ (state[p] = "EMPTY")       => (~hasBlob[p] /\ ~hasPhysical[p])

    ProtectionSafety ==
        \A p \in Pages :
            /\ (state[p] = "COMPRESSED") => (pageProt[p] = "PROT_NONE")
            /\ (state[p] = "ACTIVE")     => (pageProt[p] \in {"PROT_RW", "PROT_READ"})
            /\ (state[p] = "COMPRESSED_SHADOW") => (pageProt[p] \in {"PROT_RW", "PROT_READ"})

    \* The bug-witness invariant. The application can legitimately
    \* read/write a page in state ACTIVE; therefore the kernel mprotect
    \* bits must permit reads AND writes. PROT_READ alone is not
    \* enough because the only way state=ACTIVE coexists with
    \* pageProt=PROT_READ is the Phase-3 monitor race (in which case
    \* state would be ACTIVE_MONITORING, not ACTIVE — see Phase3 below).
    \*
    \* The May-2026 freelist-pop bug violates this: state EMPTY->ACTIVE
    \* on allocator pop, but pageProt was left at PROT_NONE by the
    \* compressor and never restored.
    ActiveImpliesRW ==
        \A p \in Pages : (state[p] = "ACTIVE") => (pageProt[p] = "PROT_RW")

    SafetyInv == BlobIntegrity /\ ProtectionSafety /\ ActiveImpliesRW
end define;

\* --- App thread: writes to pages, faults on protected ones, may free ---

fair process AppThread \in {"t1", "t2"}
variables target = 1;
begin
AppLoop:
    while TRUE do
        \* Non-deterministically pick a page to access OR free.
        with p \in Pages do
            target := p;
        end with;

        either
            \* === Access path ============================================
        AppAccess:
            if state[target] = "EMPTY" then
                \* Page not currently allocated; skip.
                skip;
            elsif pageProt[target] = "PROT_RW" then
                accessed[target] := TRUE;
            else
                \* Fault: acquire lock and handle.
            FaultLock:
                await lock[target] = "free";
                lock[target] := self;

            FaultHandle:
                if state[target] = "COMPRESSED" then
                    \* Decompress: commitPages restores PROT_RW.
                    state[target]       := "ACTIVE";
                    pageProt[target]    := "PROT_RW";
                    hasPhysical[target] := TRUE;
                    hasBlob[target]     := FALSE;
                    coldCount[target]   := 0;
                    accessed[target]    := TRUE;
                elsif state[target] = "ACTIVE_MONITORING" then
                    state[target]    := "ACTIVE";
                    pageProt[target] := "PROT_RW";
                    coldCount[target] := 0;
                    accessed[target]  := TRUE;
                elsif state[target] = "COMPRESSED_SHADOW" then
                    state[target]    := "ACTIVE";
                    pageProt[target] := "PROT_RW";
                    hasBlob[target]  := FALSE;
                    coldCount[target] := 0;
                    accessed[target]  := TRUE;
                elsif state[target] = "ACTIVE" then
                    \* Phase-3 mprotect race (model preserves this case).
                    pageProt[target] := "PROT_RW";
                else
                    \* state = EMPTY: this is the BUG witness. The fault
                    \* handler bails through the default branch — the
                    \* kernel then chains to SIGSEGV. We model it as a
                    \* no-op so the violating state persists for TLC.
                    skip;
                end if;
                lock[target] := "free";
            end if;
        or
            \* === Free path: app calls free() on this page ==============
            \* Mirrors releaseHook chain: clear smash state to EMPTY,
            \* push range to decommit queue. PROT bits are NOT touched
            \* here; that's processDecommitEntry's job (or, in the bug,
            \* nobody's job).
        AppFree:
            \* Acquire + release the per-page lock within one atomic step.
            \* PlusCal forbids assigning the same variable (lock[target])
            \* twice under one label, so we model the lock as held only for
            \* the duration of this atomic block (guarded by lock="free") and
            \* leave it "free" on exit — equivalent to acquire-do-release with
            \* no observable intermediate state. (823515f introduced a double
            \* lock[target] assignment here that broke PlusCal translation.)
            \* free() fires on ANY allocated (non-EMPTY) page, including a
            \* COMPRESSED one (PROT_NONE) or a MONITORING one (PROT_READ): the
            \* real free path does NOT decompress, so a non-RW page can be
            \* routed through decommit with its protection intact. This is what
            \* makes the BuggyMode freelist-PROT_NONE violation *reachable*; the
            \* historical guard state="ACTIVE" only (always PROT_RW) latently
            \* hid the very bug this model exists to demonstrate.
            if state[target] /= "EMPTY"
               /\ target \notin freeList
               /\ target \notin decommitQ
               /\ releases[target] < MaxReleases
               /\ lock[target] = "free" then
                state[target]    := "EMPTY";
                hasBlob[target]  := FALSE;
                hasPhysical[target] := FALSE;
                coldCount[target] := 0;
                accessed[target]  := FALSE;
                releases[target]  := releases[target] + 1;
                decommitQ := decommitQ \cup {target};
            end if;
        end either;
    end while;
end process;

\* --- Allocator process: pops freelist or bump-allocates ----------------
\* In real smash, the allocator is invoked from app threads via malloc.
\* We model it as its own process for clarity; effect is the same.

fair process Allocator = "alloc"
variables ap = 0;
begin
AllocLoop:
    while TRUE do
        either
            \* Pop from freelist (the BUG SURFACE — no commitPages here).
        AllocPop:
            if freeList /= {} then
                with p \in freeList do
                    ap := p;
                end with;
                freeList := freeList \ {ap};
                \* Transition EMPTY -> ACTIVE. pageProt is intentionally
                \* NOT touched on this path: in the buggy code,
                \* processDecommitEntry didn't restore PROT_RW, so by the
                \* time the allocator pops the run the kernel mprotect
                \* bits may still be PROT_NONE (set when smash compressed
                \* the page before free).
                if state[ap] = "EMPTY" then
                    state[ap]       := "ACTIVE";
                    hasPhysical[ap] := TRUE;
                end if;
            end if;
        or
            \* Bump-allocate path. allocatePages calls commitPages on
            \* fresh ranges — PROT_RW is guaranteed. Model: pick a page
            \* whose state is EMPTY and not on either queue (i.e., a
            \* fresh-region page) and bump it. To keep the state space
            \* finite we just no-op when nothing to do.
        AllocBump:
            skip;
        end either;
    end while;
end process;

\* --- Decommit thread: drains decommit queue, optionally restores RW ---

fair process Decommit = "dec"
variables dp = 0;
begin
DecLoop:
    while TRUE do
        if decommitQ /= {} then
        DecPick:
            with p \in decommitQ do
                dp := p;
            end with;
            decommitQ := decommitQ \ {dp};

        DecCommitPages:
            \* The fix: vm::commitPages(addr, n) restores PROT_RW. Pages
            \* may have been at PROT_NONE (compressor) or PROT_READ
            \* (monitoring) at free time; this resyncs the kernel bits.
            if ~BuggyMode then
                pageProt[dp] := "PROT_RW";
            end if;

        DecMadvise:
            \* madvise(MADV_DONTNEED): pages already had hasPhysical
            \* cleared by the free path; this is a no-op in the model.
            skip;

        DecPush:
            \* Push the run onto the freelist.
            freeList := freeList \cup {dp};
        end if;
    end while;
end process;

\* --- Compressor workers: smash has a coordinator + N helper threads ----

fair process Compressor \in {"comp1", "comp2"}
variables cp = 0;
begin
Tick:
    while TRUE do

    \* -- Phase 1: cold-count tracking --------------------------------
    P1Start:
        cp := 1;
    Phase1:
        while cp <= NumPages do
            if state[cp] = "EMPTY" then
                \* Skip empty slots in the cold scan.
                skip;
            elsif accessed[cp] then
                coldCount[cp] := 0;
                accessed[cp]  := FALSE;
            else
                coldCount[cp] := IncCold(coldCount[cp]);
            end if;
            cp := cp + 1;
        end while;

    \* -- Phase 2: compression ----------------------------------------
    P2Start:
        cp := 1;
    Phase2:
        while cp <= NumPages do
            if state[cp] \in {"ACTIVE", "ACTIVE_MONITORING"}
               /\ coldCount[cp] > ColdThreshold then
            P2Lock:
                await lock[cp] = "free";
                if state[cp] \in {"ACTIVE", "ACTIVE_MONITORING"} then
                    lock[cp]  := self;
                    state[cp] := "COMPRESSING";
                else
                    goto P2Next;
                end if;

            P2Compress:
                \* Compressor reads page contents under PROT_READ
                \* (model: setPagesReadOnly), then either reclaims
                \* immediately (PROT_NONE) or defers (PROT_RW + shadow).
                pageProt[cp] := "PROT_READ";
            P2CompressFinish:
                either
                    \* Standard path: reclaim immediately.
                    state[cp]       := "COMPRESSED";
                    pageProt[cp]    := "PROT_NONE";
                    hasBlob[cp]     := TRUE;
                    hasPhysical[cp] := FALSE;
                or
                    \* Deferred-reclaim path: keep page accessible.
                    state[cp]    := "COMPRESSED_SHADOW";
                    pageProt[cp] := "PROT_RW";
                    hasBlob[cp]  := TRUE;
                end either;
                lock[cp] := "free";
            end if;

        P2Next:
            cp := cp + 1;
        end while;

    \* -- Phase 3: set up monitoring (no lock — uses CAS) --------------
    P3Start:
        cp := 1;
    Phase3:
        while cp <= NumPages do
            if state[cp] = "ACTIVE" then
            P3CAS:
                if state[cp] = "ACTIVE" then
                    state[cp] := "ACTIVE_MONITORING";
                else
                    goto P3Next;
                end if;
            P3Prot:
                if state[cp] = "ACTIVE_MONITORING" then
                    pageProt[cp] := "PROT_READ";
                end if;
            end if;
        P3Next:
            cp := cp + 1;
        end while;

    \* -- Phase B: deferred reclaim -----------------------------------
    PBStart:
        cp := 1;
    PhaseB:
        while cp <= NumPages do
            if state[cp] = "COMPRESSED_SHADOW" then
            PBLock:
                await lock[cp] = "free";
                if state[cp] = "COMPRESSED_SHADOW" then
                    lock[cp] := self;
                else
                    goto PBNext;
                end if;

            PBProtRead:
                pageProt[cp] := "PROT_READ";

            PBVerify:
                if state[cp] /= "COMPRESSED_SHADOW" \/ accessed[cp] then
                    if state[cp] = "COMPRESSED_SHADOW" then
                        state[cp]    := "ACTIVE";
                        pageProt[cp] := "PROT_RW";
                        hasBlob[cp]  := FALSE;
                        coldCount[cp] := 0;
                        accessed[cp]  := FALSE;
                    end if;
                    lock[cp] := "free";
                    goto PBNext;
                end if;

            PBReclaim:
                state[cp]       := "COMPRESSED";
                pageProt[cp]    := "PROT_NONE";
                hasPhysical[cp] := FALSE;
                lock[cp] := "free";
            end if;

        PBNext:
            cp := cp + 1;
        end while;

    end while;
end process;

end algorithm; *)
\* BEGIN TRANSLATION (manual)
VARIABLES pc, state, pageProt, lock, coldCount, accessed, hasBlob, 
          hasPhysical, releases, decommitQ, freeList

(* define statement *)
IncCold(c) == IF c < MaxCold THEN c + 1 ELSE MaxCold


BlobIntegrity ==
    \A p \in Pages :
        /\ (state[p] = "COMPRESSED")  => (hasBlob[p] /\ ~hasPhysical[p])
        /\ (state[p] = "ACTIVE")      => (~hasBlob[p] /\ hasPhysical[p])
        /\ (state[p] = "COMPRESSING") => (~hasBlob[p] /\ hasPhysical[p])
        /\ (state[p] = "COMPRESSED_SHADOW") => (hasBlob[p] /\ hasPhysical[p])


        /\ (state[p] = "EMPTY")       => (~hasBlob[p] /\ ~hasPhysical[p])

ProtectionSafety ==
    \A p \in Pages :
        /\ (state[p] = "COMPRESSED") => (pageProt[p] = "PROT_NONE")
        /\ (state[p] = "ACTIVE")     => (pageProt[p] \in {"PROT_RW", "PROT_READ"})
        /\ (state[p] = "COMPRESSED_SHADOW") => (pageProt[p] \in {"PROT_RW", "PROT_READ"})











ActiveImpliesRW ==
    \A p \in Pages : (state[p] = "ACTIVE") => (pageProt[p] = "PROT_RW")

SafetyInv == BlobIntegrity /\ ProtectionSafety /\ ActiveImpliesRW

VARIABLES target, ap, dp, cp

vars == << pc, state, pageProt, lock, coldCount, accessed, hasBlob, 
           hasPhysical, releases, decommitQ, freeList, target, ap, dp, cp >>

ProcSet == ({"t1", "t2"}) \cup {"alloc"} \cup {"dec"} \cup ({"comp1", "comp2"})

Init == (* Global variables *)
        /\ state = [p \in Pages |-> "ACTIVE"]
        /\ pageProt = [p \in Pages |-> "PROT_RW"]
        /\ lock = [p \in Pages |-> "free"]
        /\ coldCount = [p \in Pages |-> 0]
        /\ accessed = [p \in Pages |-> FALSE]
        /\ hasBlob = [p \in Pages |-> FALSE]
        /\ hasPhysical = [p \in Pages |-> TRUE]
        /\ releases = [p \in Pages |-> 0]
        /\ decommitQ = {}
        /\ freeList = {}
        (* Process AppThread *)
        /\ target = [self \in {"t1", "t2"} |-> 1]
        (* Process Allocator *)
        /\ ap = 0
        (* Process Decommit *)
        /\ dp = 0
        (* Process Compressor *)
        /\ cp = [self \in {"comp1", "comp2"} |-> 0]
        /\ pc = [self \in ProcSet |-> CASE self \in {"t1", "t2"} -> "AppLoop"
                                        [] self = "alloc" -> "AllocLoop"
                                        [] self = "dec" -> "DecLoop"
                                        [] self \in {"comp1", "comp2"} -> "Tick"]

AppLoop(self) == /\ pc[self] = "AppLoop"
                 /\ \E p \in Pages:
                      target' = [target EXCEPT ![self] = p]
                 /\ \/ /\ pc' = [pc EXCEPT ![self] = "AppAccess"]
                    \/ /\ pc' = [pc EXCEPT ![self] = "AppFree"]
                 /\ UNCHANGED << state, pageProt, lock, coldCount, accessed, 
                                 hasBlob, hasPhysical, releases, decommitQ, 
                                 freeList, ap, dp, cp >>

AppAccess(self) == /\ pc[self] = "AppAccess"
                   /\ IF state[target[self]] = "EMPTY"
                         THEN /\ TRUE
                              /\ pc' = [pc EXCEPT ![self] = "AppLoop"]
                              /\ UNCHANGED accessed
                         ELSE /\ IF pageProt[target[self]] = "PROT_RW"
                                    THEN /\ accessed' = [accessed EXCEPT ![target[self]] = TRUE]
                                         /\ pc' = [pc EXCEPT ![self] = "AppLoop"]
                                    ELSE /\ pc' = [pc EXCEPT ![self] = "FaultLock"]
                                         /\ UNCHANGED accessed
                   /\ UNCHANGED << state, pageProt, lock, coldCount, hasBlob, 
                                   hasPhysical, releases, decommitQ, freeList, 
                                   target, ap, dp, cp >>

FaultLock(self) == /\ pc[self] = "FaultLock"
                   /\ lock[target[self]] = "free"
                   /\ lock' = [lock EXCEPT ![target[self]] = self]
                   /\ pc' = [pc EXCEPT ![self] = "FaultHandle"]
                   /\ UNCHANGED << state, pageProt, coldCount, accessed, 
                                   hasBlob, hasPhysical, releases, decommitQ, 
                                   freeList, target, ap, dp, cp >>

FaultHandle(self) == /\ pc[self] = "FaultHandle"
                     /\ IF state[target[self]] = "COMPRESSED"
                           THEN /\ state' = [state EXCEPT ![target[self]] = "ACTIVE"]
                                /\ pageProt' = [pageProt EXCEPT ![target[self]] = "PROT_RW"]
                                /\ hasPhysical' = [hasPhysical EXCEPT ![target[self]] = TRUE]
                                /\ hasBlob' = [hasBlob EXCEPT ![target[self]] = FALSE]
                                /\ coldCount' = [coldCount EXCEPT ![target[self]] = 0]
                                /\ accessed' = [accessed EXCEPT ![target[self]] = TRUE]
                           ELSE /\ IF state[target[self]] = "ACTIVE_MONITORING"
                                      THEN /\ state' = [state EXCEPT ![target[self]] = "ACTIVE"]
                                           /\ pageProt' = [pageProt EXCEPT ![target[self]] = "PROT_RW"]
                                           /\ coldCount' = [coldCount EXCEPT ![target[self]] = 0]
                                           /\ accessed' = [accessed EXCEPT ![target[self]] = TRUE]
                                           /\ UNCHANGED hasBlob
                                      ELSE /\ IF state[target[self]] = "COMPRESSED_SHADOW"
                                                 THEN /\ state' = [state EXCEPT ![target[self]] = "ACTIVE"]
                                                      /\ pageProt' = [pageProt EXCEPT ![target[self]] = "PROT_RW"]
                                                      /\ hasBlob' = [hasBlob EXCEPT ![target[self]] = FALSE]
                                                      /\ coldCount' = [coldCount EXCEPT ![target[self]] = 0]
                                                      /\ accessed' = [accessed EXCEPT ![target[self]] = TRUE]
                                                 ELSE /\ IF state[target[self]] = "ACTIVE"
                                                            THEN /\ pageProt' = [pageProt EXCEPT ![target[self]] = "PROT_RW"]
                                                            ELSE /\ TRUE
                                                                 /\ UNCHANGED pageProt
                                                      /\ UNCHANGED << state, 
                                                                      coldCount, 
                                                                      accessed, 
                                                                      hasBlob >>
                                /\ UNCHANGED hasPhysical
                     /\ lock' = [lock EXCEPT ![target[self]] = "free"]
                     /\ pc' = [pc EXCEPT ![self] = "AppLoop"]
                     /\ UNCHANGED << releases, decommitQ, freeList, target, ap, 
                                     dp, cp >>

AppFree(self) == /\ pc[self] = "AppFree"
                 /\ IF state[target[self]] /= "EMPTY"
                       /\ target[self] \notin freeList
                       /\ target[self] \notin decommitQ
                       /\ releases[target[self]] < MaxReleases
                       /\ lock[target[self]] = "free"
                       THEN /\ state' = [state EXCEPT ![target[self]] = "EMPTY"]
                            /\ hasBlob' = [hasBlob EXCEPT ![target[self]] = FALSE]
                            /\ hasPhysical' = [hasPhysical EXCEPT ![target[self]] = FALSE]
                            /\ coldCount' = [coldCount EXCEPT ![target[self]] = 0]
                            /\ accessed' = [accessed EXCEPT ![target[self]] = FALSE]
                            /\ releases' = [releases EXCEPT ![target[self]] = releases[target[self]] + 1]
                            /\ decommitQ' = (decommitQ \cup {target[self]})
                       ELSE /\ TRUE
                            /\ UNCHANGED << state, coldCount, accessed, 
                                            hasBlob, hasPhysical, releases, 
                                            decommitQ >>
                 /\ pc' = [pc EXCEPT ![self] = "AppLoop"]
                 /\ UNCHANGED << pageProt, lock, freeList, target, ap, dp, cp >>

AppThread(self) == AppLoop(self) \/ AppAccess(self) \/ FaultLock(self)
                      \/ FaultHandle(self) \/ AppFree(self)

AllocLoop == /\ pc["alloc"] = "AllocLoop"
             /\ \/ /\ pc' = [pc EXCEPT !["alloc"] = "AllocPop"]
                \/ /\ pc' = [pc EXCEPT !["alloc"] = "AllocBump"]
             /\ UNCHANGED << state, pageProt, lock, coldCount, accessed, 
                             hasBlob, hasPhysical, releases, decommitQ, 
                             freeList, target, ap, dp, cp >>

AllocPop == /\ pc["alloc"] = "AllocPop"
            /\ IF freeList /= {}
                  THEN /\ \E p \in freeList:
                            ap' = p
                       /\ freeList' = freeList \ {ap'}
                       /\ IF state[ap'] = "EMPTY"
                             THEN /\ state' = [state EXCEPT ![ap'] = "ACTIVE"]
                                  /\ hasPhysical' = [hasPhysical EXCEPT ![ap'] = TRUE]
                             ELSE /\ TRUE
                                  /\ UNCHANGED << state, hasPhysical >>
                  ELSE /\ TRUE
                       /\ UNCHANGED << state, hasPhysical, freeList, ap >>
            /\ pc' = [pc EXCEPT !["alloc"] = "AllocLoop"]
            /\ UNCHANGED << pageProt, lock, coldCount, accessed, hasBlob, 
                            releases, decommitQ, target, dp, cp >>

AllocBump == /\ pc["alloc"] = "AllocBump"
             /\ TRUE
             /\ pc' = [pc EXCEPT !["alloc"] = "AllocLoop"]
             /\ UNCHANGED << state, pageProt, lock, coldCount, accessed, 
                             hasBlob, hasPhysical, releases, decommitQ, 
                             freeList, target, ap, dp, cp >>

Allocator == AllocLoop \/ AllocPop \/ AllocBump

DecLoop == /\ pc["dec"] = "DecLoop"
           /\ IF decommitQ /= {}
                 THEN /\ pc' = [pc EXCEPT !["dec"] = "DecPick"]
                 ELSE /\ pc' = [pc EXCEPT !["dec"] = "DecLoop"]
           /\ UNCHANGED << state, pageProt, lock, coldCount, accessed, hasBlob, 
                           hasPhysical, releases, decommitQ, freeList, target, 
                           ap, dp, cp >>

DecPick == /\ pc["dec"] = "DecPick"
           /\ \E p \in decommitQ:
                dp' = p
           /\ decommitQ' = decommitQ \ {dp'}
           /\ pc' = [pc EXCEPT !["dec"] = "DecCommitPages"]
           /\ UNCHANGED << state, pageProt, lock, coldCount, accessed, hasBlob, 
                           hasPhysical, releases, freeList, target, ap, cp >>

DecCommitPages == /\ pc["dec"] = "DecCommitPages"
                  /\ IF ~BuggyMode
                        THEN /\ pageProt' = [pageProt EXCEPT ![dp] = "PROT_RW"]
                        ELSE /\ TRUE
                             /\ UNCHANGED pageProt
                  /\ pc' = [pc EXCEPT !["dec"] = "DecMadvise"]
                  /\ UNCHANGED << state, lock, coldCount, accessed, hasBlob, 
                                  hasPhysical, releases, decommitQ, freeList, 
                                  target, ap, dp, cp >>

DecMadvise == /\ pc["dec"] = "DecMadvise"
              /\ TRUE
              /\ pc' = [pc EXCEPT !["dec"] = "DecPush"]
              /\ UNCHANGED << state, pageProt, lock, coldCount, accessed, 
                              hasBlob, hasPhysical, releases, decommitQ, 
                              freeList, target, ap, dp, cp >>

DecPush == /\ pc["dec"] = "DecPush"
           /\ freeList' = (freeList \cup {dp})
           /\ pc' = [pc EXCEPT !["dec"] = "DecLoop"]
           /\ UNCHANGED << state, pageProt, lock, coldCount, accessed, hasBlob, 
                           hasPhysical, releases, decommitQ, target, ap, dp, 
                           cp >>

Decommit == DecLoop \/ DecPick \/ DecCommitPages \/ DecMadvise \/ DecPush

Tick(self) == /\ pc[self] = "Tick"
              /\ pc' = [pc EXCEPT ![self] = "P1Start"]
              /\ UNCHANGED << state, pageProt, lock, coldCount, accessed, 
                              hasBlob, hasPhysical, releases, decommitQ, 
                              freeList, target, ap, dp, cp >>

P1Start(self) == /\ pc[self] = "P1Start"
                 /\ cp' = [cp EXCEPT ![self] = 1]
                 /\ pc' = [pc EXCEPT ![self] = "Phase1"]
                 /\ UNCHANGED << state, pageProt, lock, coldCount, accessed, 
                                 hasBlob, hasPhysical, releases, decommitQ, 
                                 freeList, target, ap, dp >>

Phase1(self) == /\ pc[self] = "Phase1"
                /\ IF cp[self] <= NumPages
                      THEN /\ IF state[cp[self]] = "EMPTY"
                                 THEN /\ TRUE
                                      /\ UNCHANGED << coldCount, accessed >>
                                 ELSE /\ IF accessed[cp[self]]
                                            THEN /\ coldCount' = [coldCount EXCEPT ![cp[self]] = 0]
                                                 /\ accessed' = [accessed EXCEPT ![cp[self]] = FALSE]
                                            ELSE /\ coldCount' = [coldCount EXCEPT ![cp[self]] = IncCold(coldCount[cp[self]])]
                                                 /\ UNCHANGED accessed
                           /\ cp' = [cp EXCEPT ![self] = cp[self] + 1]
                           /\ pc' = [pc EXCEPT ![self] = "Phase1"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "P2Start"]
                           /\ UNCHANGED << coldCount, accessed, cp >>
                /\ UNCHANGED << state, pageProt, lock, hasBlob, hasPhysical, 
                                releases, decommitQ, freeList, target, ap, dp >>

P2Start(self) == /\ pc[self] = "P2Start"
                 /\ cp' = [cp EXCEPT ![self] = 1]
                 /\ pc' = [pc EXCEPT ![self] = "Phase2"]
                 /\ UNCHANGED << state, pageProt, lock, coldCount, accessed, 
                                 hasBlob, hasPhysical, releases, decommitQ, 
                                 freeList, target, ap, dp >>

Phase2(self) == /\ pc[self] = "Phase2"
                /\ IF cp[self] <= NumPages
                      THEN /\ IF state[cp[self]] \in {"ACTIVE", "ACTIVE_MONITORING"}
                                 /\ coldCount[cp[self]] > ColdThreshold
                                 THEN /\ pc' = [pc EXCEPT ![self] = "P2Lock"]
                                 ELSE /\ pc' = [pc EXCEPT ![self] = "P2Next"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "P3Start"]
                /\ UNCHANGED << state, pageProt, lock, coldCount, accessed, 
                                hasBlob, hasPhysical, releases, decommitQ, 
                                freeList, target, ap, dp, cp >>

P2Next(self) == /\ pc[self] = "P2Next"
                /\ cp' = [cp EXCEPT ![self] = cp[self] + 1]
                /\ pc' = [pc EXCEPT ![self] = "Phase2"]
                /\ UNCHANGED << state, pageProt, lock, coldCount, accessed, 
                                hasBlob, hasPhysical, releases, decommitQ, 
                                freeList, target, ap, dp >>

P2Lock(self) == /\ pc[self] = "P2Lock"
                /\ lock[cp[self]] = "free"
                /\ IF state[cp[self]] \in {"ACTIVE", "ACTIVE_MONITORING"}
                      THEN /\ lock' = [lock EXCEPT ![cp[self]] = self]
                           /\ state' = [state EXCEPT ![cp[self]] = "COMPRESSING"]
                           /\ pc' = [pc EXCEPT ![self] = "P2Compress"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "P2Next"]
                           /\ UNCHANGED << state, lock >>
                /\ UNCHANGED << pageProt, coldCount, accessed, hasBlob, 
                                hasPhysical, releases, decommitQ, freeList, 
                                target, ap, dp, cp >>

P2Compress(self) == /\ pc[self] = "P2Compress"
                    /\ pageProt' = [pageProt EXCEPT ![cp[self]] = "PROT_READ"]
                    /\ pc' = [pc EXCEPT ![self] = "P2CompressFinish"]
                    /\ UNCHANGED << state, lock, coldCount, accessed, hasBlob, 
                                    hasPhysical, releases, decommitQ, freeList, 
                                    target, ap, dp, cp >>

P2CompressFinish(self) == /\ pc[self] = "P2CompressFinish"
                          /\ \/ /\ state' = [state EXCEPT ![cp[self]] = "COMPRESSED"]
                                /\ pageProt' = [pageProt EXCEPT ![cp[self]] = "PROT_NONE"]
                                /\ hasBlob' = [hasBlob EXCEPT ![cp[self]] = TRUE]
                                /\ hasPhysical' = [hasPhysical EXCEPT ![cp[self]] = FALSE]
                             \/ /\ state' = [state EXCEPT ![cp[self]] = "COMPRESSED_SHADOW"]
                                /\ pageProt' = [pageProt EXCEPT ![cp[self]] = "PROT_RW"]
                                /\ hasBlob' = [hasBlob EXCEPT ![cp[self]] = TRUE]
                                /\ UNCHANGED hasPhysical
                          /\ lock' = [lock EXCEPT ![cp[self]] = "free"]
                          /\ pc' = [pc EXCEPT ![self] = "P2Next"]
                          /\ UNCHANGED << coldCount, accessed, releases, 
                                          decommitQ, freeList, target, ap, dp, 
                                          cp >>

P3Start(self) == /\ pc[self] = "P3Start"
                 /\ cp' = [cp EXCEPT ![self] = 1]
                 /\ pc' = [pc EXCEPT ![self] = "Phase3"]
                 /\ UNCHANGED << state, pageProt, lock, coldCount, accessed, 
                                 hasBlob, hasPhysical, releases, decommitQ, 
                                 freeList, target, ap, dp >>

Phase3(self) == /\ pc[self] = "Phase3"
                /\ IF cp[self] <= NumPages
                      THEN /\ IF state[cp[self]] = "ACTIVE"
                                 THEN /\ pc' = [pc EXCEPT ![self] = "P3CAS"]
                                 ELSE /\ pc' = [pc EXCEPT ![self] = "P3Next"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "PBStart"]
                /\ UNCHANGED << state, pageProt, lock, coldCount, accessed, 
                                hasBlob, hasPhysical, releases, decommitQ, 
                                freeList, target, ap, dp, cp >>

P3Next(self) == /\ pc[self] = "P3Next"
                /\ cp' = [cp EXCEPT ![self] = cp[self] + 1]
                /\ pc' = [pc EXCEPT ![self] = "Phase3"]
                /\ UNCHANGED << state, pageProt, lock, coldCount, accessed, 
                                hasBlob, hasPhysical, releases, decommitQ, 
                                freeList, target, ap, dp >>

P3CAS(self) == /\ pc[self] = "P3CAS"
               /\ IF state[cp[self]] = "ACTIVE"
                     THEN /\ state' = [state EXCEPT ![cp[self]] = "ACTIVE_MONITORING"]
                          /\ pc' = [pc EXCEPT ![self] = "P3Prot"]
                     ELSE /\ pc' = [pc EXCEPT ![self] = "P3Next"]
                          /\ state' = state
               /\ UNCHANGED << pageProt, lock, coldCount, accessed, hasBlob, 
                               hasPhysical, releases, decommitQ, freeList, 
                               target, ap, dp, cp >>

P3Prot(self) == /\ pc[self] = "P3Prot"
                /\ IF state[cp[self]] = "ACTIVE_MONITORING"
                      THEN /\ pageProt' = [pageProt EXCEPT ![cp[self]] = "PROT_READ"]
                      ELSE /\ TRUE
                           /\ UNCHANGED pageProt
                /\ pc' = [pc EXCEPT ![self] = "P3Next"]
                /\ UNCHANGED << state, lock, coldCount, accessed, hasBlob, 
                                hasPhysical, releases, decommitQ, freeList, 
                                target, ap, dp, cp >>

PBStart(self) == /\ pc[self] = "PBStart"
                 /\ cp' = [cp EXCEPT ![self] = 1]
                 /\ pc' = [pc EXCEPT ![self] = "PhaseB"]
                 /\ UNCHANGED << state, pageProt, lock, coldCount, accessed, 
                                 hasBlob, hasPhysical, releases, decommitQ, 
                                 freeList, target, ap, dp >>

PhaseB(self) == /\ pc[self] = "PhaseB"
                /\ IF cp[self] <= NumPages
                      THEN /\ IF state[cp[self]] = "COMPRESSED_SHADOW"
                                 THEN /\ pc' = [pc EXCEPT ![self] = "PBLock"]
                                 ELSE /\ pc' = [pc EXCEPT ![self] = "PBNext"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "Tick"]
                /\ UNCHANGED << state, pageProt, lock, coldCount, accessed, 
                                hasBlob, hasPhysical, releases, decommitQ, 
                                freeList, target, ap, dp, cp >>

PBNext(self) == /\ pc[self] = "PBNext"
                /\ cp' = [cp EXCEPT ![self] = cp[self] + 1]
                /\ pc' = [pc EXCEPT ![self] = "PhaseB"]
                /\ UNCHANGED << state, pageProt, lock, coldCount, accessed, 
                                hasBlob, hasPhysical, releases, decommitQ, 
                                freeList, target, ap, dp >>

PBLock(self) == /\ pc[self] = "PBLock"
                /\ lock[cp[self]] = "free"
                /\ IF state[cp[self]] = "COMPRESSED_SHADOW"
                      THEN /\ lock' = [lock EXCEPT ![cp[self]] = self]
                           /\ pc' = [pc EXCEPT ![self] = "PBProtRead"]
                      ELSE /\ pc' = [pc EXCEPT ![self] = "PBNext"]
                           /\ lock' = lock
                /\ UNCHANGED << state, pageProt, coldCount, accessed, hasBlob, 
                                hasPhysical, releases, decommitQ, freeList, 
                                target, ap, dp, cp >>

PBProtRead(self) == /\ pc[self] = "PBProtRead"
                    /\ pageProt' = [pageProt EXCEPT ![cp[self]] = "PROT_READ"]
                    /\ pc' = [pc EXCEPT ![self] = "PBVerify"]
                    /\ UNCHANGED << state, lock, coldCount, accessed, hasBlob, 
                                    hasPhysical, releases, decommitQ, freeList, 
                                    target, ap, dp, cp >>

PBVerify(self) == /\ pc[self] = "PBVerify"
                  /\ IF state[cp[self]] /= "COMPRESSED_SHADOW" \/ accessed[cp[self]]
                        THEN /\ IF state[cp[self]] = "COMPRESSED_SHADOW"
                                   THEN /\ state' = [state EXCEPT ![cp[self]] = "ACTIVE"]
                                        /\ pageProt' = [pageProt EXCEPT ![cp[self]] = "PROT_RW"]
                                        /\ hasBlob' = [hasBlob EXCEPT ![cp[self]] = FALSE]
                                        /\ coldCount' = [coldCount EXCEPT ![cp[self]] = 0]
                                        /\ accessed' = [accessed EXCEPT ![cp[self]] = FALSE]
                                   ELSE /\ TRUE
                                        /\ UNCHANGED << state, pageProt, 
                                                        coldCount, accessed, 
                                                        hasBlob >>
                             /\ lock' = [lock EXCEPT ![cp[self]] = "free"]
                             /\ pc' = [pc EXCEPT ![self] = "PBNext"]
                        ELSE /\ pc' = [pc EXCEPT ![self] = "PBReclaim"]
                             /\ UNCHANGED << state, pageProt, lock, coldCount, 
                                             accessed, hasBlob >>
                  /\ UNCHANGED << hasPhysical, releases, decommitQ, freeList, 
                                  target, ap, dp, cp >>

PBReclaim(self) == /\ pc[self] = "PBReclaim"
                   /\ state' = [state EXCEPT ![cp[self]] = "COMPRESSED"]
                   /\ pageProt' = [pageProt EXCEPT ![cp[self]] = "PROT_NONE"]
                   /\ hasPhysical' = [hasPhysical EXCEPT ![cp[self]] = FALSE]
                   /\ lock' = [lock EXCEPT ![cp[self]] = "free"]
                   /\ pc' = [pc EXCEPT ![self] = "PBNext"]
                   /\ UNCHANGED << coldCount, accessed, hasBlob, releases, 
                                   decommitQ, freeList, target, ap, dp, cp >>

Compressor(self) == Tick(self) \/ P1Start(self) \/ Phase1(self)
                       \/ P2Start(self) \/ Phase2(self) \/ P2Next(self)
                       \/ P2Lock(self) \/ P2Compress(self)
                       \/ P2CompressFinish(self) \/ P3Start(self)
                       \/ Phase3(self) \/ P3Next(self) \/ P3CAS(self)
                       \/ P3Prot(self) \/ PBStart(self) \/ PhaseB(self)
                       \/ PBNext(self) \/ PBLock(self) \/ PBProtRead(self)
                       \/ PBVerify(self) \/ PBReclaim(self)

Next == Allocator \/ Decommit
           \/ (\E self \in {"t1", "t2"}: AppThread(self))
           \/ (\E self \in {"comp1", "comp2"}: Compressor(self))

Spec == /\ Init /\ [][Next]_vars
        /\ \A self \in {"t1", "t2"} : WF_vars(AppThread(self))
        /\ WF_vars(Allocator)
        /\ WF_vars(Decommit)
        /\ \A self \in {"comp1", "comp2"} : WF_vars(Compressor(self))

\* END TRANSLATION

\* --- Liveness properties ---------------------------------------------

FairSpec == /\ Init /\ [][Next]_vars
            /\ \A self \in {"t1", "t2"} : SF_vars(AppThread(self))
            /\ SF_vars(Allocator)
            /\ SF_vars(Decommit)
            /\ \A self \in {"comp1", "comp2"} : SF_vars(Compressor(self))

\* L1: Every faulted thread eventually resumes
FaultedThreadResumed ==
    \A t \in {"t1", "t2"} :
        (pc[t] = "FaultLock") ~> (pc[t] = "AppLoop")

\* L2: Cold monitored pages eventually compressed or re-accessed
ColdPageCompressed ==
    \A p \in Pages :
        (state[p] = "ACTIVE_MONITORING" /\ coldCount[p] > ColdThreshold /\ ~accessed[p])
            ~> (state[p] \in {"COMPRESSED", "ACTIVE", "EMPTY"})

\* --- Symmetry reduction (NOT USABLE FROM THIS MODULE) ---------------
\* TLC's SYMMETRY directive requires the underlying constants to be
\* sets of model values. In this module Pages = 1..NumPages and the
\* compressor uses an integer cursor cp = 1..NumPages, so Pages
\* elements are integers and ThreadSymmetry / CompSymmetry would have
\* TLA-string elements — both rejected by TLC ("Symmetry function
\* must have model values as domain and range").
\*
\* The companion module SmashCoreSym.tla is a semantically-equivalent
\* refactor that takes Pages / AppThreads / Compressors as model-valued
\* CONSTANTS and replaces the integer cp cursor with a non-deterministic
\* pick from a "pending" set; SYMMETRY AllSymmetry there yields the ~N!
\* page-symmetry reduction we need at 3 pages and beyond.
\*
\* The PageSymmetry / ThreadSymmetry / CompSymmetry / AllSymmetry
\* operators are intentionally NOT defined in this module; do not add
\* them. See SmashCoreSym.tla.

\* --- Extra safety invariants (auxiliary; opt-in via cfg) ------------
\* These don't affect the existing SafetyInv; they're included only when
\* a config explicitly names them as INVARIANTs.

\* No page in COMPRESSED state should be sitting on the decommit queue:
\* free path drains the blob, so a queued page must be EMPTY.
NoCompressedOnDecommitQ ==
    \A p \in Pages : (p \in decommitQ) => (state[p] /= "COMPRESSED")

\* No EMPTY page should still own a compressed blob (would be a leak).
NoOrphanedBlob ==
    \A p \in Pages : (state[p] = "EMPTY") => ~hasBlob[p]

\* Pages on the freelist must be EMPTY (queue discipline check).
FreelistEmptyDiscipline ==
    \A p \in Pages : (p \in freeList) => (state[p] = "EMPTY")

\* Aggregated extra invariants — opt-in.
ExtraSafetyInv == NoCompressedOnDecommitQ /\ NoOrphanedBlob /\ FreelistEmptyDiscipline

=============================================================================
