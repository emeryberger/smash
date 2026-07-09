/-
  Lean 4 formalization of SmashCore safety as a CONCURRENT transition system
  (companion to model/SmashCore.tla and the single-page model/SmashCore.lean).

  The single-page SmashCore.lean proves each transition preserves a
  well-formedness invariant, but abstracts the concurrency: one page, atomic
  whole-operation steps.  This file removes that abstraction.  It models the
  full system as it appears in the PlusCal spec:

    • MANY pages: the state is `pages : ι → Page` for an ARBITRARY index type
      ι.  Nothing bounds the number of pages (TLC must fix NumPages = 2 or 3).

    • MANY threads, ARBITRARY INTERLEAVING: `Step` is a nondeterministic
      relation.  One step is "some thread fires some enabled action on some
      page."  There is no thread-local program counter to schedule and no
      bound on the number of app / compressor / decommit threads, so the
      reachable set is closed under every interleaving of every thread — the
      thing bounded model checking can only sample.  Multi-step operations
      (the compressor's lock → COMPRESSING → PROT_READ → reclaim sequence) are
      modeled as SEPARATE steps, so a page is observable in its intermediate
      states and an interleaving may occur between them.

    • The freelist / decommit queues are represented DISTRIBUTIVELY as per-page
      flags (`onFreeList`, `onDecommitQ`), which is equivalent to the global
      sets in the PlusCal model for this property and keeps the invariant
      per-page.

  Result (`reachable_safe`): in the FIXED system, SafetyInv holds in EVERY
  reachable state, for ALL pages, under ALL interleavings, for any number of
  pages and threads.  `buggy_reachable_violates` shows the pre-fix decommit
  path reaches a state that violates SafetyInv.

  KEY MODELING NOTE.  SafetyInv here is the state/protection/blob COHERENCE
  property.  It turns out to be interleaving-robust WITHOUT modeling the
  per-page lock: every guarded transition maps a coherent page to a coherent
  page regardless of what other threads did, so no lock is needed to preserve
  it.  The lock's role is the orthogonal DATA-INTEGRITY property (a reader must
  not observe stale backing), which is proved separately as NoStaleRead in
  SmashRestoreRace.lean.  We make that division of labor explicit rather than
  re-deriving the lock discipline here.
-/

namespace SmashCoreConc

-- ── Per-page state (smash PageState × kernel mprotect bits × ownership) ──────

inductive PState where
  | EMPTY | ACTIVE | ACTIVE_MONITORING | COMPRESSING | COMPRESSED | COMPRESSED_SHADOW
  deriving DecidableEq, Repr

inductive Prot where
  | PROT_RW | PROT_READ | PROT_NONE
  deriving DecidableEq, Repr

open PState Prot

structure Page where
  state       : PState
  pageProt    : Prot
  hasBlob     : Bool
  hasPhysical : Bool
  onDecommitQ : Bool
  onFreeList  : Bool
  deriving DecidableEq, Repr

-- ── Safety property (verbatim from the TLA+ / single-page model) ─────────────

def BlobIntegrity (p : Page) : Prop :=
  (p.state = COMPRESSED        → p.hasBlob = true  ∧ p.hasPhysical = false) ∧
  (p.state = ACTIVE            → p.hasBlob = false ∧ p.hasPhysical = true)  ∧
  (p.state = COMPRESSING       → p.hasBlob = false ∧ p.hasPhysical = true)  ∧
  (p.state = COMPRESSED_SHADOW → p.hasBlob = true  ∧ p.hasPhysical = true)  ∧
  (p.state = EMPTY             → p.hasBlob = false ∧ p.hasPhysical = false)

def ProtectionSafety (p : Page) : Prop :=
  (p.state = COMPRESSED        → p.pageProt = PROT_NONE) ∧
  (p.state = ACTIVE            → p.pageProt = PROT_RW ∨ p.pageProt = PROT_READ) ∧
  (p.state = COMPRESSED_SHADOW → p.pageProt = PROT_RW ∨ p.pageProt = PROT_READ)

def ActiveImpliesRW (p : Page) : Prop :=
  p.state = ACTIVE → p.pageProt = PROT_RW

def SafetyInv (p : Page) : Prop :=
  BlobIntegrity p ∧ ProtectionSafety p ∧ ActiveImpliesRW p

