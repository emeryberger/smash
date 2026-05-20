------------------------------ MODULE SmashCore ------------------------------
(*
 * PlusCal model of Smash's per-page state machine.
 *
 * Actors:
 *   - AppThread:    read/write pages (faults on protected pages)
 *   - Compressor:   3-phase background tick + Phase B (deferred reclaim)
 *
 * Safety:  BlobIntegrity, ProtectionConsistency
 * Liveness: FaultedThreadResumed, ColdPageCompressed
 *
 * States: ACTIVE, ACTIVE_MONITORING, COMPRESSING, COMPRESSED,
 *         COMPRESSED_SHADOW (deferred-reclaim: blob exists, page still RW)
 *
 * Bug found: Phase 3 used plain store for ACTIVE → ACTIVE_MONITORING.
 * Fix: CAS (modeled as check-then-set within one atomic label).
 *)

EXTENDS Integers, FiniteSets, TLC

CONSTANTS
    NumPages,       \* e.g., 2
    ColdThreshold   \* ticks before compression eligible (e.g., 1)

Pages == 1..NumPages
MaxCold == ColdThreshold + 1

(*--algorithm SmashCore

variables
    state     = [p \in Pages |-> "ACTIVE"],
    prot      = [p \in Pages |-> "PROT_RW"],
    lock      = [p \in Pages |-> "free"],
    coldCount = [p \in Pages |-> 0],
    accessed  = [p \in Pages |-> FALSE],
    hasBlob   = [p \in Pages |-> FALSE],
    hasPhysical = [p \in Pages |-> TRUE];

define
    IncCold(c) == IF c < MaxCold THEN c + 1 ELSE MaxCold

    \* Safety invariants
    BlobIntegrity ==
        \A p \in Pages :
            /\ (state[p] = "COMPRESSED")  => (hasBlob[p] /\ ~hasPhysical[p])
            /\ (state[p] = "ACTIVE")      => (~hasBlob[p] /\ hasPhysical[p])
            /\ (state[p] = "COMPRESSING") => (~hasBlob[p] /\ hasPhysical[p])
            \* Shadow: blob exists AND physical page still present (not yet reclaimed)
            /\ (state[p] = "COMPRESSED_SHADOW") => (hasBlob[p] /\ hasPhysical[p])

    ProtectionSafety ==
        \A p \in Pages :
            /\ (state[p] = "COMPRESSED") => (prot[p] = "PROT_NONE")
            /\ (state[p] = "ACTIVE")     => (prot[p] \in {"PROT_RW", "PROT_READ"})
            \* Shadow pages are accessible: PROT_RW before Phase B, PROT_READ during verify
            /\ (state[p] = "COMPRESSED_SHADOW") => (prot[p] \in {"PROT_RW", "PROT_READ"})

    SafetyInv == BlobIntegrity /\ ProtectionSafety
end define;

\* ─── App thread: writes to pages, faults on protected ones ───────────

fair process AppThread \in {"t1", "t2"}
variables target = 1;
begin
AppLoop:
    while TRUE do
        \* Non-deterministically pick a page to access
        with p \in Pages do
            target := p;
        end with;

    AppAccess:
        if prot[target] = "PROT_RW" then
            \* No fault — just mark accessed
            accessed[target] := TRUE;
        else
            \* Fault: need to acquire lock and handle
        FaultLock:
            await lock[target] = "free";
            lock[target] := self;

        FaultHandle:
            if state[target] = "COMPRESSED" then
                \* Decompress
                state[target] := "ACTIVE";
                prot[target] := "PROT_RW";
                hasPhysical[target] := TRUE;
                hasBlob[target] := FALSE;
                coldCount[target] := 0;
                accessed[target] := TRUE;
            elsif state[target] = "ACTIVE_MONITORING" then
                \* Was being monitored, restore
                state[target] := "ACTIVE";
                prot[target] := "PROT_RW";
                coldCount[target] := 0;
                accessed[target] := TRUE;
            elsif state[target] = "COMPRESSED_SHADOW" then
                \* Shadow page set to PROT_READ by Phase B (or Phase 3).
                \* Write faulted — discard stale blob, restore to ACTIVE.
                state[target] := "ACTIVE";
                prot[target] := "PROT_RW";
                hasBlob[target] := FALSE;
                coldCount[target] := 0;
                accessed[target] := TRUE;
            elsif state[target] = "ACTIVE" then
                \* Phase 3 mprotect race — just restore RW
                prot[target] := "PROT_RW";
            end if;
            lock[target] := "free";
        end if;
    end while;
end process;

\* ─── Compressor: 3-phase background tick ─────────────────────────────

fair process Compressor = "comp"
variables cp = 0;
begin
Tick:
    while TRUE do

    \* ── Phase 1: cold-count tracking ──────────────────────────────
    P1Start:
        cp := 1;
    Phase1:
        while cp <= NumPages do
            if accessed[cp] then
                coldCount[cp] := 0;
                accessed[cp] := FALSE;
            else
                coldCount[cp] := IncCold(coldCount[cp]);
            end if;
            cp := cp + 1;
        end while;

    \* ── Phase 2: compression ──────────────────────────────────────
    \* Standard path: COMPRESSING → COMPRESSED (mprotect PROT_NONE)
    \* Deferred path: COMPRESSING → COMPRESSED_SHADOW (page stays RW)
    P2Start:
        cp := 1;
    Phase2:
        while cp <= NumPages do
            if state[cp] \in {"ACTIVE", "ACTIVE_MONITORING"}
               /\ coldCount[cp] > ColdThreshold then
                \* Try to compress this page
            P2Lock:
                await lock[cp] = "free";
                if state[cp] \in {"ACTIVE", "ACTIVE_MONITORING"} then
                    lock[cp] := "comp";
                    state[cp] := "COMPRESSING";
                else
                    \* State changed; skip
                    goto P2Next;
                end if;

            P2Compress:
                \* Non-deterministic choice models both standard and deferred paths
                either
                    \* Standard path: reclaim immediately
                    state[cp] := "COMPRESSED";
                    prot[cp] := "PROT_NONE";
                    hasBlob[cp] := TRUE;
                    hasPhysical[cp] := FALSE;
                or
                    \* Deferred-reclaim path: keep page accessible
                    state[cp] := "COMPRESSED_SHADOW";
                    prot[cp] := "PROT_RW";  \* Restore RW (may have been PROT_READ from monitoring)
                    hasBlob[cp] := TRUE;
                    \* hasPhysical stays TRUE
                end either;
                lock[cp] := "free";
            end if;

        P2Next:
            cp := cp + 1;
        end while;

    \* ── Phase 3: set up monitoring (NO lock — uses CAS) ──────────
    P3Start:
        cp := 1;
    Phase3:
        while cp <= NumPages do
            if state[cp] = "ACTIVE" then
                \* CAS: ACTIVE → ACTIVE_MONITORING (atomic check+set)
            P3CAS:
                if state[cp] = "ACTIVE" then
                    state[cp] := "ACTIVE_MONITORING";
                else
                    goto P3Next;
                end if;
            P3Prot:
                prot[cp] := "PROT_READ";
            end if;
        P3Next:
            cp := cp + 1;
        end while;

    \* ── Phase B: deferred reclaim ───────────────────────────────
    \* Set PROT_READ (catch writes), verify content, then reclaim.
    \* The PROT_READ→verify→PROT_NONE sequence is split across labels
    \* to model the real interleave window faithfully.
    PBStart:
        cp := 1;
    PhaseB:
        while cp <= NumPages do
            if state[cp] = "COMPRESSED_SHADOW" then
            PBLock:
                await lock[cp] = "free";
                if state[cp] = "COMPRESSED_SHADOW" then
                    lock[cp] := "comp";
                else
                    goto PBNext;
                end if;

            PBProtRead:
                \* Set PROT_READ: writes will fault through handler
                \* (which discards blob and sets ACTIVE)
                prot[cp] := "PROT_READ";

            PBVerify:
                \* Interleave point: app thread can write here, triggering
                \* a fault that changes state to ACTIVE (handler discards blob).
                \* Re-check state after the window.
                if state[cp] /= "COMPRESSED_SHADOW" then
                    \* Handler fired between PBProtRead and here — abort
                    lock[cp] := "free";
                    goto PBNext;
                end if;

                \* Content check: if accessed flag set, page was mutated
                if accessed[cp] then
                    \* Page was written before PROT_READ — blob is stale
                    state[cp] := "ACTIVE";
                    prot[cp] := "PROT_RW";
                    hasBlob[cp] := FALSE;
                    coldCount[cp] := 0;
                    accessed[cp] := FALSE;
                    lock[cp] := "free";
                    goto PBNext;
                end if;

            PBReclaim:
                \* Content verified under PROT_READ. Any write between
                \* PBVerify and now would have faulted (PROT_READ), so
                \* the blob is guaranteed to match current page content.
                state[cp] := "COMPRESSED";
                prot[cp] := "PROT_NONE";
                hasPhysical[cp] := FALSE;
                lock[cp] := "free";
            end if;

        PBNext:
            cp := cp + 1;
        end while;

    end while;
end process;

end algorithm; *)
\* BEGIN TRANSLATION (chksum(pcal) = "70b04945" /\ chksum(tla) = "ff108263")
VARIABLES pc, state, prot, lock, coldCount, accessed, hasBlob, hasPhysical

