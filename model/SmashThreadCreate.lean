/-
  Lean 4 formalization of the SmashThreadCreate safety property.

  Theorem: When workers are pre-created (preCreateWorkers = true),
  the invariant ThreadCreateSafe holds in all reachable states.

  The invariant states: at the moment pthread_create fires
  (justCreated = true), the number of TLS pages with PROT_NONE
  must not exceed MaxFaultable.

  The fix makes this vacuously true: with preCreateWorkers = true,
  workersCreated starts as true, so the MaybeCreateWorker action
  never sets justCreated = true.
-/

-- Page protection states
inductive PageProt where
  | RW | READ | NONE
  deriving DecidableEq, Repr

-- System state (abstract — we only track what matters for the invariant)
structure State where
  workersCreated : Bool
  justCreated    : Bool
  somePageProtNone : Bool  -- ∃ p ∈ TlsPages, pageProt p = PROT_NONE

-- The safety invariant: justCreated → ¬ (too many PROT_NONE TLS pages).
-- Simplified: since any PROT_NONE TLS page at pthread_create time is unsafe
-- (MaxFaultable can be as low as 0 in the buggy case), we prove the stronger:
-- justCreated is never true when preCreateWorkers = true.
def threadCreateSafe (s : State) : Prop :=
  s.justCreated = true → False

-- Initial state with the fix applied
def initFixed : State :=
  { workersCreated := true
    justCreated := false
    somePageProtNone := false }

-- MaybeCreateWorker transition
def stepMaybeCreateWorker (s : State) : State :=
  if !s.workersCreated && s.somePageProtNone then
    { s with workersCreated := true, justCreated := true }
  else
    { s with justCreated := false }

-- AdvanceTick transition (always clears justCreated)
def stepAdvanceTick (s : State) : State :=
  { s with justCreated := false }

-- DeferredSweep transition (may set somePageProtNone, doesn't touch workers)
def stepDeferredSweep (s : State) (pagesCompressed : Bool) : State :=
  { s with somePageProtNone := pagesCompressed }

-- ═══════════════════════════════════════════════════════════════════
-- THEOREMS
-- ═══════════════════════════════════════════════════════════════════

-- Lemma: When workersCreated = true, MaybeCreateWorker sets justCreated = false.
theorem mcw_preserves_safe (s : State) (h : s.workersCreated = true) :
    (stepMaybeCreateWorker s).justCreated = false := by
  simp [stepMaybeCreateWorker, h]

-- Lemma: MaybeCreateWorker preserves workersCreated = true.
theorem mcw_preserves_wc (s : State) (h : s.workersCreated = true) :
    (stepMaybeCreateWorker s).workersCreated = true := by
  simp [stepMaybeCreateWorker, h]

-- Lemma: AdvanceTick preserves workersCreated.
theorem tick_preserves_wc (s : State) :
    (stepAdvanceTick s).workersCreated = s.workersCreated := by
  simp [stepAdvanceTick]

-- Lemma: DeferredSweep preserves workersCreated.
theorem sweep_preserves_wc (s : State) (b : Bool) :
    (stepDeferredSweep s b).workersCreated = s.workersCreated := by
  simp [stepDeferredSweep]

-- Main theorem: With preCreateWorkers = true (workersCreated starts true),
-- threadCreateSafe holds after every MaybeCreateWorker step.
theorem safety_after_mcw (s : State) (h_wc : s.workersCreated = true) :
    threadCreateSafe (stepMaybeCreateWorker s) := by
  intro h_jc
  have := mcw_preserves_safe s h_wc
  rw [this] at h_jc
  exact absurd h_jc (by decide)

-- Inductive invariant: workersCreated is monotonically true.
-- Once workersCreated = true, no transition sets it to false.
-- (DeferredSweep, AdvanceTick don't touch it; MaybeCreateWorker only sets it true.)

-- Corollary: initFixed satisfies the invariant
theorem init_safe : threadCreateSafe initFixed := by
  intro h
  simp [initFixed] at h

-- The full inductive invariant: workersCreated = true ∧ justCreated = false.
-- This is preserved by all transitions.
def inductiveInv (s : State) : Prop :=
  s.workersCreated = true ∧ s.justCreated = false

-- initFixed satisfies the inductive invariant
theorem init_inv : inductiveInv initFixed := by
  constructor <;> simp [initFixed]

-- The inductive invariant implies safety
theorem inv_implies_safe (s : State) (h : inductiveInv s) :
    threadCreateSafe s := by
  intro hjc
  exact absurd hjc (by rw [h.2]; decide)

-- The inductive invariant is preserved by all transitions
theorem mcw_preserves_inv (s : State) (h : inductiveInv s) :
    inductiveInv (stepMaybeCreateWorker s) := by
  constructor
  · exact mcw_preserves_wc s h.1
  · exact mcw_preserves_safe s h.1

theorem tick_preserves_inv (s : State) (h : inductiveInv s) :
    inductiveInv (stepAdvanceTick s) := by
  constructor
  · rw [tick_preserves_wc]; exact h.1
  · simp [stepAdvanceTick]

theorem sweep_preserves_inv (s : State) (h : inductiveInv s) (b : Bool) :
    inductiveInv (stepDeferredSweep s b) := by
  constructor
  · rw [sweep_preserves_wc]; exact h.1
  · simp [stepDeferredSweep]; exact h.2