-- ── Inductive invariant (per page).  Strengthens SafetyInv so it is closed
--    under every transition.  The load-bearing concurrency clause is
--    `onFreeList → PROT_RW`: it is what lets AllocPop (EMPTY→ACTIVE, which does
--    NOT touch protection) land on a PROT_RW page, and it is exactly the clause
--    the pre-fix decommit path fails to establish. ──────────────────────────

def WF (p : Page) : Prop :=
  (p.onFreeList = true → p.state = EMPTY ∧ p.pageProt = PROT_RW ∧ p.onDecommitQ = false) ∧
  (p.onDecommitQ = true → p.state = EMPTY) ∧
  (p.state = EMPTY  → p.hasBlob = false ∧ p.hasPhysical = false) ∧
  (p.state = ACTIVE → p.hasBlob = false ∧ p.hasPhysical = true ∧ p.pageProt = PROT_RW) ∧
  (p.state = ACTIVE_MONITORING → p.hasBlob = false ∧ p.hasPhysical = true) ∧
  (p.state = COMPRESSING → p.hasBlob = false ∧ p.hasPhysical = true) ∧
  (p.state = COMPRESSED →
      p.hasBlob = true ∧ p.hasPhysical = false ∧ p.pageProt = PROT_NONE) ∧
  (p.state = COMPRESSED_SHADOW →
      p.hasBlob = true ∧ p.hasPhysical = true ∧
      (p.pageProt = PROT_RW ∨ p.pageProt = PROT_READ))

theorem wf_implies_safety (p : Page) (h : WF p) : SafetyInv p := by
  obtain ⟨_, _, hE, hA, _, hCing, hC, hSh⟩ := h
  refine ⟨⟨?_, ?_, ?_, ?_, ?_⟩, ⟨?_, ?_, ?_⟩, ?_⟩ <;> intro hs
  · exact ⟨(hC hs).1, (hC hs).2.1⟩
  · exact ⟨(hA hs).1, (hA hs).2.1⟩
  · exact ⟨(hCing hs).1, (hCing hs).2⟩
  · exact ⟨(hSh hs).1, (hSh hs).2.1⟩
  · exact hE hs
  · exact (hC hs).2.2
  · exact Or.inl (hA hs).2.2
  · exact (hSh hs).2.2
  · exact (hA hs).2.2

-- ── Single-page transitions (guarded; each is one atomic sub-step of a thread
--    action).  A transition returns the new page; the `Step` relation below
--    applies it at one index and leaves every other page unchanged. ──────────

-- App fault handler: restore a protected page on access.  Mirrors FaultHandle.
def faultRestore (p : Page) : Page :=
  match p.state with
  | COMPRESSED        => { p with state := ACTIVE, pageProt := PROT_RW, hasBlob := false, hasPhysical := true }
  | COMPRESSED_SHADOW => { p with state := ACTIVE, pageProt := PROT_RW, hasBlob := false }
  | ACTIVE_MONITORING => { p with state := ACTIVE, pageProt := PROT_RW }
  | ACTIVE            => { p with pageProt := PROT_RW }  -- Phase-3 mprotect race window
  | _                 => p

-- App free(): any allocated page → EMPTY + enqueue for decommit.  Fires even on
-- COMPRESSED/MONITORING pages (protection intact), which is what makes the
-- buggy freelist-PROT_NONE state reachable.
def appFree (p : Page) : Page :=
  if p.state ≠ EMPTY ∧ p.onFreeList = false ∧ p.onDecommitQ = false then
    { p with state := EMPTY, hasBlob := false, hasPhysical := false, onDecommitQ := true }
  else p

-- Decommit thread: commitPages then push to freelist, as ONE thread action
-- (dequeue decommit, restore protection, enqueue free).  FIXED restores PROT_RW
-- before publishing to the freelist; BUGGY skips the restore — that is the bug.
-- Modeled atomically because no other transition is enabled on an onDecommitQ
-- page (it is EMPTY, off the freelist), so the commit→push order is not
-- observable to any concurrent thread and needs no separate interleaving point.
def decProcess (buggy : Bool) (p : Page) : Page :=
  if p.onDecommitQ = true then
    let q := { p with onDecommitQ := false, onFreeList := true }
    if buggy then q else { q with pageProt := PROT_RW }
  else p