(* define statement *)
IncCold(c) == IF c < MaxCold THEN c + 1 ELSE MaxCold


BlobIntegrity ==
    \A p \in Pages :
        /\ (state[p] = "COMPRESSED")  => (hasBlob[p] /\ ~hasPhysical[p])
        /\ (state[p] = "ACTIVE")      => (~hasBlob[p] /\ hasPhysical[p])
        /\ (state[p] = "COMPRESSING") => (~hasBlob[p] /\ hasPhysical[p])

ProtectionSafety ==
    \A p \in Pages :
        /\ (state[p] = "COMPRESSED") => (prot[p] = "PROT_NONE")
        /\ (state[p] = "ACTIVE")     => (prot[p] \in {"PROT_RW", "PROT_READ"})

SafetyInv == BlobIntegrity /\ ProtectionSafety

VARIABLES target, cp

vars == << pc, state, prot, lock, coldCount, accessed, hasBlob, hasPhysical, 
           target, cp >>

ProcSet == ({"t1", "t2"}) \cup {"comp"}

Init == (* Global variables *)
        /\ state = [p \in Pages |-> "ACTIVE"]
        /\ prot = [p \in Pages |-> "PROT_RW"]
        /\ lock = [p \in Pages |-> "free"]
        /\ coldCount = [p \in Pages |-> 0]
        /\ accessed = [p \in Pages |-> FALSE]
        /\ hasBlob = [p \in Pages |-> FALSE]
        /\ hasPhysical = [p \in Pages |-> TRUE]
        (* Process AppThread *)
        /\ target = [self \in {"t1", "t2"} |-> 1]
        (* Process Compressor *)
        /\ cp = 0
        /\ pc = [self \in ProcSet |-> CASE self \in {"t1", "t2"} -> "AppLoop"
                                        [] self = "comp" -> "Tick"]

