/-
  Lean 4 formalization of the SmashDeferredProtNone safety property
  (companion model for the deferred-reclaim compress-snapshot PROT_NONE crash).

  THE BUG (root-caused 2026-07-08, full-mode rocksdb SIGSEGV / livelock; PR #55):
  In the default deferred-reclaim mode a compressor worker compresses a page by
  SNAPSHOTTING it: `memcpy(worker.page_buf, page_addr, kPageSize)`. Before the
  read, compressPage restored PROT_RW ONLY when the CAPTURED page state was
  ACTIVE_MONITORING:

      if (st == PageState::ACTIVE_MONITORING)
          vm::protectPages(page_addr, kPageSize, true, true);  // PROT_RW
      memcpy(worker.page_buf, page_addr, kPageSize);           // <-- may fault

  But an ACTIVE page can ALSO be physically PROT_NONE: escalateToDeepMonitoring
  (Linux, deferred mode) arms PROT_NONE for pre-compression read detection, and
  the page's *state* can read ACTIVE at that point. The worker then captures
  st = ACTIVE, SKIPS the restore, and reads a PROT_NONE-but-still-backed page →
  SIGSEGV (handleFault's reentrancy guard re-raises it fatally). Confirmed on
  cores: state = ACTIVE(1), page physically PROT_NONE.

  This is exactly SmashCore's `ActiveImpliesRW` (state = ACTIVE ⇒ PROT_RW) being
  violated by a transition SmashCore never modeled — the deferred-mode escalation
  ARM. SmashCore proved ActiveImpliesRW *inductive* over the transitions it knew;
  the arm is not one of them, and neither is the consumer READ that assumes it.
  This model adds both and checks the property AT THE READ.

  Parameterized by Variant:
    - guarded       : PRE-FIX — restore PROT_RW only if captured state was
                      ACTIVE_MONITORING. An ACTIVE+PROT_NONE page is read directly
                      → NoProtNoneRead is VIOLATED (concrete crash trace below).
    - unconditional : FIX (PR #55) — restore PROT_RW UNCONDITIONALLY before the
                      read. NoProtNoneRead HOLDS.

  Two safety properties:
    NoProtNoneRead : the worker never reads a PROT_NONE page (the crash). Holds
                     for `unconditional`, fails for `guarded`.
    NoStaleRead    : the worker never reads an UNBACKED page (would be silent
                     corruption). Holds for BOTH variants — the page is always
                     still backed (decommit only happens from COMPRESSED), so the
                     fix's restored read returns LIVE data, it does not read
                     decommitted zeros. This is why the fix is *correct*, not just
                     crash-free.

  Deadlock note: the fix's extra mprotect only LOOSENS protection (→ PROT_RW)
  under the per-page lock the worker already holds. Loosening cannot fault an app
  thread, so it does not reintroduce the TLB-shootdown-vs-page-lock hazard (which
  only applies to TIGHTENING). That is a structural argument; no lock/IPI is
  modeled here (the crash needs neither — it is a single-worker vs escalator
  protection desync).
-/

-- ── Abstractions (single contiguous arena page, deferred-reclaim mode) ────────

inductive PState where
  | ACTIVE | ACTIVE_MONITORING | COMPRESSING
  deriving DecidableEq, Repr

inductive Prot where
  | RW | NONE
  deriving DecidableEq, Repr

inductive Variant where
  | guarded | unconditional
  deriving DecidableEq, Repr

/-- Captured state at compressPage's eligibility check (line 1803). -/
inductive Captured where
  | NONE_YET | cap_ACTIVE | cap_MONITORING
  deriving DecidableEq, Repr

/-- Monitoring-escalator program counter. -/
inductive EscPC where
  | EIdle | EArmed
  deriving DecidableEq, Repr

/-- Compressor-worker program counter, mirroring compressPage's deferred branch. -/
inductive WorkerPC where
  | WIdle | WCaptured | WDone
  deriving DecidableEq, Repr

structure State where
  pstate    : PState
  prot      : Prot
  backed    : Bool          -- physical backing present (never dropped while non-COMPRESSED)
  stcap     : Captured      -- state captured at the eligibility check
  epc       : EscPC
  wpc       : WorkerPC
  badRead   : Bool          -- ghost: worker read a PROT_NONE page (the SIGSEGV)
  staleRead : Bool          -- ghost: worker read an UNBACKED page (silent corruption)
  deriving Repr

def init : State :=
  { pstate := PState.ACTIVE, prot := Prot.RW, backed := true,
    stcap := Captured.NONE_YET, epc := EscPC.EIdle, wpc := WorkerPC.WIdle,
    badRead := false, staleRead := false }

-- ── Monitoring escalator (Phase 3 + escalateToDeepMonitoring) ─────────────────

/-- Arm PROT_NONE for pre-compression READ detection. The page stays physically
    BACKED (mprotect only; no madvise) and the state left behind is ACTIVE — this
    models the observed desync where a concurrent handleFault/phase transition
    leaves state ACTIVE while the escalation's PROT_NONE lingers. -/
def stepEscalate (s : State) : State :=
  if s.epc = EscPC.EIdle ∧ (s.pstate = PState.ACTIVE ∨ s.pstate = PState.ACTIVE_MONITORING) then
    { s with prot := Prot.NONE, pstate := PState.ACTIVE, epc := EscPC.EArmed }
  else s

-- ── Compressor worker (compressPage, deferred branch) ─────────────────────────

/-- compressPage eligibility check: capture the current page state, mark
    COMPRESSING (line 1803/1810). -/
def stepWorkerCapture (s : State) : State :=
  if s.wpc = WorkerPC.WIdle ∧ (s.pstate = PState.ACTIVE ∨ s.pstate = PState.ACTIVE_MONITORING) then
    { s with
        stcap := (if s.pstate = PState.ACTIVE_MONITORING then Captured.cap_MONITORING
                  else Captured.cap_ACTIVE),
        pstate := PState.COMPRESSING, wpc := WorkerPC.WCaptured }
  else s

/-- The post-restore protection the snapshot reads under.
    guarded       : restore RW only if captured state was ACTIVE_MONITORING.
    unconditional : always restore RW (the fix). -/
def postProt (v : Variant) (s : State) : Prot :=
  match v with
  | Variant.unconditional => Prot.RW
  | Variant.guarded =>
      if s.stcap = Captured.cap_MONITORING then Prot.RW else s.prot

/-- The deferred-branch restore + snapshot read `memcpy(buf, page_addr)`. Faults
    iff prot = NONE (badRead); reads garbage iff ¬backed (staleRead). -/
def stepWorkerSnapshot (v : Variant) (s : State) : State :=
  if s.wpc = WorkerPC.WCaptured then
    let p := postProt v s
    { s with prot := p,
             badRead := s.badRead || (p = Prot.NONE),
             staleRead := s.staleRead || (!s.backed),
             wpc := WorkerPC.WDone }
  else s

-- ── Safety properties ─────────────────────────────────────────────────────────

/-- NoProtNoneRead: the worker never read a PROT_NONE page (the crash). -/
def NoProtNoneRead (s : State) : Prop := s.badRead = false

/-- NoStaleRead: the worker never read an UNBACKED page (silent corruption). -/
def NoStaleRead (s : State) : Prop := s.staleRead = false

-- ═══════════════════════════════════════════════════════════════════
-- UNCONDITIONAL variant (the fix): NoProtNoneRead holds via an inductive
-- invariant. The crux: the snapshot always restores PROT_RW before reading, so
-- the read is never under PROT_NONE regardless of what the escalator armed.
-- ═══════════════════════════════════════════════════════════════════

/-- I1 no bad read; I2 backing never dropped (no non-COMPRESSED madvise). -/
def FixedInv (s : State) : Prop :=
  s.badRead = false ∧ s.backed = true

theorem fixed_init_inv : FixedInv init := ⟨rfl, rfl⟩

theorem fixed_escalate_preserves (s : State) (h : FixedInv s) :
    FixedInv (stepEscalate s) := by
  obtain ⟨h1, h2⟩ := h
  unfold stepEscalate
  by_cases hc : s.epc = EscPC.EIdle ∧ (s.pstate = PState.ACTIVE ∨ s.pstate = PState.ACTIVE_MONITORING)
  · simp only [if_pos hc]; exact ⟨h1, h2⟩       -- badRead, backed unchanged
  · simp only [if_neg hc]; exact ⟨h1, h2⟩

theorem fixed_capture_preserves (s : State) (h : FixedInv s) :
    FixedInv (stepWorkerCapture s) := by
  obtain ⟨h1, h2⟩ := h
  unfold stepWorkerCapture
  by_cases hc : s.wpc = WorkerPC.WIdle ∧ (s.pstate = PState.ACTIVE ∨ s.pstate = PState.ACTIVE_MONITORING)
  · simp only [if_pos hc]; exact ⟨h1, h2⟩       -- badRead, backed unchanged
  · simp only [if_neg hc]; exact ⟨h1, h2⟩

/-- The key lemma: in the unconditional variant the post-restore protection is
    always RW, so the snapshot read never sets badRead, and backing is intact so
    it never sets staleRead. -/
theorem fixed_snapshot_preserves (s : State) (h : FixedInv s) :
    FixedInv (stepWorkerSnapshot Variant.unconditional s) := by
  obtain ⟨h1, h2⟩ := h
  unfold stepWorkerSnapshot
  by_cases hc : s.wpc = WorkerPC.WCaptured
  · simp only [if_pos hc]
    refine ⟨?_, h2⟩
    -- postProt unconditional s = RW, so (p = NONE) is false; badRead stays false.
    simp [postProt, h1]
  · simp only [if_neg hc]; exact ⟨h1, h2⟩

/-- FixedInv ⇒ NoProtNoneRead. Holds initially and is preserved by every
    transition of the unconditional variant, so it holds in all reachable states. -/
theorem fixed_inv_implies_safe (s : State) (h : FixedInv s) : NoProtNoneRead s := h.1

-- ═══════════════════════════════════════════════════════════════════
-- Both variants preserve NoStaleRead: the page is always backed (no
-- non-COMPRESSED madvise in this model), so no read is ever stale. Proven via
-- the invariant `backed = true ∧ staleRead = false`, variant-generic.
-- ═══════════════════════════════════════════════════════════════════

def BackedInv (s : State) : Prop := s.backed = true ∧ s.staleRead = false

theorem backed_init : BackedInv init := ⟨rfl, rfl⟩

theorem backed_escalate (s : State) (h : BackedInv s) : BackedInv (stepEscalate s) := by
  obtain ⟨h1, h2⟩ := h
  unfold stepEscalate
  by_cases hc : s.epc = EscPC.EIdle ∧ (s.pstate = PState.ACTIVE ∨ s.pstate = PState.ACTIVE_MONITORING)
  · simp only [if_pos hc]; exact ⟨h1, h2⟩
  · simp only [if_neg hc]; exact ⟨h1, h2⟩

theorem backed_capture (s : State) (h : BackedInv s) : BackedInv (stepWorkerCapture s) := by
  obtain ⟨h1, h2⟩ := h
  unfold stepWorkerCapture
  by_cases hc : s.wpc = WorkerPC.WIdle ∧ (s.pstate = PState.ACTIVE ∨ s.pstate = PState.ACTIVE_MONITORING)
  · simp only [if_pos hc]; exact ⟨h1, h2⟩
  · simp only [if_neg hc]; exact ⟨h1, h2⟩

theorem backed_snapshot (v : Variant) (s : State) (h : BackedInv s) :
    BackedInv (stepWorkerSnapshot v s) := by
  obtain ⟨h1, h2⟩ := h
  unfold stepWorkerSnapshot
  by_cases hc : s.wpc = WorkerPC.WCaptured
  · simp only [if_pos hc]
    -- staleRead' = staleRead || !backed = false || !true = false
    refine ⟨h1, ?_⟩
    simp [h1, h2]
  · simp only [if_neg hc]; exact ⟨h1, h2⟩

theorem backed_inv_implies_safe (s : State) (h : BackedInv s) : NoStaleRead s := h.2

-- ═══════════════════════════════════════════════════════════════════
-- GUARDED variant (pre-fix): NoProtNoneRead is VIOLATED — the crash trace.
-- ═══════════════════════════════════════════════════════════════════

/-- The exact crashing interleaving:
      Escalate arms PROT_NONE, leaving state ACTIVE (backed),
      WorkerCapture captures st = ACTIVE (→ COMPRESSING, WCaptured),
      WorkerSnapshot: guarded restore is a no-op (captured state was ACTIVE, not
        ACTIVE_MONITORING), so the read happens under PROT_NONE → badRead. -/
def guardedCrashState : State :=
  stepWorkerSnapshot Variant.guarded
    (stepWorkerCapture
      (stepEscalate init))

/-- In the guarded variant the worker reads a PROT_NONE page: NoProtNoneRead is
    violated. This is the modeled counterexample for the deferred-mode crash. -/
theorem guarded_violates_safe : ¬ NoProtNoneRead guardedCrashState := by
  unfold NoProtNoneRead guardedCrashState stepWorkerSnapshot stepWorkerCapture
         stepEscalate postProt init
  decide

/-- The crash is a FAULT, not silent corruption: even on the guarded crash
    trace, NoStaleRead still holds (the page stays backed). -/
theorem guarded_crash_is_fault_not_corruption : NoStaleRead guardedCrashState := by
  unfold NoStaleRead guardedCrashState stepWorkerSnapshot stepWorkerCapture
         stepEscalate postProt init
  decide

/-- For contrast: under the fix, the SAME schedule (escalate arms PROT_NONE, then
    the worker captures ACTIVE and snapshots) does NOT crash — the unconditional
    restore re-establishes PROT_RW before the read. -/
theorem fixed_same_schedule_safe :
    NoProtNoneRead
      (stepWorkerSnapshot Variant.unconditional
        (stepWorkerCapture
          (stepEscalate init))) := by
  unfold NoProtNoneRead stepWorkerSnapshot stepWorkerCapture stepEscalate postProt init
  decide