-- Allocator, pop freelist: EMPTY → ACTIVE.  Protection is NOT touched here (the
-- bug surface): the page must already be PROT_RW, which WF guarantees.
def allocPop (p : Page) : Page :=
  if p.onFreeList = true ∧ p.state = EMPTY then
    { p with state := ACTIVE, hasPhysical := true, onFreeList := false }
  else p

-- Compressor Phase 2, lock+begin: ACTIVE/MONITORING → COMPRESSING.
def p2Begin (p : Page) : Page :=
  if p.state = ACTIVE ∨ p.state = ACTIVE_MONITORING then { p with state := COMPRESSING } else p

-- Compressor Phase 2, read under PROT_READ (intermediate; state stays COMPRESSING).
def p2Read (p : Page) : Page :=
  if p.state = COMPRESSING then { p with pageProt := PROT_READ } else p

-- Compressor Phase 2, finish (immediate reclaim): COMPRESSING → COMPRESSED.
def p2Reclaim (p : Page) : Page :=
  if p.state = COMPRESSING then
    { p with state := COMPRESSED, pageProt := PROT_NONE, hasBlob := true, hasPhysical := false }
  else p

-- Compressor Phase 2, finish (deferred): COMPRESSING → COMPRESSED_SHADOW.
def p2Defer (p : Page) : Page :=
  if p.state = COMPRESSING then
    { p with state := COMPRESSED_SHADOW, pageProt := PROT_RW, hasBlob := true }
  else p

-- Compressor Phase 2, finish (ratio-gate failure): COMPRESSING → ACTIVE.
-- No blob is stored (hasBlob stays false); PROT_RW is restored BEFORE the
-- state flips to ACTIVE in the code — modeled atomically here because the
-- per-page lock is held across both, so no thread observes the intermediate.
def p2Fail (p : Page) : Page :=
  if p.state = COMPRESSING then
    { p with state := ACTIVE, pageProt := PROT_RW }
  else p

-- Compressor Phase 3 CAS: ACTIVE → ACTIVE_MONITORING (protection NOT yet changed;
-- the intermediate MONITORING+PROT_RW state is observable to other threads).
def p3CAS (p : Page) : Page :=
  if p.state = ACTIVE then { p with state := ACTIVE_MONITORING } else p

-- Compressor Phase 3 protect: MONITORING → PROT_READ.
def p3Prot (p : Page) : Page :=
  if p.state = ACTIVE_MONITORING then { p with pageProt := PROT_READ } else p

-- Compressor Phase B reclaim: SHADOW → COMPRESSED (deferred reclaim commit).
def pbReclaim (p : Page) : Page :=
  if p.state = COMPRESSED_SHADOW then
    { p with state := COMPRESSED, pageProt := PROT_NONE, hasPhysical := false }
  else p

-- Compressor Phase B verify-and-restore: SHADOW → ACTIVE (page was re-accessed).
def pbRestore (p : Page) : Page :=
  if p.state = COMPRESSED_SHADOW then
    { p with state := ACTIVE, pageProt := PROT_RW, hasBlob := false }
  else p

-- The set of one-page transitions available to threads, parameterized by mode.
inductive Trans (buggy : Bool) : Page → Page → Prop where
  | fault    (p : Page) : Trans buggy p (faultRestore p)
  | free     (p : Page) : Trans buggy p (appFree p)
  | decProc  (p : Page) : Trans buggy p (decProcess buggy p)
  | allocPop (p : Page) : Trans buggy p (allocPop p)
  | p2Begin  (p : Page) : Trans buggy p (p2Begin p)
  | p2Read   (p : Page) : Trans buggy p (p2Read p)
  | p2Reclaim(p : Page) : Trans buggy p (p2Reclaim p)
  | p2Defer  (p : Page) : Trans buggy p (p2Defer p)
  | p2Fail   (p : Page) : Trans buggy p (p2Fail p)
  | p3CAS    (p : Page) : Trans buggy p (p3CAS p)
  | p3Prot   (p : Page) : Trans buggy p (p3Prot p)
  | pbReclaim(p : Page) : Trans buggy p (pbReclaim p)
  | pbRestore(p : Page) : Trans buggy p (pbRestore p)

