/-
  Lean 4 formalization of the SmashExternalRace safety property
  (companion model for the full-mode external-page munmap-vs-compress race).

  THE BUG (root-caused 2026-06-24, full-mode rocksdb SIGSEGV in compressPage):
  In full mode smash tracks application-direct mmap regions as "external" pages
  so the compressor can compress them. A compressor worker compresses a page by
  SNAPSHOTTING it: `memcpy(worker.page_buf, page_addr, kPageSize)`, where
  page_addr = VmRegion::track_reverse_[idx]. When the application munmap()s that
  region, the munmap interposer (deregisterLinuxExternalRange) sets the
  PageState to EMPTY, untracks, and calls real_munmap — but WITHOUT taking the
  per-page lock. So this interleaving crashes:

      Worker:  tryLock(idx) ✓ ; state==ACTIVE ✓ ; state := COMPRESSING
      munmap:  state := EMPTY (NO LOCK) ; untrack ; real_munmap(addr)   ← unmapped
      Worker:  memcpy(buf, page_addr, 4096)                            ← SIGSEGV

  Parameterized by Variant:
    - unlocked : PRE-FIX — munmap mutates state + unmaps WITHOUT the lock.
                 NoUnmappedRead is VIOLATED (concrete crash trace below).
    - locked   : FIX — munmap must acquire the per-page lock before clearing
                 state / unmapping. While a worker holds the lock across its
                 check→snapshot, munmap cannot run. NoUnmappedRead HOLDS.

  Deadlock note (user-flagged): the locked fix serializes munmap-untrack against
  the worker SNAPSHOT (a plain memcpy — neither side does a TLB-shootdown IPI
  while holding the lock) and releases the lock BEFORE real_munmap, so it does
  not reintroduce the documented TLB-shootdown-vs-page-lock deadlock
  (compressor_thread.h:1925). The model proves the SAFETY property and mutual
  exclusion (`locked_mutual_exclusion`); deadlock-FREEDOM (a liveness claim) is
  argued structurally from "no lock is ever held across an IPI", not checked here.

  Safety invariant (NoUnmappedRead): a worker never snapshots an unmapped page.
-/

-- ── Abstractions (single external page) ──────────────────────────────────────

inductive PState where
  | ACTIVE | COMPRESSING | EMPTY
  deriving DecidableEq, Repr

inductive Lock where
  | free | worker | munmapper
  deriving DecidableEq, Repr

inductive Variant where
  | unlocked | locked
  deriving DecidableEq, Repr

/-- Compressor-worker program counter, mirroring compressPage. -/
inductive WorkerPC where
  | WIdle | WSnapshot | WDone
  deriving DecidableEq, Repr

/-- munmap interposer program counter. -/
inductive MunmapPC where
  | MIdle | MUnmap | MDone
  deriving DecidableEq, Repr

structure State where
  pstate  : PState
  lock    : Lock
  mapped  : Bool         -- page's physical backing still mmap'd?
  wpc     : WorkerPC
  mpc     : MunmapPC
  badRead : Bool         -- ghost: worker read an UNMAPPED page (the crash)
  deriving Repr

def init : State :=
  { pstate := PState.ACTIVE, lock := Lock.free, mapped := true,
    wpc := WorkerPC.WIdle, mpc := MunmapPC.MIdle, badRead := false }

-- ── Compressor worker transitions ────────────────────────────────────────────

/-- compressPage entry (deferred mode): tryLock + verify still ACTIVE, then mark
    COMPRESSING and advance to the snapshot. tryLock succeeds only when the lock
    is free; on success the worker HOLDS the lock. -/
def stepWorkerLock (s : State) : State :=
  if s.wpc = WorkerPC.WIdle ∧ s.lock = Lock.free ∧ s.pstate = PState.ACTIVE then
    { s with lock := Lock.worker, pstate := PState.COMPRESSING,
             wpc := WorkerPC.WSnapshot }
  else s

/-- The snapshot read `memcpy(buf, page_addr, kPageSize)`. If the page has been
    unmapped this is a wild read → record badRead (the SIGSEGV). Then unlock. -/
def stepWorkerSnapshot (s : State) : State :=
  if s.wpc = WorkerPC.WSnapshot then
    { s with badRead := s.badRead || (!s.mapped),
             lock := Lock.free, wpc := WorkerPC.WDone }
  else s

-- ── munmap interposer transitions (variant-parameterized) ────────────────────

/-- deregisterLinuxExternalRange: set state EMPTY + untrack.
    unlocked: fires regardless of the lock (the bug).
    locked:   only when the lock is free; acquires it (the fix). -/
