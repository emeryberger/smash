--------------------------- MODULE SmashForkPause ---------------------------
(*
 * The compressor fork-pause handshake (compressor_thread.h coordEntry /
 * pauseForFork).
 *
 * Two actors:
 *   - Coordinator: loops { maybe tick }.  tick() acquires per-page locks,
 *     CompressStore shard locks and /proc/self/mem handles, so fork() MUST
 *     NOT happen while the coordinator is inside the tick body: the child
 *     inherits those spinlocks in the locked state with no owner.
 *   - Prepare: the pthread_atfork prepare handler.  Sets paused, waits for
 *     inTick = FALSE, then lets fork() proceed.
 *
 * The CheckThenSet constant selects the coordinator's ordering:
 *   TRUE  (pre-fix): if (paused) continue;  inTick := TRUE;  tick();
 *   FALSE (fixed)  : inTick := TRUE;  if (paused) { inTick := FALSE;
 *                    continue; }  tick();
 *
 * The pre-fix order is a TOCTOU: the coordinator reads paused = FALSE, the
 * prepare handler then sets paused and reads inTick = 0 (the coordinator
 * has not stored it yet), fork() proceeds — and the coordinator enters the
 * tick body concurrently with the fork.  The fixed order is Dekker-style:
 * both sides store first and load second, so (under sequential consistency
 * — the code uses seq_cst for exactly this reason) at least one side
 * observes the other's store.
 *
 * NOT modeled: the 50 ms cap on the prepare wait loop.  The cap is a
 * deliberate deadlock-avoidance tradeoff (a wedged tick must not hang
 * fork() forever); under the cap the guarantee is best-effort by design.
 * This model verifies the handshake ORDERING assuming the wait completes
 * by observing inTick = FALSE.
 *
 * TLC (run with -deadlock; the forked state is terminal for Prepare):
 *   SmashForkPause_checkthenset_buggy.cfg  -> NoForkDuringTick violated
 *   SmashForkPause_setthencheck_fixed.cfg  -> No error
 *)

EXTENDS TLC

CONSTANT CheckThenSet   \* TRUE = pre-fix coordinator ordering (the bug)

VARIABLES paused, inTick, forked, pcC, pcP

vars == <<paused, inTick, forked, pcC, pcP>>

Init == /\ paused = FALSE
        /\ inTick = FALSE
        /\ forked = FALSE
        /\ pcC = "Idle"
        /\ pcP = "Idle"

\* --- Coordinator ------------------------------------------------------

CIdle == /\ pcC = "Idle"
         /\ pcC' = IF CheckThenSet THEN "BCheck" ELSE "FSet"
         /\ UNCHANGED <<paused, inTick, forked, pcP>>

\* Pre-fix order: check paused FIRST ...
BCheck == /\ pcC = "BCheck"
          /\ pcC' = IF paused THEN "Idle" ELSE "BSet"
          /\ UNCHANGED <<paused, inTick, forked, pcP>>

\* ... THEN publish inTick.  The gap between BCheck and BSet is the bug.
BSet == /\ pcC = "BSet"
        /\ inTick' = TRUE
        /\ pcC' = "Body"
        /\ UNCHANGED <<paused, forked, pcP>>

\* Fixed order: publish inTick FIRST ...
FSet == /\ pcC = "FSet"
        /\ inTick' = TRUE
        /\ pcC' = "FCheck"
        /\ UNCHANGED <<paused, forked, pcP>>

\* ... THEN check paused; back off (clearing inTick) if a fork is pending.
FCheck == /\ pcC = "FCheck"
          /\ IF paused
                THEN /\ inTick' = FALSE
                     /\ pcC' = "Idle"
                ELSE /\ inTick' = inTick
                     /\ pcC' = "Body"
          /\ UNCHANGED <<paused, forked, pcP>>

\* tick(): page locks, store shard locks held here.
Body == /\ pcC = "Body"
        /\ pcC' = "Clear"
        /\ UNCHANGED <<paused, inTick, forked, pcP>>

Clear == /\ pcC = "Clear"
         /\ inTick' = FALSE
         /\ pcC' = "Idle"
         /\ UNCHANGED <<paused, forked, pcP>>

\* --- Prepare handler / fork -------------------------------------------

PSet == /\ pcP = "Idle"
        /\ paused' = TRUE
        /\ pcP' = "Wait"
        /\ UNCHANGED <<inTick, forked, pcC>>

\* The wait loop completes only by observing inTick = FALSE (cap not modeled).
PWait == /\ pcP = "Wait"
         /\ inTick = FALSE
         /\ pcP' = "Fork"
         /\ UNCHANGED <<paused, inTick, forked, pcC>>

PFork == /\ pcP = "Fork"
         /\ forked' = TRUE
         /\ pcP' = "Done"
         /\ UNCHANGED <<paused, inTick, pcC>>

Next == CIdle \/ BCheck \/ BSet \/ FSet \/ FCheck \/ Body \/ Clear
        \/ PSet \/ PWait \/ PFork

Spec == Init /\ [][Next]_vars

\* --- Safety ------------------------------------------------------------
\* fork() must never fire while the coordinator is inside the tick body
\* (locks held).  Once forked, the coordinator must also never ENTER the
\* body again while paused — covered by the same invariant since paused
\* stays TRUE after PSet.
NoForkDuringTick == ~(forked /\ pcC = "Body")

=============================================================================