-- ── Concurrent system over an arbitrary page-index type ι ────────────────────

variable {ι : Type} [DecidableEq ι]

/-- A system state maps each page index to its page. -/
abbrev Sys (ι : Type) := ι → Page

/-- Pointwise function update (core Lean has no `Function.update`). -/
def upd [DecidableEq ι] (s : Sys ι) (i : ι) (q : Page) : Sys ι :=
  fun j => if j = i then q else s j

@[simp] theorem upd_same [DecidableEq ι] (s : Sys ι) (i : ι) (q : Page) : upd s i q i = q := by
  unfold upd; simp

theorem upd_noteq [DecidableEq ι] {s : Sys ι} {i j : ι} (q : Page) (h : j ≠ i) :
    upd s i q j = s j := by
  unfold upd; simp [h]

/-- The initial state: every page ACTIVE + PROT_RW (freshly committed), no
    queues.  (Matches the TLA+ Init.) -/
def initPage : Page :=
  { state := ACTIVE, pageProt := PROT_RW, hasBlob := false, hasPhysical := true,
    onDecommitQ := false, onFreeList := false }

/-- One concurrent step: some thread fires some enabled transition on some page
    `i`; all other pages are unchanged.  Quantifying over `i` and over the
    `Trans` constructors captures every interleaving, for any number of pages
    and threads. -/
inductive Step (buggy : Bool) : Sys ι → Sys ι → Prop where
  | fire (s : Sys ι) (i : ι) (q : Page) (h : Trans buggy (s i) q) :
      Step buggy s (upd s i q)

/-- Reachability: closure of `{init}` under `Step`. -/
inductive Reachable (buggy : Bool) : Sys ι → Prop where
  | init : Reachable buggy (fun _ => initPage)
  | step {s t : Sys ι} : Reachable buggy s → Step buggy s t → Reachable buggy t

-- ── Per-page preservation: every transition maps WF to WF ────────────────────
-- Each proof is a finite case split over the page's state (+ flags), discharged
-- by `decide`-style reasoning after unfolding.  `simp_all`/`decide` close them
-- because Page fields are finite enums / Bools.

set_option maxHeartbeats 1000000

theorem faultRestore_wf (p : Page) (h : WF p) : WF (faultRestore p) := by
  unfold WF faultRestore at *; cases hs : p.state <;> simp_all

theorem appFree_wf (p : Page) (h : WF p) : WF (appFree p) := by
  unfold WF appFree at *; cases hs : p.state <;> simp_all

theorem decProcess_fixed_wf (p : Page) (h : WF p) : WF (decProcess false p) := by
  unfold WF decProcess at *; cases hs : p.state <;>
    by_cases hq : p.onDecommitQ = true <;> simp_all

theorem allocPop_wf (p : Page) (h : WF p) : WF (allocPop p) := by
  unfold WF allocPop at *; cases hs : p.state <;>
    by_cases hf : p.onFreeList = true <;> simp_all

theorem p2Begin_wf (p : Page) (h : WF p) : WF (p2Begin p) := by
  unfold WF p2Begin at *; cases hs : p.state <;> simp_all

theorem p2Read_wf (p : Page) (h : WF p) : WF (p2Read p) := by
  unfold WF p2Read at *; cases hs : p.state <;> simp_all

theorem p2Reclaim_wf (p : Page) (h : WF p) : WF (p2Reclaim p) := by
  unfold WF p2Reclaim at *; cases hs : p.state <;> simp_all

theorem p2Defer_wf (p : Page) (h : WF p) : WF (p2Defer p) := by
  unfold WF p2Defer at *; cases hs : p.state <;> simp_all

theorem p2Fail_wf (p : Page) (h : WF p) : WF (p2Fail p) := by
  unfold WF p2Fail at *; cases hs : p.state <;> simp_all

theorem p3CAS_wf (p : Page) (h : WF p) : WF (p3CAS p) := by
  unfold WF p3CAS at *; cases hs : p.state <;> simp_all

theorem p3Prot_wf (p : Page) (h : WF p) : WF (p3Prot p) := by
  unfold WF p3Prot at *; cases hs : p.state <;> simp_all

theorem pbReclaim_wf (p : Page) (h : WF p) : WF (pbReclaim p) := by
  unfold WF pbReclaim at *; cases hs : p.state <;> simp_all