def stepMunmapClear (v : Variant) (s : State) : State :=
  if s.mpc = MunmapPC.MIdle then
    match v with
    | Variant.unlocked =>
        { s with pstate := PState.EMPTY, mpc := MunmapPC.MUnmap }
    | Variant.locked =>
        if s.lock = Lock.free then
          { s with lock := Lock.munmapper, pstate := PState.EMPTY,
                   mpc := MunmapPC.MUnmap }
        else s
  else s

/-- real_munmap: tear down the physical mapping. In the locked variant the lock
    is released here (before/with the unmap — modeled together; no IPI held). -/
def stepMunmapUnmap (v : Variant) (s : State) : State :=
  if s.mpc = MunmapPC.MUnmap then
    match v with
    | Variant.unlocked => { s with mapped := false, mpc := MunmapPC.MDone }
    | Variant.locked   =>
        { s with mapped := false, lock := Lock.free, mpc := MunmapPC.MDone }
  else s

-- ── Safety invariant ─────────────────────────────────────────────────────────

/-- NoUnmappedRead: the worker never read an unmapped page. -/
def NoUnmappedRead (s : State) : Prop := s.badRead = false

-- ═══════════════════════════════════════════════════════════════════
-- LOCKED variant: NoUnmappedRead holds via an inductive invariant.
-- The crux is mutual exclusion on the single per-page lock: the worker holds
-- it while poised to snapshot, and munmap holds it across clear→unmap, so the
-- two critical sections cannot overlap and the page stays mapped under the
-- worker's snapshot.
-- ═══════════════════════════════════════════════════════════════════

/-- I1 no bad read; I2 ACTIVE ⇒ mapped; I3 worker-poised ⇒ mapped ∧ holds lock;
    I4 munmap-mid ⇒ state EMPTY ∧ holds lock. -/
def LockedInv (s : State) : Prop :=
  s.badRead = false
  ∧ (s.pstate = PState.ACTIVE → s.mapped = true)
  ∧ (s.wpc = WorkerPC.WSnapshot → s.mapped = true ∧ s.lock = Lock.worker)
  ∧ (s.mpc = MunmapPC.MUnmap → s.pstate = PState.EMPTY ∧ s.lock = Lock.munmapper)

theorem locked_init_inv : LockedInv init := by
  refine ⟨rfl, ?_, ?_, ?_⟩
  · intro _; rfl                       -- ACTIVE ⇒ mapped: init.mapped = true
  · intro h; simp [init] at h          -- wpc = WIdle ≠ WSnapshot
  · intro h; simp [init] at h          -- mpc = MIdle ≠ MUnmap

theorem locked_worker_lock_preserves (s : State) (h : LockedInv s) :
    LockedInv (stepWorkerLock s) := by
  obtain ⟨h1, h2, h3, h4⟩ := h
  unfold stepWorkerLock
  by_cases hc : s.wpc = WorkerPC.WIdle ∧ s.lock = Lock.free ∧ s.pstate = PState.ACTIVE
  · have hfree : s.lock = Lock.free := hc.2.1
    have hmap : s.mapped = true := h2 hc.2.2
    have hmpc : s.mpc ≠ MunmapPC.MUnmap := by
      intro hm; have := (h4 hm).2; rw [hfree] at this; exact absurd this (by decide)
    simp only [if_pos hc]
    refine ⟨h1, ?_, ?_, ?_⟩
    · intro h; simp at h                    -- post pstate = COMPRESSING ≠ ACTIVE
    · intro _; exact ⟨hmap, rfl⟩            -- post mapped = s.mapped = true, lock = worker
    · intro hm; exact absurd hm hmpc        -- post mpc unchanged ≠ MUnmap
  · simp only [if_neg hc]; exact ⟨h1, h2, h3, h4⟩

theorem locked_worker_snapshot_preserves (s : State) (h : LockedInv s) :
    LockedInv (stepWorkerSnapshot s) := by
  obtain ⟨h1, h2, h3, h4⟩ := h
  unfold stepWorkerSnapshot
  by_cases hc : s.wpc = WorkerPC.WSnapshot
  · have hmap := (h3 hc).1
    have hlw := (h3 hc).2
    have hmpc : s.mpc ≠ MunmapPC.MUnmap := by
      intro hm; have := (h4 hm).2; rw [hlw] at this; exact absurd this (by decide)
    simp only [if_pos hc]
    refine ⟨by simp [h1, hmap], ?_, ?_, ?_⟩
    · intro hact; exact h2 hact
    · intro h; simp at h                    -- post wpc = WDone ≠ WSnapshot
    · intro hm; exact absurd hm hmpc        -- post mpc unchanged ≠ MUnmap
  · simp only [if_neg hc]; exact ⟨h1, h2, h3, h4⟩

