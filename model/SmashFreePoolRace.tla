------------------------- MODULE SmashFreePoolRace -------------------------
(*
 * SmashFreePoolRace — models the full-mode LargeAlloc spin-livelock
 * (root-caused 2026-06-25). Companion to model/SmashFreePoolRace.lean
 * (machine-checked, zero axioms).
 *
 * In src/vm/vm_region.h each FreeShard has ONE spinlock guarding BOTH free_list
 * and free_pool (the FreeRun recycle stack). recycleFreeRun PUSHES free_pool
 * under shard.lock (from allocatePages). But processDecommitEntry (the DECOMMIT
 * THREAD) calls newFreeRun — which POPS free_pool — BEFORE acquiring shard.lock.
 * So free_pool, a singly-linked Treiber stack, is mutated CONCURRENTLY by an
 * unlocked pop (decommit thread) and a locked push. The push's non-atomic
 * read-head/write-head window can interleave the pop → lost update / a node
 * whose ->next links back into the live list → a CYCLE → a later walk
 * (newFreeRun pop or the free_list traversal in allocatePages) spins forever →
 * decommit thread spins, allocate stalls, rocksdb write-stalls, process hangs.
 *
 * Variant:
 *   "unlocked" : pop takes NO lock (the bug) — can fire during the push's RMW
 *                window → NoCorrupt VIOLATED.
 *   "locked"   : pop takes the shard lock (the fix) — mutually exclusive with
 *                the push's RMW → NoCorrupt HOLDS.
 *
 * Deadlock note: the fix only EXTENDS the existing shard lock's scope (moves the
 * pop inside it, where the push already runs). No new lock, no new ordering →
 * no deadlock. See MutualExclusion.
 *
 * Safety invariant (NoCorrupt): the pool is never mutated by one actor while the
 * other is mid (non-atomic) RMW.
 *
 * TLC (run with -deadlock; terminating model):
 *   _unlocked_buggy.cfg : Invariant NoCorrupt is VIOLATED.
 *   _locked_fixed.cfg   : No error — NoCorrupt AND MutualExclusion hold.
 *)

EXTENDS TLC

CONSTANTS Variant   \* "unlocked" | "locked"

VARIABLES
    lock,      \* "free" | "popper" | "pusher"
    ppc,       \* pusher PC: "PIdle" | "PMid" | "PDone"
    opc,       \* popper PC: "OIdle" | "ODone"
    corrupt    \* BOOLEAN ghost — pool mutated mid-RMW by the other actor

vars == << lock, ppc, opc, corrupt >>

Init ==
    /\ lock = "free"
    /\ ppc  = "PIdle"
    /\ opc  = "OIdle"
    /\ corrupt = FALSE

\* --- Pusher (recycleFreeRun): under shard.lock in BOTH variants -------------

\* Begin: acquire lock, read head (enter the non-atomic RMW window).
PushBegin ==
    /\ ppc = "PIdle"
    /\ lock = "free"
    /\ lock' = "pusher"
    /\ ppc' = "PMid"
    /\ UNCHANGED << opc, corrupt >>

\* End: finish the RMW (set node.next:=head; head:=node), release the lock.
PushEnd ==
    /\ ppc = "PMid"
    /\ lock' = "free"
    /\ ppc' = "PDone"
    /\ UNCHANGED << opc, corrupt >>

\* --- Popper (newFreeRun): lock discipline depends on the variant -----------

\* Pop the pool head. Corrupts iff it fires while the pusher is mid-RMW (PMid).
Pop ==
    /\ opc = "OIdle"
    /\ IF Variant = "locked"
         THEN /\ lock = "free"            \* fix: must hold the lock
              /\ lock' = "popper"
         ELSE /\ lock' = lock             \* bug: ignores the lock
    /\ opc' = "ODone"
    /\ corrupt' = (corrupt \/ (ppc = "PMid"))
    /\ UNCHANGED ppc

\* Pop release: popper returns to idle; locked variant drops the lock.
PopRelease ==
    /\ opc = "ODone"
    /\ opc' = "OIdle"
    /\ lock' = IF Variant = "locked" /\ lock = "popper" THEN "free" ELSE lock
    /\ UNCHANGED << ppc, corrupt >>

Next ==
    \/ PushBegin
    \/ PushEnd
    \/ Pop
    \/ PopRelease
    \* Terminal self-loop when both actors are done so TLC has no deadlock state
    \* (push done, popper idle, nothing else enabled).
    \/ (ppc = "PDone" /\ opc = "OIdle" /\ lock = "free" /\ UNCHANGED vars)

Spec == Init /\ [][Next]_vars

\* --- Safety -----------------------------------------------------------------

NoCorrupt == corrupt = FALSE

TypeOK ==
    /\ lock \in {"free", "popper", "pusher"}
    /\ ppc \in {"PIdle", "PMid", "PDone"}
    /\ opc \in {"OIdle", "ODone"}
    /\ corrupt \in BOOLEAN

\* The pusher's RMW window and the popper's mutation never overlap (single lock).
MutualExclusion == ~(ppc = "PMid" /\ opc = "ODone")

=============================================================================