AppLoop(self) == /\ pc[self] = "AppLoop"
                 /\ \E p \in Pages:
                      target' = [target EXCEPT ![self] = p]
                 /\ pc' = [pc EXCEPT ![self] = "AppAccess"]
                 /\ UNCHANGED << state, prot, lock, coldCount, accessed, 
                                 hasBlob, hasPhysical, cp >>

AppAccess(self) == /\ pc[self] = "AppAccess"
                   /\ IF prot[target[self]] = "PROT_RW"
                         THEN /\ accessed' = [accessed EXCEPT ![target[self]] = TRUE]
                              /\ pc' = [pc EXCEPT ![self] = "AppLoop"]
                         ELSE /\ pc' = [pc EXCEPT ![self] = "FaultLock"]
                              /\ UNCHANGED accessed
                   /\ UNCHANGED << state, prot, lock, coldCount, hasBlob, 
                                   hasPhysical, target, cp >>

FaultLock(self) == /\ pc[self] = "FaultLock"
                   /\ lock[target[self]] = "free"
                   /\ lock' = [lock EXCEPT ![target[self]] = self]
                   /\ pc' = [pc EXCEPT ![self] = "FaultHandle"]
                   /\ UNCHANGED << state, prot, coldCount, accessed, hasBlob, 
                                   hasPhysical, target, cp >>