theorem locked_munmap_clear_preserves (s : State) (h : LockedInv s) :
    LockedInv (stepMunmapClear Variant.locked s) := by
  obtain ⟨h1, h2, h3, h4⟩ := h
  unfold stepMunmapClear
  by_cases hc : s.mpc = MunmapPC.MIdle
  · by_cases hfree : s.lock = Lock.free
    · have hwpc : s.wpc ≠ WorkerPC.WSnapshot := by
        intro hw; have := (h3 hw).2; rw [hfree] at this; exact absurd this (by decide)
      simp only [if_pos hc, if_pos hfree]
      refine ⟨h1, ?_, ?_, ?_⟩
      · intro h; simp at h                       -- post pstate = EMPTY ≠ ACTIVE
      · intro hw; exact absurd hw hwpc           -- post wpc unchanged ≠ WSnapshot
      · intro _; exact ⟨rfl, rfl⟩                -- post pstate=EMPTY, lock=munmapper
    · simp only [if_pos hc, if_neg hfree]; exact ⟨h1, h2, h3, h4⟩
  · simp only [if_neg hc]; exact ⟨h1, h2, h3, h4⟩

theorem locked_munmap_unmap_preserves (s : State) (h : LockedInv s) :
    LockedInv (stepMunmapUnmap Variant.locked s) := by
  obtain ⟨h1, h2, h3, h4⟩ := h
  unfold stepMunmapUnmap
  by_cases hc : s.mpc = MunmapPC.MUnmap
  · obtain ⟨hempty, hlock⟩ := h4 hc
    have hwpc : s.wpc ≠ WorkerPC.WSnapshot := by
      intro hw; have := (h3 hw).2; rw [hlock] at this; exact absurd this (by decide)
    simp only [if_pos hc]
    refine ⟨h1, ?_, ?_, ?_⟩
    · intro hact; rw [hempty] at hact; exact absurd hact (by decide)  -- pstate EMPTY
    · intro hw; exact absurd hw hwpc            -- post wpc unchanged ≠ WSnapshot
    · intro hm; simp at hm                      -- post mpc = MDone ≠ MUnmap
  · simp only [if_neg hc]; exact ⟨h1, h2, h3, h4⟩

/-- LockedInv ⇒ NoUnmappedRead. Holds initially and is preserved by every
    transition of the locked variant, so it holds in all reachable states. -/
theorem locked_inv_implies_safe (s : State) (h : LockedInv s) : NoUnmappedRead s := h.1

/-- Mutual exclusion: under LockedInv, the worker's snapshot critical section and
    munmap's unmap critical section never overlap (the single lock can't be held
    by both). This is what makes the read-vs-unmap race impossible. -/
theorem locked_mutual_exclusion (s : State) (h : LockedInv s) :
    ¬ (s.wpc = WorkerPC.WSnapshot ∧ s.mpc = MunmapPC.MUnmap) := by
  obtain ⟨_, _, h3, h4⟩ := h
  rintro ⟨hw, hm⟩
  have hlw := (h3 hw).2     -- lock = worker
  have hlm := (h4 hm).2     -- lock = munmapper
  rw [hlw] at hlm; exact absurd hlm (by decide)

-- ═══════════════════════════════════════════════════════════════════
-- UNLOCKED variant: NoUnmappedRead is VIOLATED — the concrete crash trace.
-- ═══════════════════════════════════════════════════════════════════

/-- The exact crashing interleaving:
      Worker locks + marks COMPRESSING (→ WSnapshot, holding the lock),
      munmap (no lock!) clears state to EMPTY,
      munmap real_munmap drops the mapping,
      Worker performs the snapshot read on the now-unmapped page. -/
def unlockedCrashState : State :=
  stepWorkerSnapshot
    (stepMunmapUnmap Variant.unlocked
      (stepMunmapClear Variant.unlocked
        (stepWorkerLock init)))

/-- In the unlocked variant the worker reads an unmapped page: NoUnmappedRead is
    violated. This is the modeled counterexample for the full-mode crash. -/
theorem unlocked_violates_safe : ¬ NoUnmappedRead unlockedCrashState := by
  unfold NoUnmappedRead unlockedCrashState stepWorkerSnapshot stepMunmapUnmap
         stepMunmapClear stepWorkerLock init
  decide

/-- For contrast: under the locked fix, the same schedule cannot reach the bad
    read. Starting from init, run the worker to its snapshot and let munmap try
    to clear: munmap is blocked (lock held by worker), so its clear is a no-op,
    the page stays mapped, and the worker's snapshot sets no badRead. -/
theorem locked_same_schedule_safe :
    NoUnmappedRead
      (stepWorkerSnapshot
        (stepMunmapUnmap Variant.locked
          (stepMunmapClear Variant.locked
            (stepWorkerLock init)))) := by
  unfold NoUnmappedRead stepWorkerSnapshot stepMunmapUnmap
         stepMunmapClear stepWorkerLock init
  decide