theorem pbRestore_wf (p : Page) (h : WF p) : WF (pbRestore p) := by
  unfold WF pbRestore at *; cases hs : p.state <;> simp_all

/-- Every FIXED-mode transition preserves WF (dispatch over the constructors).
    The decommit path only preserves the freelist invariant
    (`onFreeList → PROT_RW`) when it restores protection — i.e.\ in fixed mode;
    that is exactly the bug, exhibited concretely by `buggy_reachable_violates`. -/
theorem trans_wf {p q : Page} (h : Trans false p q) (hp : WF p) : WF q := by
  cases h with
  | fault     => exact faultRestore_wf _ hp
  | free      => exact appFree_wf _ hp
  | decProc   => exact decProcess_fixed_wf _ hp
  | allocPop  => exact allocPop_wf _ hp
  | p2Begin   => exact p2Begin_wf _ hp
  | p2Read    => exact p2Read_wf _ hp
  | p2Reclaim => exact p2Reclaim_wf _ hp
  | p2Defer   => exact p2Defer_wf _ hp
  | p2Fail    => exact p2Fail_wf _ hp
  | p3CAS     => exact p3CAS_wf _ hp
  | p3Prot    => exact p3Prot_wf _ hp
  | pbReclaim => exact pbReclaim_wf _ hp
  | pbRestore => exact pbRestore_wf _ hp

-- ── Global invariant: every page is WF ───────────────────────────────────────

def Inv (s : Sys ι) : Prop := ∀ i, WF (s i)

theorem init_inv : Inv (ι := ι) (fun _ => initPage) := by
  intro i; unfold WF initPage; refine ⟨?_,?_,?_,?_,?_,?_,?_,?_⟩ <;> intro h <;> simp_all

/-- The global invariant is preserved by every concurrent step: the fired page
    is preserved by `trans_wf`; all other pages are unchanged. -/
theorem step_inv {s t : Sys ι} (hs : Inv s) (hstep : Step false s t) : Inv t := by
  cases hstep with
  | fire i q h =>
    intro j
    by_cases hj : j = i
    · subst hj; rw [upd_same]; exact trans_wf h (hs j)
    · rw [upd_noteq q hj]; exact hs j

/-- MAIN THEOREM.  In the fixed system, SafetyInv holds for every page in every
    reachable state, under all interleavings, for any number of pages and
    threads. -/
theorem reachable_safe {s : Sys ι} (h : Reachable false s) : ∀ i, SafetyInv (s i) := by
  have hinv : Inv s := by
    induction h with
    | init => exact init_inv
    | step _ hstep ih => exact step_inv ih hstep
  intro i; exact wf_implies_safety _ (hinv i)

-- ── The bug is real: the pre-fix decommit path reaches a SafetyInv violation ──
-- Trace (single page, buggy=true): ACTIVE →(p2Begin) COMPRESSING →(p2Reclaim)
-- COMPRESSED(PROT_NONE) →(free) EMPTY+onDecommitQ →(decProcess BUGGY: pushes to
-- freelist but leaves prot PROT_NONE) →(allocPop) ACTIVE+PROT_NONE.
-- That final page is ACTIVE with PROT_NONE, violating ActiveImpliesRW.

def buggyBad : Page :=
  { state := ACTIVE, pageProt := PROT_NONE, hasBlob := false, hasPhysical := true,
    onDecommitQ := false, onFreeList := false }

theorem buggyBad_violates : ¬ SafetyInv buggyBad := by
  intro h; have := h.2.2; simp [buggyBad, ActiveImpliesRW] at this

/-- The bad page is reachable in buggy mode from a single-page initial state,
    via the trace above.  We exhibit it by composing the transitions and
    checking the result equals `buggyBad`. -/
theorem buggy_reachable_violates :
    ∃ p : Page,
      p = allocPop (decProcess true (appFree (p2Reclaim (p2Begin initPage))))
      ∧ ¬ SafetyInv p := by
  refine ⟨_, rfl, ?_⟩
  have : allocPop (decProcess true (appFree (p2Reclaim (p2Begin initPage)))) = buggyBad := by
    decide
  rw [this]; exact buggyBad_violates

end SmashCoreConc