FaultHandle(self) == /\ pc[self] = "FaultHandle"
                     /\ IF state[target[self]] = "COMPRESSED"
                           THEN /\ state' = [state EXCEPT ![target[self]] = "ACTIVE"]
                                /\ prot' = [prot EXCEPT ![target[self]] = "PROT_RW"]
                                /\ hasPhysical' = [hasPhysical EXCEPT ![target[self]] = TRUE]
                                /\ hasBlob' = [hasBlob EXCEPT ![target[self]] = FALSE]
                                /\ coldCount' = [coldCount EXCEPT ![target[self]] = 0]
                                /\ accessed' = [accessed EXCEPT ![target[self]] = TRUE]
                           ELSE /\ IF state[target[self]] = "ACTIVE_MONITORING"
                                      THEN /\ state' = [state EXCEPT ![target[self]] = "ACTIVE"]
                                           /\ prot' = [prot EXCEPT ![target[self]] = "PROT_RW"]
                                           /\ coldCount' = [coldCount EXCEPT ![target[self]] = 0]
                                           /\ accessed' = [accessed EXCEPT ![target[self]] = TRUE]
                                      ELSE /\ IF state[target[self]] = "ACTIVE"
                                                 THEN /\ prot' = [prot EXCEPT ![target[self]] = "PROT_RW"]
                                                 ELSE /\ TRUE
                                                      /\ prot' = prot
                                           /\ UNCHANGED << state, coldCount, 
                                                           accessed >>
                                /\ UNCHANGED << hasBlob, hasPhysical >>
                     /\ lock' = [lock EXCEPT ![target[self]] = "free"]
                     /\ pc' = [pc EXCEPT ![self] = "AppLoop"]
                     /\ UNCHANGED << target, cp >>

AppThread(self) == AppLoop(self) \/ AppAccess(self) \/ FaultLock(self)
                      \/ FaultHandle(self)

Tick == /\ pc["comp"] = "Tick"
        /\ pc' = [pc EXCEPT !["comp"] = "P1Start"]
        /\ UNCHANGED << state, prot, lock, coldCount, accessed, hasBlob, 
                        hasPhysical, target, cp >>

P1Start == /\ pc["comp"] = "P1Start"
           /\ cp' = 1
           /\ pc' = [pc EXCEPT !["comp"] = "Phase1"]
           /\ UNCHANGED << state, prot, lock, coldCount, accessed, hasBlob, 
                           hasPhysical, target >>

Phase1 == /\ pc["comp"] = "Phase1"
          /\ IF cp <= NumPages
                THEN /\ IF accessed[cp]
                           THEN /\ coldCount' = [coldCount EXCEPT ![cp] = 0]
                                /\ accessed' = [accessed EXCEPT ![cp] = FALSE]
                           ELSE /\ coldCount' = [coldCount EXCEPT ![cp] = IncCold(coldCount[cp])]
                                /\ UNCHANGED accessed
                     /\ cp' = cp + 1
                     /\ pc' = [pc EXCEPT !["comp"] = "Phase1"]
                ELSE /\ pc' = [pc EXCEPT !["comp"] = "P2Start"]
                     /\ UNCHANGED << coldCount, accessed, cp >>
          /\ UNCHANGED << state, prot, lock, hasBlob, hasPhysical, target >>

P2Start == /\ pc["comp"] = "P2Start"
           /\ cp' = 1
           /\ pc' = [pc EXCEPT !["comp"] = "Phase2"]
           /\ UNCHANGED << state, prot, lock, coldCount, accessed, hasBlob, 
                           hasPhysical, target >>

Phase2 == /\ pc["comp"] = "Phase2"
          /\ IF cp <= NumPages
                THEN /\ IF state[cp] \in {"ACTIVE", "ACTIVE_MONITORING"}
                           /\ coldCount[cp] > ColdThreshold
                           THEN /\ pc' = [pc EXCEPT !["comp"] = "P2Lock"]
                           ELSE /\ pc' = [pc EXCEPT !["comp"] = "P2Next"]
                ELSE /\ pc' = [pc EXCEPT !["comp"] = "P3Start"]
          /\ UNCHANGED << state, prot, lock, coldCount, accessed, hasBlob, 
                          hasPhysical, target, cp >>

P2Next == /\ pc["comp"] = "P2Next"
          /\ cp' = cp + 1
          /\ pc' = [pc EXCEPT !["comp"] = "Phase2"]
          /\ UNCHANGED << state, prot, lock, coldCount, accessed, hasBlob, 
                          hasPhysical, target >>

P2Lock == /\ pc["comp"] = "P2Lock"
          /\ lock[cp] = "free"
          /\ IF state[cp] \in {"ACTIVE", "ACTIVE_MONITORING"}
                THEN /\ lock' = [lock EXCEPT ![cp] = "comp"]
                     /\ state' = [state EXCEPT ![cp] = "COMPRESSING"]
                     /\ pc' = [pc EXCEPT !["comp"] = "P2Compress"]
                ELSE /\ pc' = [pc EXCEPT !["comp"] = "P2Next"]
                     /\ UNCHANGED << state, lock >>
          /\ UNCHANGED << prot, coldCount, accessed, hasBlob, hasPhysical, 
                          target, cp >>

