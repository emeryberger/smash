/-
  Lean 4 formalization of the SmashFreePoolRace safety property
  (companion model for the full-mode LargeAlloc spin-livelock).

  THE BUG (root-caused 2026-06-25, full-mode rocksdb intermittent hang):
  In src/vm/vm_region.h each FreeShard has ONE spinlock guarding BOTH free_list
  and free_pool (the FreeRun recycle stack). recycleFreeRun PUSHES free_pool
  under shard.lock (from allocatePages). But processDecommitEntry (the DECOMMIT
  THREAD) calls newFreeRun — which POPS free_pool (`free_pool = r->next`) —
  BEFORE acquiring shard.lock. So free_pool, a singly-linked Treiber stack, is
  mutated CONCURRENTLY by an unlocked pop (decommit thread) and a locked push
  (app/compressor thread). The classic non-atomic-stack race: a push's
  read-of-head and a pop's write-of-head interleave, producing a lost update or
  a node whose ->next links back into the live list → a CYCLE. A later walk
  (newFreeRun's pop, or the free_list traversal in allocatePages) then spins
  forever on the cycle → decommit thread spins, allocate stalls, rocksdb
  write-stalls, the process hangs.

  We model the recycle stack abstractly. The two mutators are:
    - Popper  : the decommit thread's newFreeRun pop.
    - Pusher  : the app thread's recycleFreeRun push (read head; set node.next =
                head; set head = node) — modeled as a non-atomic RMW with an
                exposed mid-point so a concurrent pop can interleave.
  parameterized by Variant:
    - unlocked : Popper takes NO lock (the bug). The pop can fire between the
                 Pusher's read-of-head and write-of-head → lost update / corrupt
                 stack. Safety (SerializedPool) is VIOLATED.
    - locked   : Popper takes the shard lock (the fix). Pop and push are
                 mutually exclusive, so the stack is never concurrently mutated.
                 SerializedPool HOLDS.

  Safety invariant (SerializedPool): the free_pool stack is never mutated by one
  actor while the other holds a half-finished (non-atomic) mutation — i.e. the
  push's read→write window and the pop never overlap. This is the precondition
  whose violation lets the stack become cyclic/lost; preserving it keeps the
  stack well-formed so every walk terminates.

  Deadlock note: the fix only EXTENDS an existing lock's scope (moves the pop
  inside shard.lock, where the push already is). It adds no new lock and no new
  ordering: newFreeRun's BootstrapAlloc fallback uses its own lock, taken AFTER
  shard.lock here and nowhere-else-before it, so no inversion. Modeled by
  `locked_mutual_exclusion`.
-/

-- ── Abstractions ─────────────────────────────────────────────────────────────

inductive Lock where
  | free | popper | pusher
  deriving DecidableEq, Repr

inductive Variant where
  | unlocked | locked
  deriving DecidableEq, Repr

/-- Pusher (recycleFreeRun) program counter. The push is a non-atomic 3-step:
    read head → set node.next := head → set head := node. The middle exposes the
    window a concurrent unlocked pop can corrupt. -/
inductive PushPC where
  | PIdle | PMid | PDone
  deriving DecidableEq, Repr

/-- Popper (newFreeRun) program counter. -/
inductive PopPC where
  | OIdle | ODone
  deriving DecidableEq, Repr

structure State where
  lock     : Lock
  ppc      : PushPC      -- pusher location
  opc      : PopPC       -- popper location
  -- ghost: a mutation happened on the pool while the OTHER actor was mid-RMW.
  corrupt  : Bool
  deriving Repr

def init : State :=
  { lock := Lock.free, ppc := PushPC.PIdle, opc := PopPC.OIdle, corrupt := false }

-- ── Pusher (recycleFreeRun): always under shard.lock in BOTH variants ────────

/-- Push step 1: acquire shard.lock and read head (enter the RMW window). -/
def stepPushBegin (s : State) : State :=
  if s.ppc = PushPC.PIdle ∧ s.lock = Lock.free then
    { s with lock := Lock.pusher, ppc := PushPC.PMid }
  else s

/-- Push step 2: complete the RMW (set node.next := head; head := node) and
    release the lock. If a pop mutated the pool during PMid, corruption already
    recorded. -/
def stepPushEnd (s : State) : State :=
  if s.ppc = PushPC.PMid then
    { s with lock := Lock.free, ppc := PushPC.PDone }
  else s

