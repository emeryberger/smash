/-
  Lean 4 formalization of the SmashCore safety property
  (companion to model/SmashCore.tla).

  SmashCore models TWO per-page state machines and the desync bugs that live at
  their boundary:
    • state    : smash PageState — EMPTY, ACTIVE, ACTIVE_MONITORING,
                 COMPRESSING, COMPRESSED, COMPRESSED_SHADOW
    • pageProt : kernel mprotect bits — PROT_RW, PROT_READ, PROT_NONE

  SafetyInv = BlobIntegrity ∧ ProtectionSafety ∧ ActiveImpliesRW.

  The May-2026 freelist-pop bug: the compressor sets pageProt = PROT_NONE when
  it compresses a page; the page is then freed (state → EMPTY) and queued for
  decommit.  processDecommitEntry is supposed to call commitPages (restore
  PROT_RW) before pushing the run onto the freelist.  The buggy code skipped
  that.  So when the allocator later pops the run (state EMPTY → ACTIVE), the
  kernel bits are still PROT_NONE — and ActiveImpliesRW (state = ACTIVE ⇒
  pageProt = PROT_RW) is violated.  The application then faults on what it
  believes is a valid heap pointer → SIGSEGV.

  `BuggyMode = true` reproduces the skipped commitPages; `false` is the fix.

  Because SafetyInv alone is NOT inductive (allocPop EMPTY→ACTIVE keeps whatever
  pageProt the EMPTY page had — that's exactly the bug surface), we prove safety
  through a stronger inductive invariant `WF` that additionally pins down each
  state's (blob, physical, prot) and the queue discipline
  (onFreeList ⇒ EMPTY ∧ PROT_RW).  Then:
    • WF p → SafetyInv p                                    (wf_implies_safety)
    • WF init, and every FIXED-mode transition preserves WF (init_wf, *_pres)
    • BUGGY mode reaches a state with state = ACTIVE ∧ pageProt = PROT_NONE,
      so SafetyInv is reachably violated                     (buggy_violates_safety)
-/

-- ── Domains ──────────────────────────────────────────────────────────────────

inductive PState where
  | EMPTY | ACTIVE | ACTIVE_MONITORING | COMPRESSING | COMPRESSED | COMPRESSED_SHADOW
  deriving DecidableEq, Repr

inductive Prot where
  | PROT_RW | PROT_READ | PROT_NONE
  deriving DecidableEq, Repr

structure Page where
  state       : PState
  pageProt    : Prot
  hasBlob     : Bool
  hasPhysical : Bool
  onDecommitQ : Bool
  onFreeList  : Bool
  deriving DecidableEq, Repr

-- ── The three SafetyInv conjuncts (exactly as in the TLA+ model) ─────────────

def BlobIntegrity (p : Page) : Prop :=
  (p.state = PState.COMPRESSED        → p.hasBlob = true  ∧ p.hasPhysical = false) ∧
  (p.state = PState.ACTIVE            → p.hasBlob = false ∧ p.hasPhysical = true)  ∧
  (p.state = PState.COMPRESSING       → p.hasBlob = false ∧ p.hasPhysical = true)  ∧
  (p.state = PState.COMPRESSED_SHADOW → p.hasBlob = true  ∧ p.hasPhysical = true)  ∧
  (p.state = PState.EMPTY             → p.hasBlob = false ∧ p.hasPhysical = false)

def ProtectionSafety (p : Page) : Prop :=
  (p.state = PState.COMPRESSED        → p.pageProt = Prot.PROT_NONE) ∧
  (p.state = PState.ACTIVE            → p.pageProt = Prot.PROT_RW ∨ p.pageProt = Prot.PROT_READ) ∧
  (p.state = PState.COMPRESSED_SHADOW → p.pageProt = Prot.PROT_RW ∨ p.pageProt = Prot.PROT_READ)

def ActiveImpliesRW (p : Page) : Prop :=
  p.state = PState.ACTIVE → p.pageProt = Prot.PROT_RW

def SafetyInv (p : Page) : Prop :=
  BlobIntegrity p ∧ ProtectionSafety p ∧ ActiveImpliesRW p

-- ── The inductive strengthening WF ───────────────────────────────────────────
-- Pins each state's fields and the queue discipline.  `decide`-friendly: every
-- conjunct is an equality/disjunction over the finite Prot / PState / Bool.

def WF (p : Page) : Prop :=
  -- queue discipline (a page is on at most one queue)
  (p.onFreeList = true → p.state = PState.EMPTY ∧ p.pageProt = Prot.PROT_RW ∧ p.onDecommitQ = false) ∧
  (p.onDecommitQ = true → p.state = PState.EMPTY) ∧
  -- per-state field characterization
  (p.state = PState.EMPTY  → p.hasBlob = false ∧ p.hasPhysical = false) ∧
  (p.state = PState.ACTIVE → p.hasBlob = false ∧ p.hasPhysical = true ∧ p.pageProt = Prot.PROT_RW) ∧
  (p.state = PState.ACTIVE_MONITORING →
      p.hasBlob = false ∧ p.hasPhysical = true ∧ p.pageProt = Prot.PROT_READ) ∧
  (p.state = PState.COMPRESSING → p.hasBlob = false ∧ p.hasPhysical = true) ∧
  (p.state = PState.COMPRESSED →
      p.hasBlob = true ∧ p.hasPhysical = false ∧ p.pageProt = Prot.PROT_NONE) ∧
  (p.state = PState.COMPRESSED_SHADOW →
      p.hasBlob = true ∧ p.hasPhysical = true ∧
      (p.pageProt = Prot.PROT_RW ∨ p.pageProt = Prot.PROT_READ))

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

-- ── Initial state ────────────────────────────────────────────────────────────

def init : Page :=
  { state := PState.ACTIVE, pageProt := Prot.PROT_RW, hasBlob := false,
    hasPhysical := true, onDecommitQ := false, onFreeList := false }

theorem init_wf : WF init := by unfold WF init; refine ⟨?_,?_,?_,?_,?_,?_,?_,?_⟩ <;> intro h <;> simp_all

-- ── Transitions (single-page projections of the PlusCal actions) ─────────────

def faultRestore (p : Page) : Page :=
  match p.state with
  | PState.COMPRESSED =>
      { p with state := PState.ACTIVE, pageProt := Prot.PROT_RW,
               hasBlob := false, hasPhysical := true }
  | PState.COMPRESSED_SHADOW =>
      { p with state := PState.ACTIVE, pageProt := Prot.PROT_RW, hasBlob := false }
  | PState.ACTIVE_MONITORING =>
      { p with state := PState.ACTIVE, pageProt := Prot.PROT_RW }
  | PState.ACTIVE =>
      { p with pageProt := Prot.PROT_RW }   -- Phase-3 mprotect race
  | _ => p

/-- free(): any allocated (non-EMPTY) page → EMPTY, drop blob+physical, enqueue
    for decommit.  Crucially this fires even on a COMPRESSED page (PROT_NONE):
    the real free path does NOT decompress, so the page stays PROT_NONE until
    decommit.  pageProt is NOT touched here — that is precisely what makes the
    buggy decommit path observable downstream. -/
def appFree (p : Page) : Page :=
  if p.state ≠ PState.EMPTY ∧ p.onFreeList = false then
    { p with state := PState.EMPTY, hasBlob := false, hasPhysical := false,
             onDecommitQ := true }
  else p

/-- processDecommitEntry: fix (buggy=false) restores PROT_RW; bug (true) skips. -/
def decProcess (buggy : Bool) (p : Page) : Page :=
  if p.onDecommitQ = true then
    { p with onDecommitQ := false, onFreeList := true,
             pageProt := if buggy then p.pageProt else Prot.PROT_RW }
  else p

def allocPop (p : Page) : Page :=
  if p.onFreeList = true ∧ p.state = PState.EMPTY then
    { p with state := PState.ACTIVE, hasPhysical := true, onFreeList := false }
  else p

def compressReclaim (p : Page) : Page :=
  if (p.state = PState.ACTIVE ∨ p.state = PState.ACTIVE_MONITORING) ∧ p.onFreeList = false then
    { p with state := PState.COMPRESSED, pageProt := Prot.PROT_NONE,
             hasBlob := true, hasPhysical := false }
  else p

def compressDefer (p : Page) : Page :=
  if (p.state = PState.ACTIVE ∨ p.state = PState.ACTIVE_MONITORING) ∧ p.onFreeList = false then
    { p with state := PState.COMPRESSED_SHADOW, pageProt := Prot.PROT_RW, hasBlob := true }
  else p

def phaseBReclaim (p : Page) : Page :=
  if p.state = PState.COMPRESSED_SHADOW then
    { p with state := PState.COMPRESSED, pageProt := Prot.PROT_NONE, hasPhysical := false }
  else p

def phase3Monitor (p : Page) : Page :=
  if p.state = PState.ACTIVE then
    { p with state := PState.ACTIVE_MONITORING, pageProt := Prot.PROT_READ }
  else p

-- ═══════════════════════════════════════════════════════════════════
-- FIXED MODE: WF preserved by every transition (uniform: cases + simp_all)
-- ═══════════════════════════════════════════════════════════════════

theorem faultRestore_pres (p : Page) (h : WF p) : WF (faultRestore p) := by
  unfold WF faultRestore at *; cases hs : p.state <;> simp_all

theorem appFree_pres (p : Page) (h : WF p) : WF (appFree p) := by
  unfold WF appFree at *; cases hs : p.state <;> simp_all

theorem decProcess_fixed_pres (p : Page) (h : WF p) : WF (decProcess false p) := by
  unfold WF decProcess at *; cases hs : p.state <;>
    by_cases hq : p.onDecommitQ = true <;> simp_all

theorem allocPop_pres (p : Page) (h : WF p) : WF (allocPop p) := by
  unfold WF allocPop at *; cases hs : p.state <;>
    by_cases hf : p.onFreeList = true <;> simp_all

theorem compressReclaim_pres (p : Page) (h : WF p) : WF (compressReclaim p) := by
  unfold WF compressReclaim at *; cases hs : p.state <;> simp_all

theorem compressDefer_pres (p : Page) (h : WF p) : WF (compressDefer p) := by
  unfold WF compressDefer at *; cases hs : p.state <;> simp_all

theorem phaseBReclaim_pres (p : Page) (h : WF p) : WF (phaseBReclaim p) := by
  unfold WF phaseBReclaim at *; cases hs : p.state <;> simp_all

theorem phase3Monitor_pres (p : Page) (h : WF p) : WF (phase3Monitor p) := by
  unfold WF phase3Monitor at *; cases hs : p.state <;> simp_all

/-- Headline: in FIXED mode SafetyInv holds in every reachable state, because WF
    holds initially (`init_wf`), is preserved by every transition (`*_pres`),
    and implies SafetyInv (`wf_implies_safety`). -/
theorem fixed_mode_safe (p : Page) (h : WF p) : SafetyInv p := wf_implies_safety p h

-- ═══════════════════════════════════════════════════════════════════
-- BUGGY MODE: SafetyInv is reachably VIOLATED
-- ═══════════════════════════════════════════════════════════════════

/-- The full bug trace, reachable from `init`:
      init (ACTIVE, RW)
        → compressReclaim   (COMPRESSED, PROT_NONE, blob, no physical)
        → appFree           (EMPTY, PROT_NONE, on decommit queue)
        → decProcess true   (on freelist, PROT_NONE LEFT IN PLACE — the bug)
        → allocPop          (ACTIVE, PROT_NONE)
    End state: state = ACTIVE but pageProt = PROT_NONE. -/
def buggyTrace : Page :=
  allocPop (decProcess true (appFree (compressReclaim init)))

theorem buggy_state :
    buggyTrace.state = PState.ACTIVE ∧ buggyTrace.pageProt = Prot.PROT_NONE := by decide

theorem buggy_violates_safety : ¬ SafetyInv buggyTrace := by
  intro h
  have hrw : buggyTrace.pageProt = Prot.PROT_RW := h.2.2 buggy_state.1
  rw [buggy_state.2] at hrw
  exact absurd hrw (by decide)

/-- Contrast: the SAME trace in FIXED mode (decProcess false) restores PROT_RW,
    so the popped page is ACTIVE + PROT_RW and SafetyInv holds. -/
def fixedTrace : Page :=
  allocPop (decProcess false (appFree (compressReclaim init)))

theorem fixed_trace_ok :
    fixedTrace.state = PState.ACTIVE ∧ fixedTrace.pageProt = Prot.PROT_RW := by decide

/-- And the fixed trace is genuinely safe (SafetyInv holds at the end state). -/
theorem fixed_trace_safe : SafetyInv fixedTrace := by
  apply wf_implies_safety
  -- reachable from init via WF-preserving fixed-mode transitions
  exact allocPop_pres _ (decProcess_fixed_pres _ (appFree_pres _ (compressReclaim_pres _ init_wf)))
