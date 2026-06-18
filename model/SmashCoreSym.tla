----------------------------- MODULE SmashCoreSym -----------------------------
(*
 * SmashCoreSym — symmetry-reduced companion to SmashCore.tla.
 *
 * SmashCore.tla uses Pages = 1..NumPages (integers) and an integer compressor
 * cursor cp, which makes TLC's SYMMETRY directive unusable (symmetry requires
 * the domain to be a set of model values).  This module is a semantically
 * equivalent refactor:
 *
 *   - Pages, AppThreads, Compressors are model-valued CONSTANTS (declared as
 *     symmetry sets in the .cfg), so SYMMETRY AllSymmetry collapses the ~N!
 *     interchangeable-page interleavings.
 *   - The compressor's per-phase integer cursor is replaced by a
 *     non-deterministic pick of any eligible page (\E p \in Pages : ...).  The
 *     phases of SmashCore become guard-enabled per-page actions here; their
 *     reachable (state, pageProt, hasBlob, hasPhysical) combinations are
 *     identical.
 *
 * Same two state machines and same safety invariant as SmashCore:
 *   state[p]    : EMPTY | ACTIVE | ACTIVE_MONITORING | COMPRESSING
 *               | COMPRESSED | COMPRESSED_SHADOW
 *   pageProt[p] : PROT_RW | PROT_READ | PROT_NONE
 *
 * SafetyInv == BlobIntegrity /\ ProtectionSafety /\ ActiveImpliesRW.
 *
 * BuggyMode = TRUE models the pre-fix processDecommitEntry (skips commitPages,
 * leaving PROT_NONE on a freed-while-compressed run); the allocator then pops
 * that run EMPTY->ACTIVE with PROT_NONE still set, violating ActiveImpliesRW.
 * This matches the May-2026 freelist-PROT_NONE bug.  BuggyMode = FALSE is the
 * fix and holds.
 *
 * NOTE (vs the historical SmashCore.tla): Free() here fires on ANY non-EMPTY
 * page, including COMPRESSED ones.  The real free path does not decompress, so
 * a compressed page (PROT_NONE) can be freed and routed through decommit with
 * its PROT_NONE intact — this is what makes the buggy violation *reachable*.
 * SmashCore.tla's Free guard was state="ACTIVE" only, which (latently) hid the
 * very bug the model was meant to demonstrate.  The companion fix is mirrored
 * back into SmashCore.tla.
 *)

EXTENDS Integers, FiniteSets, TLC

CONSTANTS
    Pages,          \* model-value set, e.g. {p1, p2, p3} (symmetry set)
    AppThreads,     \* model-value set, e.g. {t1, t2}      (symmetry set)
    Compressors,    \* model-value set, e.g. {c1, c2}      (symmetry set)
    ColdThreshold,  \* ticks before a page is compression-eligible
    MaxReleases,    \* per-page free->realloc cycle cap (state-space bound)
    BuggyMode       \* TRUE => pre-fix processDecommitEntry (skip commitPages)

MaxCold == ColdThreshold + 1

VARIABLES
    state,        \* [Pages -> PStates]
    pageProt,     \* [Pages -> Prots]
    lock,         \* [Pages -> {"free"} \cup Compressors \cup AppThreads]
    coldCount,    \* [Pages -> 0..MaxCold]
    accessed,     \* [Pages -> BOOLEAN]
    hasBlob,      \* [Pages -> BOOLEAN]
    hasPhysical,  \* [Pages -> BOOLEAN]
    releases,     \* [Pages -> 0..MaxReleases]
    decommitQ,    \* SUBSET Pages
    freeList      \* SUBSET Pages

vars == << state, pageProt, lock, coldCount, accessed, hasBlob, hasPhysical,
           releases, decommitQ, freeList >>

PStates == {"EMPTY","ACTIVE","ACTIVE_MONITORING","COMPRESSING",
            "COMPRESSED","COMPRESSED_SHADOW"}
Prots   == {"PROT_RW","PROT_READ","PROT_NONE"}

IncCold(c) == IF c < MaxCold THEN c + 1 ELSE MaxCold

\* ===== Safety invariant (identical to SmashCore) ======================
BlobIntegrity ==
    \A p \in Pages :
        /\ (state[p] = "COMPRESSED")        => (hasBlob[p] /\ ~hasPhysical[p])
        /\ (state[p] = "ACTIVE")            => (~hasBlob[p] /\ hasPhysical[p])
        /\ (state[p] = "COMPRESSING")       => (~hasBlob[p] /\ hasPhysical[p])
        /\ (state[p] = "COMPRESSED_SHADOW") => (hasBlob[p] /\ hasPhysical[p])
        /\ (state[p] = "EMPTY")             => (~hasBlob[p] /\ ~hasPhysical[p])

ProtectionSafety ==
    \A p \in Pages :
        /\ (state[p] = "COMPRESSED")        => (pageProt[p] = "PROT_NONE")
        /\ (state[p] = "ACTIVE")            => (pageProt[p] \in {"PROT_RW","PROT_READ"})
        /\ (state[p] = "COMPRESSED_SHADOW") => (pageProt[p] \in {"PROT_RW","PROT_READ"})