-- ── Popper (newFreeRun): lock discipline depends on the variant ──────────────

/-- Pop (decommit thread): mutate the pool head (`head := head.next`).
    unlocked: fires whenever the popper is idle, IGNORING the lock (the bug).
    locked:   only when the lock is free; acquires it (the fix).
    If the pop mutates while the pusher is mid-RMW (ppc = PMid), it corrupts the
    stack — that's the data race that creates the cycle. -/
def stepPop (v : Variant) (s : State) : State :=
  if s.opc = PopPC.OIdle then
    match v with
    | Variant.unlocked =>
        -- No lock: pop runs regardless. Corrupts iff the pusher is mid-RMW.
        { s with opc := PopPC.ODone,
                 corrupt := s.corrupt || (decide (s.ppc = PushPC.PMid)) }
    | Variant.locked =>
        if s.lock = Lock.free then
          -- Holds the lock across the pop; pusher cannot be mid-RMW (it would
          -- hold the lock), so no corruption.
          { s with lock := Lock.popper, opc := PopPC.ODone,
                   corrupt := s.corrupt || (decide (s.ppc = PushPC.PMid)) }
        else s
  else s

/-- Pop release: the popper finishes and returns to idle; the locked variant
    also drops the lock. -/
def stepPopRelease (v : Variant) (s : State) : State :=
  if s.opc = PopPC.ODone then
    match v with
    | Variant.unlocked => { s with opc := PopPC.OIdle }
    | Variant.locked   =>
        if s.lock = Lock.popper then { s with lock := Lock.free, opc := PopPC.OIdle }
        else { s with opc := PopPC.OIdle }
  else s

-- ── Safety invariant ─────────────────────────────────────────────────────────

/-- SerializedPool: the pool was never mutated mid-RMW by the other actor. -/
def NoCorrupt (s : State) : Prop := s.corrupt = false

-- ═══════════════════════════════════════════════════════════════════
-- LOCKED variant: NoCorrupt holds via an inductive invariant.
-- The crux: the single shard lock makes the pusher's RMW window (PMid) and the
-- popper's mutation mutually exclusive, so the pop never fires during PMid.
-- ═══════════════════════════════════════════════════════════════════

/-- I1 no corruption; I2 pusher mid-RMW ⇒ it holds the lock; I3 popper mid-pop ⇒
    it holds the lock. Together (single lock) ⇒ never both mid-section. -/
def LockedInv (s : State) : Prop :=
  s.corrupt = false
  ∧ (s.ppc = PushPC.PMid → s.lock = Lock.pusher)
  ∧ (s.opc = PopPC.ODone → s.lock = Lock.popper)

theorem locked_init_inv : LockedInv init := by
  refine ⟨rfl, ?_, ?_⟩ <;> intro h <;> simp [init] at h

theorem locked_pushbegin_preserves (s : State) (h : LockedInv s) :
    LockedInv (stepPushBegin s) := by
  obtain ⟨h1, h2, h3⟩ := h
  unfold stepPushBegin
  by_cases hc : s.ppc = PushPC.PIdle ∧ s.lock = Lock.free
  · -- popper not mid-pop: opc=ODone would need lock=popper, but lock=free
    have hop : s.opc ≠ PopPC.ODone := by
      intro ho; have := h3 ho; rw [hc.2] at this; exact absurd this (by decide)
    simp only [if_pos hc]
    refine ⟨h1, ?_, ?_⟩
    · intro _; rfl                       -- post lock = pusher
    · intro ho; exact absurd ho hop      -- post opc unchanged ≠ ODone
  · simp only [if_neg hc]; exact ⟨h1, h2, h3⟩

theorem locked_pushend_preserves (s : State) (h : LockedInv s) :
    LockedInv (stepPushEnd s) := by
  obtain ⟨h1, h2, h3⟩ := h
  unfold stepPushEnd
  by_cases hc : s.ppc = PushPC.PMid
  · -- pusher held the lock (h2); releasing it. popper not mid (it would need the
    -- lock, which the pusher holds).
    have hlk : s.lock = Lock.pusher := h2 hc
    have hop : s.opc ≠ PopPC.ODone := by
      intro ho; have := h3 ho; rw [hlk] at this; exact absurd this (by decide)
    simp only [if_pos hc]
    refine ⟨h1, ?_, ?_⟩
    · intro h; simp at h                    -- post ppc = PDone ≠ PMid
    · intro ho; exact absurd ho hop         -- post opc unchanged ≠ ODone
  · simp only [if_neg hc]; exact ⟨h1, h2, h3⟩