P2Compress == /\ pc["comp"] = "P2Compress"
              /\ state' = [state EXCEPT ![cp] = "COMPRESSED"]
              /\ prot' = [prot EXCEPT ![cp] = "PROT_NONE"]
              /\ hasBlob' = [hasBlob EXCEPT ![cp] = TRUE]
              /\ hasPhysical' = [hasPhysical EXCEPT ![cp] = FALSE]
              /\ lock' = [lock EXCEPT ![cp] = "free"]
              /\ pc' = [pc EXCEPT !["comp"] = "P2Next"]
              /\ UNCHANGED << coldCount, accessed, target, cp >>

P3Start == /\ pc["comp"] = "P3Start"
           /\ cp' = 1
           /\ pc' = [pc EXCEPT !["comp"] = "Phase3"]
           /\ UNCHANGED << state, prot, lock, coldCount, accessed, hasBlob, 
                           hasPhysical, target >>

Phase3 == /\ pc["comp"] = "Phase3"
          /\ IF cp <= NumPages
                THEN /\ IF state[cp] = "ACTIVE"
                           THEN /\ pc' = [pc EXCEPT !["comp"] = "P3CAS"]
                           ELSE /\ pc' = [pc EXCEPT !["comp"] = "P3Next"]
                ELSE /\ pc' = [pc EXCEPT !["comp"] = "Tick"]
          /\ UNCHANGED << state, prot, lock, coldCount, accessed, hasBlob, 
                          hasPhysical, target, cp >>

P3Next == /\ pc["comp"] = "P3Next"
          /\ cp' = cp + 1
          /\ pc' = [pc EXCEPT !["comp"] = "Phase3"]
          /\ UNCHANGED << state, prot, lock, coldCount, accessed, hasBlob, 
                          hasPhysical, target >>

P3CAS == /\ pc["comp"] = "P3CAS"
         /\ IF state[cp] = "ACTIVE"
               THEN /\ state' = [state EXCEPT ![cp] = "ACTIVE_MONITORING"]
                    /\ pc' = [pc EXCEPT !["comp"] = "P3Prot"]
               ELSE /\ pc' = [pc EXCEPT !["comp"] = "P3Next"]
                    /\ state' = state
         /\ UNCHANGED << prot, lock, coldCount, accessed, hasBlob, hasPhysical, 
                         target, cp >>

P3Prot == /\ pc["comp"] = "P3Prot"
          /\ prot' = [prot EXCEPT ![cp] = "PROT_READ"]
          /\ pc' = [pc EXCEPT !["comp"] = "P3Next"]
          /\ UNCHANGED << state, lock, coldCount, accessed, hasBlob, 
                          hasPhysical, target, cp >>

Compressor == Tick \/ P1Start \/ Phase1 \/ P2Start \/ Phase2 \/ P2Next
                 \/ P2Lock \/ P2Compress \/ P3Start \/ Phase3 \/ P3Next
                 \/ P3CAS \/ P3Prot

Next == Compressor
           \/ (\E self \in {"t1", "t2"}: AppThread(self))

Spec == /\ Init /\ [][Next]_vars
        /\ \A self \in {"t1", "t2"} : WF_vars(AppThread(self))
        /\ WF_vars(Compressor)

\* END TRANSLATION

\* ─── Liveness properties ─────────────────────────────────────────────

\* Override: use strong fairness (WF can't guarantee progress when app
\* threads intermittently disable compressor lock acquisition)
FairSpec == /\ Init /\ [][Next]_vars
            /\ \A self \in {"t1", "t2"} : SF_vars(AppThread(self))
            /\ SF_vars(Compressor)

\* L1: Every faulted thread eventually resumes
FaultedThreadResumed ==
    \A t \in {"t1", "t2"} :
        (pc[t] = "FaultLock") ~> (pc[t] = "AppLoop")

\* L2: Cold monitored pages are eventually compressed or re-accessed
ColdPageCompressed ==
    \A p \in Pages :
        (state[p] = "ACTIVE_MONITORING" /\ coldCount[p] > ColdThreshold /\ ~accessed[p])
            ~> (state[p] \in {"COMPRESSED", "ACTIVE"})

=============================================================================

=============================================================================