ActiveImpliesRW ==
    \A p \in Pages : (state[p] = "ACTIVE") => (pageProt[p] = "PROT_RW")

SafetyInv == BlobIntegrity /\ ProtectionSafety /\ ActiveImpliesRW

\* Type invariant.
TypeOK ==
    /\ state \in [Pages -> PStates]
    /\ pageProt \in [Pages -> Prots]
    /\ lock \in [Pages -> ({"free"} \cup Compressors \cup AppThreads)]
    /\ coldCount \in [Pages -> 0..MaxCold]
    /\ accessed \in [Pages -> BOOLEAN]
    /\ hasBlob \in [Pages -> BOOLEAN]
    /\ hasPhysical \in [Pages -> BOOLEAN]
    /\ releases \in [Pages -> 0..MaxReleases]
    /\ decommitQ \in SUBSET Pages
    /\ freeList \in SUBSET Pages

\* ===== Init ===========================================================
Init ==
    /\ state       = [p \in Pages |-> "ACTIVE"]
    /\ pageProt    = [p \in Pages |-> "PROT_RW"]
    /\ lock        = [p \in Pages |-> "free"]
    /\ coldCount   = [p \in Pages |-> 0]
    /\ accessed    = [p \in Pages |-> FALSE]
    /\ hasBlob     = [p \in Pages |-> FALSE]
    /\ hasPhysical = [p \in Pages |-> TRUE]
    /\ releases    = [p \in Pages |-> 0]
    /\ decommitQ   = {}
    /\ freeList    = {}

\* ===== App thread actions =============================================
\* Access a readable page (records liveness; resets the cold counter).
AppAccess(t) ==
    \E p \in Pages :
        /\ state[p] \notin {"EMPTY"}
        /\ pageProt[p] = "PROT_RW"
        /\ accessed' = [accessed EXCEPT ![p] = TRUE]
        /\ UNCHANGED << state, pageProt, lock, coldCount, hasBlob,
                        hasPhysical, releases, decommitQ, freeList >>