theorem locked_pop_preserves (s : State) (h : LockedInv s) :
    LockedInv (stepPop Variant.locked s) := by
  obtain ⟨h1, h2, h3⟩ := h
  unfold stepPop
  by_cases hc : s.opc = PopPC.OIdle
  · by_cases hfree : s.lock = Lock.free
    · -- pusher not mid-RMW (it would hold the lock, but lock is free) ⇒ no corrupt
      have hpp : s.ppc ≠ PushPC.PMid := by
        intro hp; have := h2 hp; rw [hfree] at this; exact absurd this (by decide)
      simp only [if_pos hc, if_pos hfree]
      refine ⟨?_, ?_, ?_⟩
      · -- corrupt' = corrupt || decide(ppc=PMid) = false || false
        simp [h1]; intro hp; exact absurd hp hpp
      · intro hp; exact absurd hp hpp        -- ppc unchanged ≠ PMid (so lock claim vacuous)
      · intro _; rfl                         -- post lock = popper
    · simp only [if_pos hc, if_neg hfree]; exact ⟨h1, h2, h3⟩
  · simp only [if_neg hc]; exact ⟨h1, h2, h3⟩

theorem locked_poprelease_preserves (s : State) (h : LockedInv s) :
    LockedInv (stepPopRelease Variant.locked s) := by
  obtain ⟨h1, h2, h3⟩ := h
  unfold stepPopRelease
  by_cases hc : s.opc = PopPC.ODone
  · by_cases hlk : s.lock = Lock.popper
    · -- releasing the lock; pusher not mid (it would need the lock = popper now)
      have hpp : s.ppc ≠ PushPC.PMid := by
        intro hp; have := h2 hp; rw [hlk] at this; exact absurd this (by decide)
      simp only [if_pos hc, if_pos hlk]
      refine ⟨h1, ?_, ?_⟩
      · intro hp; exact absurd hp hpp        -- ppc unchanged ≠ PMid
      · intro ho; simp at ho                 -- post opc = OIdle ≠ ODone (vacuous)
    · simp only [if_pos hc, if_neg hlk]
      refine ⟨h1, h2, ?_⟩
      · intro ho; simp at ho                 -- post opc = OIdle ≠ ODone (vacuous)
  · simp only [if_neg hc]; exact ⟨h1, h2, h3⟩

/-- LockedInv ⇒ NoCorrupt; holds initially and is preserved by every transition
    of the locked variant ⇒ holds in all reachable states. -/
theorem locked_inv_implies_safe (s : State) (h : LockedInv s) : NoCorrupt s := h.1

/-- Mutual exclusion: under LockedInv the pusher's RMW window and the popper's
    mutation never overlap (single shard lock). This is what keeps free_pool
    well-formed (acyclic), so allocatePages/newFreeRun walks terminate. -/
theorem locked_mutual_exclusion (s : State) (h : LockedInv s) :
    ¬ (s.ppc = PushPC.PMid ∧ s.opc = PopPC.ODone) := by
  obtain ⟨_, h2, h3⟩ := h
  rintro ⟨hp, ho⟩
  have l1 := h2 hp; have l2 := h3 ho
  rw [l1] at l2; exact absurd l2 (by decide)

-- ═══════════════════════════════════════════════════════════════════
-- UNLOCKED variant: NoCorrupt is VIOLATED — the concrete data-race trace.
-- ═══════════════════════════════════════════════════════════════════

/-- The exact corrupting interleaving:
      Pusher begins its RMW (reads head, now PMid, holding the lock),
      Popper (NO lock!) mutates the pool head while the pusher is mid-RMW
      → corrupt (lost update / cycle precondition). -/
def unlockedCorruptState : State :=
  stepPop Variant.unlocked (stepPushBegin init)

theorem unlocked_violates_safe : ¬ NoCorrupt unlockedCorruptState := by
  unfold NoCorrupt unlockedCorruptState stepPop stepPushBegin init
  decide

/-- Under the fix, the same schedule cannot corrupt: with the pusher mid-RMW it
    holds the lock, so the locked pop is disabled (no-op) — no concurrent
    mutation. -/
theorem locked_same_schedule_safe :
    NoCorrupt (stepPop Variant.locked (stepPushBegin init)) := by
  unfold NoCorrupt stepPop stepPushBegin init
  decide
