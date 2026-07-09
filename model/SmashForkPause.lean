/-
  Lean 4 formalization of the compressor fork-pause handshake
  (companion to model/SmashForkPause.tla; code in compressor_thread.h
  coordEntry / pauseForFork).

  tick() runs holding per-page and CompressStore shard spinlocks, so fork()
  must never fire while the coordinator is inside the tick body: the child
  would inherit those locks in the locked state with no owner (permanent
  spin on first touch).

  Coordinator orderings:
    • check-then-set (pre-fix): if (paused) continue; inTick := true; tick()
      — a TOCTOU: prepare can set paused and observe inTick = false in the
      gap, letting fork() proceed concurrently with tick().
    • set-then-check (fixed, Dekker-style): inTick := true; if (paused)
      { inTick := false; continue; } tick() — both sides store first and
      load second, so under sequential consistency (the code uses seq_cst
      for exactly this) at least one side observes the other's store.

  Results:
    • `reachable_safe`  — in the FIXED system, NoForkDuringTick holds in
      every reachable state (inductive invariant `WFP`).
    • `buggy_reachable_violates` — the pre-fix ordering reaches a state
      with forked = true ∧ pcC = body.

  Like SmashForkPause.tla, the 50 ms cap on the prepare wait loop is NOT
  modeled (it is a deliberate best-effort tradeoff); pWait fires only by
  observing inTick = false.
-/

namespace SmashForkPause

inductive PcC where
  | idle | bCheck | bSet | fSet | fCheck | body | clear
  deriving DecidableEq, Repr

inductive PcP where
  | idle | wait | fork | done
  deriving DecidableEq, Repr

open PcC PcP

structure St where
  paused : Bool
  inTick : Bool
  forked : Bool
  pcC    : PcC
  pcP    : PcP
  deriving DecidableEq, Repr

-- ── Safety property ───────────────────────────────────────────────────────────

/-- fork() must never have fired while the coordinator is inside tick(). -/
def Safe (s : St) : Prop := ¬(s.forked = true ∧ s.pcC = body)

def init : St :=
  { paused := false, inTick := false, forked := false, pcC := idle, pcP := idle }

-- ── Transitions (guarded; disabled ⇒ identity) ───────────────────────────────

-- Coordinator, FIXED ordering: idle → fSet → fCheck → {body | idle} → …
def cIdleF (s : St) : St := if s.pcC = PcC.idle then { s with pcC := fSet } else s

def fSetT (s : St) : St :=
  if s.pcC = fSet then { s with inTick := true, pcC := fCheck } else s

def fCheckT (s : St) : St :=
  if s.pcC = fCheck then
    if s.paused then { s with inTick := false, pcC := PcC.idle }
    else { s with pcC := body }
  else s

-- Coordinator, PRE-FIX ordering: idle → bCheck → bSet → body → …
def cIdleB (s : St) : St := if s.pcC = PcC.idle then { s with pcC := bCheck } else s

def bCheckT (s : St) : St :=
  if s.pcC = bCheck then
    if s.paused then { s with pcC := PcC.idle } else { s with pcC := bSet }
  else s

def bSetT (s : St) : St :=
  if s.pcC = bSet then { s with inTick := true, pcC := body } else s

-- tick body and the trailing inTick clear (shared by both orderings).
def bodyT (s : St) : St := if s.pcC = body then { s with pcC := clear } else s

def clearT (s : St) : St :=
  if s.pcC = clear then { s with inTick := false, pcC := PcC.idle } else s

-- Prepare handler: set paused → wait for inTick = false → fork.
def pSetT (s : St) : St :=
  if s.pcP = PcP.idle then { s with paused := true, pcP := wait } else s

def pWaitT (s : St) : St :=
  if s.pcP = wait ∧ s.inTick = false then { s with pcP := fork } else s

def pForkT (s : St) : St :=
  if s.pcP = fork then { s with forked := true, pcP := PcP.done } else s

-- ── FIXED system: transition relation and reachability ───────────────────────

inductive Trans : St → St → Prop where
  | cIdle  (s : St) : Trans s (cIdleF s)
  | fSet   (s : St) : Trans s (fSetT s)
  | fCheck (s : St) : Trans s (fCheckT s)
  | body   (s : St) : Trans s (bodyT s)
  | clear  (s : St) : Trans s (clearT s)
  | pSet   (s : St) : Trans s (pSetT s)
  | pWait  (s : St) : Trans s (pWaitT s)
  | pFork  (s : St) : Trans s (pForkT s)

