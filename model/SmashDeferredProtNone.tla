------------------------- MODULE SmashDeferredProtNone -------------------------
(*
 * SmashDeferredProtNone — models the deferred-reclaim compress-snapshot crash
 * that SIGSEGVs CompressorThread::compressPage (root-caused 2026-07-08, full-mode
 * rocksdb SIGSEGV / livelock; fixed in PR #55). Companion to
 * model/SmashDeferredProtNone.lean, which carries the machine-checked proof
 * (zero axioms beyond propext).
 *
 * THE BUG. In the default deferred-reclaim mode, a compressor worker compresses a
 * page by SNAPSHOTTING it: memcpy(worker.page_buf, page_addr, kPageSize). Before
 * the read, compressPage restored PROT_RW ONLY when the captured page state was
 * ACTIVE_MONITORING:
 *
 *     if (st == PageState::ACTIVE_MONITORING)
 *         vm::protectPages(page_addr, kPageSize, true, true);  // PROT_RW
 *     memcpy(worker.page_buf, page_addr, kPageSize);           // <-- may fault
 *
 * But an ACTIVE page can ALSO be physically PROT_NONE at that point:
 * escalateToDeepMonitoring (Linux, deferred mode) arms PROT_NONE on a page for
 * pre-compression read detection while its state stays ACTIVE / ACTIVE_MONITORING.
 * When the worker later captures st == ACTIVE, it SKIPS the PROT_RW restore and
 * reads a PROT_NONE-but-still-backed page → SIGSEGV (handleFault's reentrancy
 * guard re-raises it fatally). Confirmed on cores: state ACTIVE(1), page PROT_NONE.
 *
 * This is exactly SmashCore's ActiveImpliesRW invariant (state = ACTIVE ⇒
 * pageProt = PROT_RW) being violated by a transition SmashCore never modeled —
 * the deferred-mode escalation arm. SmashCore proved ActiveImpliesRW *inductive*
 * over the transitions it knew about; the escalation arm is not one of them, so
 * the desync (and the consumer read that assumes it can't happen) were outside
 * the modeled state space. This model adds both: the escalation ARM (producer)
 * and the snapshot READ (consumer), and checks the property AT THE READ.
 *
 * Variant:
 *   "guarded"       : PRE-FIX — the snapshot restores PROT_RW only when the
 *                     captured state is ACTIVE_MONITORING. An ACTIVE+PROT_NONE
 *                     page is read directly → NoProtNoneRead is VIOLATED.
 *   "unconditional" : FIX (PR #55) — the snapshot restores PROT_RW
 *                     UNCONDITIONALLY before the read. NoProtNoneRead HOLDS.
 *
 * Why the fix is safe, not just crash-free: the page is always still BACKED here
 * (never madvise'd away while ACTIVE — decommit only happens from COMPRESSED),
 * so restoring PROT_RW recovers the live bytes rather than reading zeros. The
 * `backed` variable tracks this: NoStaleRead asserts the worker never reads an
 * unbacked page either, and it holds in BOTH variants (the fix does not trade a
 * crash for silent corruption).
 *
 * Deadlock note: the fix's extra mprotect only LOOSENS protection (→ PROT_RW)
 * and runs under the per-page lock the worker already holds. Loosening cannot
 * fault an app thread, so it does not reintroduce the TLB-shootdown-vs-page-lock
 * hazard (which only applies to TIGHTENING protection, compressor_thread.h). No
 * lock/IPI modeling is needed for that argument; it is structural.
 *
 * TLC results (run with `-deadlock`; terminating model, not reactive):
 *   _guarded_buggy.cfg       : Invariant NoProtNoneRead is VIOLATED (crash trace).
 *   _unconditional_fixed.cfg : No error — NoProtNoneRead AND NoStaleRead hold.
 * Agrees with SmashDeferredProtNone.lean (machine-checked, zero axioms).
 *)

EXTENDS TLC

CONSTANTS Variant   \* "guarded" | "unconditional"

VARIABLES
    pstate,    \* "ACTIVE" | "ACTIVE_MONITORING" | "COMPRESSING"
    prot,      \* "RW" | "NONE"  — kernel mprotect bits for the page
    backed,    \* BOOLEAN — physical backing present (never dropped while non-COMPRESSED)
    stcap,     \* captured state at compressPage's eligibility check (line 1803)
    epc,       \* escalator PC: "EIdle" | "EArmed"
    wpc,       \* worker    PC: "WIdle" | "WCaptured" | "WDone"
    badRead,   \* ghost: worker read a PROT_NONE page (the SIGSEGV)
    staleRead  \* ghost: worker read an UNBACKED page (would be silent corruption)

vars == << pstate, prot, backed, stcap, epc, wpc, badRead, staleRead >>

Init ==
    /\ pstate    = "ACTIVE"
    /\ prot      = "RW"
    /\ backed    = TRUE
    /\ stcap     = "NONE_YET"
    /\ epc       = "EIdle"
    /\ wpc       = "WIdle"
    /\ badRead   = FALSE
    /\ staleRead = FALSE

\* --- Monitoring escalator (Phase 3 + escalateToDeepMonitoring) --------------

\* Phase 3 CAS ACTIVE -> ACTIVE_MONITORING is folded into the arm: the escalator
\* arms PROT_NONE for pre-compression READ detection. Crucially the page stays
\* physically BACKED (mprotect only; no madvise) and the *state* it leaves behind
\* is ACTIVE (models the observed desync: a concurrent handleFault / phase
\* transition leaves state ACTIVE while the escalation's PROT_NONE lingers).
Escalate ==
    /\ epc = "EIdle"
    /\ pstate \in {"ACTIVE", "ACTIVE_MONITORING"}
    /\ prot'   = "NONE"          \* PROT_NONE armed, page still backed
    /\ pstate' = "ACTIVE"        \* desync: state reads ACTIVE, prot is NONE
    /\ epc'    = "EArmed"
    /\ UNCHANGED << backed, stcap, wpc, badRead, staleRead >>

\* --- Compressor worker (compressPage, deferred branch) ----------------------

\* compressPage eligibility check: capture the current page state (line 1803).
WorkerCapture ==
    /\ wpc = "WIdle"
    /\ pstate \in {"ACTIVE", "ACTIVE_MONITORING"}
    /\ stcap'  = pstate
    /\ pstate' = "COMPRESSING"
    /\ wpc'    = "WCaptured"
    /\ UNCHANGED << prot, backed, epc, badRead, staleRead >>

\* The deferred-branch restore + snapshot read.
\*   guarded       : restore PROT_RW only if captured state was ACTIVE_MONITORING.
\*   unconditional : always restore PROT_RW before the read (the fix).
\* Then memcpy(buf, page_addr): faults iff prot = NONE (badRead); reads garbage
\* iff ~backed (staleRead). We compute the post-restore prot, then read under it.
WorkerSnapshot ==
    /\ wpc = "WCaptured"
    /\ LET postProt ==
             IF Variant = "unconditional"
               THEN "RW"                                    \* always restore
               ELSE IF stcap = "ACTIVE_MONITORING"
                      THEN "RW"                             \* guarded restore
                      ELSE prot                             \* leave as-is (BUG)
       IN /\ prot'      = postProt
          /\ badRead'   = (badRead   \/ postProt = "NONE")
          /\ staleRead' = (staleRead \/ ~backed)
    /\ wpc' = "WDone"
    /\ UNCHANGED << pstate, backed, stcap, epc >>

Next ==
    \/ Escalate
    \/ WorkerCapture
    \/ WorkerSnapshot
    \* Stutter when both actors are done so TLC has a terminal self-loop.
    \/ (epc = "EArmed" /\ wpc = "WDone" /\ UNCHANGED vars)
    \/ (epc = "EIdle"  /\ wpc = "WDone" /\ UNCHANGED vars)

Spec == Init /\ [][Next]_vars

\* --- Safety -----------------------------------------------------------------

\* The worker never read a PROT_NONE page (the crash).
NoProtNoneRead == badRead = FALSE

\* The worker never read an UNBACKED page (would be silent corruption — this must
\* hold in BOTH variants: the fix restores PROT_RW on a still-backed page, so it
\* recovers live data, it does not read decommitted zeros).
NoStaleRead == staleRead = FALSE

TypeOK ==
    /\ pstate \in {"ACTIVE", "ACTIVE_MONITORING", "COMPRESSING"}
    /\ prot   \in {"RW", "NONE"}
    /\ backed \in BOOLEAN
    /\ stcap  \in {"NONE_YET", "ACTIVE", "ACTIVE_MONITORING"}
    /\ epc    \in {"EIdle", "EArmed"}
    /\ wpc    \in {"WIdle", "WCaptured", "WDone"}
    /\ badRead   \in BOOLEAN
    /\ staleRead \in BOOLEAN

=============================================================================
