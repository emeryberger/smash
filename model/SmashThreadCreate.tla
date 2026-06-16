------------------------------ MODULE SmashThreadCreate ------------------------------
(*
 * PlusCal model extension to SmashCore: models the bug where
 * CompressorThread::tick() calls pthread_create, triggering
 * glibc _dl_allocate_tls_init which reads link_map structs
 * allocated on smash-managed pages.
 *
 * Key insight: pthread_create's TLS init must read N pages
 * (one per loaded DSO's link_map). If any of those pages are
 * PROT_NONE, the signal handler decompresses them. But empirically,
 * after K successful signal-handler decompressions in a single
 * syscall context (pthread_create), the (K+1)th SIGSEGV is not
 * delivered to the handler. The process dies.
 *
 * This model captures the invariant:
 *   "At the moment the compressor calls pthread_create, no more
 *    than MaxFaultable pages in TlsPages may be PROT_NONE."
 *
 * Fix modeled: PreCreateWorkers = TRUE means all pthread_create
 * calls happen before any compression (at init time), so the
 * invariant is trivially satisfied.
 *)

EXTENDS Integers, FiniteSets

CONSTANTS
    NumPages,         \* total managed pages
    TlsPages,         \* subset of Pages whose data may be read by TLS init
                      \* (pages allocated by glibc for link_map structs)
    ColdThreshold,    \* ticks before compression eligible
    MaxFaultable,     \* max PROT_NONE pages the handler can survive (empirically ~3)
    PreCreateWorkers  \* TRUE => fix applied (workers pre-created)

Pages == 1..NumPages

ASSUME TlsPages \subseteq Pages
ASSUME MaxFaultable >= 0

(*--algorithm SmashThreadCreate

variables
    state     = [p \in Pages |-> "ACTIVE"],
    pageProt  = [p \in Pages |-> "PROT_RW"],
    coldCount = [p \in Pages |-> 0],
    hasBlob   = [p \in Pages |-> FALSE],
    tick      = 0,
    workersCreated = PreCreateWorkers,  \* with fix, already done
    justCreated = FALSE;  \* ghost: TRUE in the step that creates workers

define
    \* Count how many TLS-relevant pages are currently PROT_NONE
    TlsFaultCount == Cardinality({p \in TlsPages : pageProt[p] = "PROT_NONE"})

    \* SAFETY: at the moment workers are created (MaybeCreateWorker just
    \* set workersCreated := TRUE), at most MaxFaultable TLS pages may be
    \* PROT_NONE. We use the pc (program counter) to detect the exact
    \* state: pc["compressor"] was at "MaybeCreateWorker" and workersCreated
    \* is now TRUE. But since PlusCal changes pc atomically with the
    \* assignment, we can't observe the "just assigned" instant.
    \*
    \* Instead: the invariant is "if workersCreated is FALSE and pages are
    \* about to be read, the count must be safe." Equivalently: at any state
    \* where workersCreated = FALSE, we could safely create workers iff
    \* TlsFaultCount <= MaxFaultable. If the model ever has a state with
    \* workersCreated=FALSE and TlsFaultCount > MaxFaultable, AND the
    \* compressor can reach MaybeCreateWorker from there, the bug exists.
    \*
    \* Simpler: just check at the MaybeCreateWorker step. Since the
    \* compressor sets workersCreated := TRUE at that step, check the
    \* state AFTER: workersCreated=TRUE AND the TlsFaultCount at that
    \* moment. Since DeferredSweep can't fire between MaybeCreateWorker
    \* and the invariant check (they're the same step), the count captured
    \* is the count at pthread_create time.
    \* The invariant fires ONLY in the state where justCreated is TRUE
    \* (the step that just did pthread_create). At that instant, TLS
    \* pages must be accessible (at most MaxFaultable PROT_NONE).
    ThreadCreateSafe ==
        justCreated => (TlsFaultCount <= MaxFaultable)

    SafetyInv == ThreadCreateSafe
end define;

\* --- Compressor thread: compress cold pages, scale workers ---

fair process Compressor = "compressor"
begin
CompressorLoop:
    while TRUE do
        \* Phase 2: compress cold pages
    Phase2:
        with p \in Pages do
            if state[p] = "ACTIVE" /\ coldCount[p] >= ColdThreshold then
                state[p]   := "COMPRESSED";
                hasBlob[p] := TRUE;
            end if;
        end with;

        \* Adaptive worker scaling FIRST (real code order: adaptWorkerCount
        \* runs before sweepDeferredMadvise in tick()). The pthread_create
        \* here sees PROT_NONE pages from PREVIOUS ticks' sweeps.
    MaybeCreateWorker:
        \* Workers are created lazily — only when there's compression workload.
        \* In reality this means pages have been compressed in previous ticks.
        \* Model: create workers only if some page IS already PROT_NONE
        \* (meaning a previous tick's sweep already ran).
        if ~workersCreated /\ (\E p \in Pages : pageProt[p] = "PROT_NONE") then
            workersCreated := TRUE;
            justCreated := TRUE;
        else
            justCreated := FALSE;
        end if;

        \* THEN deferred madvise sweep: apply PROT_NONE to compressed pages
    DeferredSweep:
        pageProt := [p \in Pages |-> IF state[p] = "COMPRESSED"
                                     THEN "PROT_NONE"
                                     ELSE pageProt[p]];

        \* Advance tick (cold counting)
    AdvanceTick:
        if tick < ColdThreshold + 3 then
            tick := tick + 1;
        end if;
        justCreated := FALSE;
        coldCount := [p \in Pages |-> IF state[p] = "ACTIVE"
                                      THEN IF coldCount[p] < ColdThreshold + 1
                                           THEN coldCount[p] + 1
                                           ELSE coldCount[p]
                                      ELSE coldCount[p]];
    end while;
end process;

\* --- App thread: accesses pages, triggers fault handler ---

fair process AppThread = "app"
variables target = 1;
begin
AppLoop:
    while TRUE do
        with p \in Pages do target := p; end with;
    AppAccess:
        if pageProt[target] = "PROT_NONE" /\ state[target] = "COMPRESSED" then
            \* Fault handler decompresses
            state[target]   := "ACTIVE";
            pageProt[target] := "PROT_RW";
            hasBlob[target]  := FALSE;
        elsif pageProt[target] \in {"PROT_RW", "PROT_READ"} then
            \* Normal access
            coldCount[target] := 0;
        end if;
    end while;
end process;

end algorithm; *)
===============================================================================