inductive Reachable : St → Prop where
  | init : Reachable init
  | step {s t : St} : Reachable s → Trans s t → Reachable t

-- ── Inductive invariant for the fixed ordering ───────────────────────────────
--
--  (1) once prepare has started, paused stays true;
--  (2,3) the coordinator holds inTick from the fSet publish all the way
--      through fCheck / body / clear;
--  (4) forked only at pcP = done;
--  (5) the load-bearing clause: once prepare passed its wait (pcP ∈
--      {fork, done}), the coordinator is not in the body — pWait's
--      inTick = false guard excludes body via (3), and once paused is
--      set fCheck can never re-enter body.

def WFP (s : St) : Prop :=
  (s.pcP ≠ PcP.idle → s.paused = true) ∧
  (s.pcC = fCheck → s.inTick = true) ∧
  (s.pcC = body ∨ s.pcC = clear → s.inTick = true) ∧
  (s.forked = true → s.pcP = PcP.done) ∧
  ((s.pcP = fork ∨ s.pcP = PcP.done) → s.pcC ≠ body)

theorem wfp_implies_safe (s : St) (h : WFP s) : Safe s := by
  obtain ⟨-, -, -, h4, h5⟩ := h
  intro ⟨hf, hb⟩
  exact h5 (Or.inr (h4 hf)) hb

theorem init_wfp : WFP init := by unfold WFP init; simp

-- Preservation: every fixed-mode transition maps WFP to WFP.

theorem cIdleF_pres (s : St) (h : WFP s) : WFP (cIdleF s) := by
  unfold WFP cIdleF at *; cases hc : s.pcC <;> simp_all

theorem fSetT_pres (s : St) (h : WFP s) : WFP (fSetT s) := by
  unfold WFP fSetT at *; cases hc : s.pcC <;> simp_all

theorem fCheckT_pres (s : St) (h : WFP s) : WFP (fCheckT s) := by
  unfold WFP fCheckT at *
  cases hc : s.pcC <;> by_cases hp : s.paused = true <;>
    cases hq : s.pcP <;> simp_all

theorem bodyT_pres (s : St) (h : WFP s) : WFP (bodyT s) := by
  unfold WFP bodyT at *; cases hc : s.pcC <;> simp_all

theorem clearT_pres (s : St) (h : WFP s) : WFP (clearT s) := by
  unfold WFP clearT at *; cases hc : s.pcC <;> simp_all

theorem pSetT_pres (s : St) (h : WFP s) : WFP (pSetT s) := by
  unfold WFP pSetT at *; cases hq : s.pcP <;> simp_all

theorem pWaitT_pres (s : St) (h : WFP s) : WFP (pWaitT s) := by
  unfold WFP pWaitT at *
  cases hq : s.pcP <;> cases hc : s.pcC <;>
    by_cases hi : s.inTick = false <;> simp_all

theorem pForkT_pres (s : St) (h : WFP s) : WFP (pForkT s) := by
  unfold WFP pForkT at *; cases hq : s.pcP <;> simp_all

theorem trans_pres {s t : St} (h : Trans s t) (hs : WFP s) : WFP t := by
  cases h with
  | cIdle  => exact cIdleF_pres _ hs
  | fSet   => exact fSetT_pres _ hs
  | fCheck => exact fCheckT_pres _ hs
  | body   => exact bodyT_pres _ hs
  | clear  => exact clearT_pres _ hs
  | pSet   => exact pSetT_pres _ hs
  | pWait  => exact pWaitT_pres _ hs
  | pFork  => exact pForkT_pres _ hs

/-- MAIN THEOREM (fixed ordering): fork() never fires while the coordinator
    is inside tick(), in every reachable state. -/
theorem reachable_safe {s : St} (h : Reachable s) : Safe s := by
  have hw : WFP s := by
    induction h with
    | init => exact init_wfp
    | step _ ht ih => exact trans_pres ht ih
  exact wfp_implies_safe s hw

-- ── PRE-FIX ordering: the violation is reachable ─────────────────────────────
--
-- Trace: coordinator passes its paused check (bCheck, paused = false);
-- prepare then sets paused, observes inTick = false (not yet stored!),
-- and fork() proceeds; the coordinator continues bSet → body.

def buggyTrace : St :=
  bSetT (pForkT (pWaitT (pSetT (bCheckT (cIdleB init)))))

theorem buggy_reachable_violates :
    buggyTrace.forked = true ∧ buggyTrace.pcC = body := by decide

theorem buggy_not_safe : ¬ Safe buggyTrace := by
  intro h; exact h buggy_reachable_violates

end SmashForkPause