\* Fault on a protected page: the fault handler restores it to ACTIVE+PROT_RW.
\* Covers COMPRESSED, COMPRESSED_SHADOW, ACTIVE_MONITORING, and the Phase-3
\* ACTIVE/PROT_READ race.  Takes the per-page lock for the transition.
AppFault(t) ==
    \E p \in Pages :
        /\ lock[p] = "free"
        /\ \/ state[p] = "COMPRESSED"
           \/ state[p] = "COMPRESSED_SHADOW"
           \/ state[p] = "ACTIVE_MONITORING"
           \/ (state[p] = "ACTIVE" /\ pageProt[p] # "PROT_RW")
        /\ state'       = [state       EXCEPT ![p] = "ACTIVE"]
        /\ pageProt'    = [pageProt    EXCEPT ![p] = "PROT_RW"]
        /\ hasBlob'     = [hasBlob     EXCEPT ![p] = FALSE]
        /\ hasPhysical' = [hasPhysical EXCEPT ![p] = TRUE]
        /\ coldCount'   = [coldCount   EXCEPT ![p] = 0]
        /\ accessed'    = [accessed    EXCEPT ![p] = TRUE]
        /\ UNCHANGED << lock, releases, decommitQ, freeList >>

\* free(): ANY non-EMPTY page (incl. COMPRESSED) -> EMPTY, enqueue for decommit.
\* pageProt is NOT touched here (that is processDecommitEntry's job).
AppFree(t) ==
    \E p \in Pages :
        /\ state[p] # "EMPTY"
        /\ p \notin freeList
        /\ p \notin decommitQ
        /\ releases[p] < MaxReleases
        /\ lock[p] = "free"
        /\ state'       = [state       EXCEPT ![p] = "EMPTY"]
        /\ hasBlob'     = [hasBlob     EXCEPT ![p] = FALSE]
        /\ hasPhysical' = [hasPhysical EXCEPT ![p] = FALSE]
        /\ coldCount'   = [coldCount   EXCEPT ![p] = 0]
        /\ accessed'    = [accessed    EXCEPT ![p] = FALSE]
        /\ releases'    = [releases    EXCEPT ![p] = releases[p] + 1]
        /\ decommitQ'   = decommitQ \cup {p}
        /\ UNCHANGED << pageProt, lock, freeList >>

AppThread(t) == AppAccess(t) \/ AppFault(t) \/ AppFree(t)

\* ===== Allocator ======================================================
\* Pop a freelisted (EMPTY) run: EMPTY -> ACTIVE, restore physical.  pageProt
\* is intentionally NOT touched — relies on decommit having restored PROT_RW.
AllocPop ==
    \E p \in freeList :
        /\ state[p] = "EMPTY"
        /\ state'       = [state       EXCEPT ![p] = "ACTIVE"]
        /\ hasPhysical' = [hasPhysical EXCEPT ![p] = TRUE]
        /\ freeList'    = freeList \ {p}
        /\ UNCHANGED << pageProt, lock, coldCount, accessed, hasBlob,
                        releases, decommitQ >>

\* ===== Decommit thread ================================================
\* Drain a decommit-queue entry.  Fix (~BuggyMode) restores PROT_RW via
\* commitPages before pushing to the freelist; the bug skips it.
DecProcess ==
    \E p \in decommitQ :
        /\ decommitQ' = decommitQ \ {p}
        /\ freeList'  = freeList \cup {p}
        /\ pageProt'  = IF BuggyMode THEN pageProt
                                     ELSE [pageProt EXCEPT ![p] = "PROT_RW"]
        /\ UNCHANGED << state, lock, coldCount, accessed, hasBlob,
                        hasPhysical, releases >>

\* ===== Compressor (phases as guard-enabled per-page actions) ==========
\* Phase 1: cold-count tracking — non-deterministic page pick (no cursor).
CompColdScan(c) ==
    \E p \in Pages :
        /\ state[p] \notin {"EMPTY"}
        /\ IF accessed[p]
              THEN /\ coldCount' = [coldCount EXCEPT ![p] = 0]
                   /\ accessed'  = [accessed  EXCEPT ![p] = FALSE]
              ELSE /\ coldCount' = [coldCount EXCEPT ![p] = IncCold(coldCount[p])]
                   /\ accessed'  = accessed
        /\ UNCHANGED << state, pageProt, lock, hasBlob, hasPhysical,
                        releases, decommitQ, freeList >>

\* Phase 2: compress a cold ACTIVE/MONITORING page.  Two outcomes:
\*   immediate reclaim   -> COMPRESSED, PROT_NONE, drop physical
\*   deferred reclaim     -> COMPRESSED_SHADOW, PROT_RW, keep physical
CompCompress(c) ==
    \E p \in Pages :
        /\ state[p] \in {"ACTIVE","ACTIVE_MONITORING"}
        /\ coldCount[p] > ColdThreshold
        /\ lock[p] = "free"
        /\ p \notin freeList
        /\ p \notin decommitQ
        /\ \/ /\ state'       = [state       EXCEPT ![p] = "COMPRESSED"]
              /\ pageProt'    = [pageProt    EXCEPT ![p] = "PROT_NONE"]
              /\ hasBlob'     = [hasBlob     EXCEPT ![p] = TRUE]
              /\ hasPhysical' = [hasPhysical EXCEPT ![p] = FALSE]
           \/ /\ state'       = [state       EXCEPT ![p] = "COMPRESSED_SHADOW"]
              /\ pageProt'    = [pageProt    EXCEPT ![p] = "PROT_RW"]
              /\ hasBlob'     = [hasBlob     EXCEPT ![p] = TRUE]
              /\ hasPhysical' = hasPhysical
        /\ UNCHANGED << lock, coldCount, accessed, releases, decommitQ, freeList >>

\* Phase 3: set up monitoring on an ACTIVE page: -> ACTIVE_MONITORING + PROT_READ.
CompMonitor(c) ==
    \E p \in Pages :
        /\ state[p] = "ACTIVE"
        /\ pageProt[p] = "PROT_RW"
        /\ state'    = [state    EXCEPT ![p] = "ACTIVE_MONITORING"]
        /\ pageProt' = [pageProt EXCEPT ![p] = "PROT_READ"]
        /\ UNCHANGED << lock, coldCount, accessed, hasBlob, hasPhysical,
                        releases, decommitQ, freeList >>

\* Phase B: reclaim a deferred shadow page: COMPRESSED_SHADOW -> COMPRESSED.
CompPhaseB(c) ==
    \E p \in Pages :
        /\ state[p] = "COMPRESSED_SHADOW"
        /\ lock[p] = "free"
        /\ state'       = [state       EXCEPT ![p] = "COMPRESSED"]
        /\ pageProt'    = [pageProt    EXCEPT ![p] = "PROT_NONE"]
        /\ hasPhysical' = [hasPhysical EXCEPT ![p] = FALSE]
        /\ UNCHANGED << lock, coldCount, accessed, hasBlob, releases,
                        decommitQ, freeList >>

Compressor(c) ==
    CompColdScan(c) \/ CompCompress(c) \/ CompMonitor(c) \/ CompPhaseB(c)

\* ===== Spec ===========================================================
Next ==
    \/ \E t \in AppThreads  : AppThread(t)
    \/ \E c \in Compressors : Compressor(c)
    \/ AllocPop
    \/ DecProcess

Spec == Init /\ [][Next]_vars

\* ===== Symmetry =======================================================
\* Pages / AppThreads / Compressors are model values, so these are valid
\* symmetry sets.  AllSymmetry collapses the interchangeable interleavings.
PageSymmetry   == Permutations(Pages)
ThreadSymmetry == Permutations(AppThreads)
CompSymmetry   == Permutations(Compressors)
AllSymmetry    == PageSymmetry \cup ThreadSymmetry \cup CompSymmetry

=============================================================================
