# Plan: Slab-lock contention fixes for high thread counts

Context: Larson @64t collapses (per-arena `Slab::lock_` contention dominated by the
drain/free path — see memory `smash-hoard-bench-cpu`). Two complementary approaches.
Pursue **A first** (smaller, removes the lock from the contended free path regardless
of routing), then **B** (the faithful Hoard per-CPU port). Sequential, not parallel:
both edit `slab.h` + the free/drain path and would conflict + confound measurement.

Baseline to beat (idle box, NO_COMPRESSOR, Larson `3 10 500 1000 10 1 <T>`):
- 8t ~58M, 32t ~160M, 64t ~6-50M (high variance — the cliff). jemalloc 64t ~220M, Hoard ~310M.
Invariants that constrain every design:
- **No metadata in user data pages** (compression). Cannot thread a freelist next-ptr
  through freed objects (Hoard does; smash cannot) → deferred nodes live in BootstrapAlloc.
- **Faithful with compression ON**: deferred objects must stay marked-allocated in the span
  bitmap until applied, so the compressor still treats their pages as live.
- Span descriptors are immortal; page ranges recycle across size classes (see
  `smash-span-lifecycle-invariant`) → a deferred free must be applied to the span that owned
  the ptr AT FREE TIME. Capture the resolved `Span*` in the node; do not re-resolve later.
- `SmashHeap::lock()/unlock()` (atfork) locks every slab; deferred stacks must be safe to
  drain under it (drain-on-lock-acquire covers this).

---

## Approach A — Lock-free deferred-free (Treiber stack) — DO FIRST

Idea: on `ThreadCache::drain`, try the slab lock; if uncontended apply directly (today's
path, unchanged). If contended, push the whole already-resolved batch as ONE node onto a
per-Slab atomic LIFO and return immediately — the freeing thread never blocks. Any thread
that next acquires that slab's lock (a malloc refill, or a drain that won `tryLock`) first
drains the pending stack and applies all deferred frees. Mirrors VmRegion's existing
decommit Treiber stack (`vm_region.h::queueForDecommit`).

Why a stack not a deque (decided): MPSC, drain-ALL, order-independent. Treiber push = 1 CAS;
drain = 1 `exchange(nullptr)`. A deque's two-ended ops buy nothing when the consumer always
takes everything; Chase-Lev is single-producer (wrong shape). No staleness risk — drained in
full on the very next lock acquisition.

Node granularity: ONE node per drain batch (~64-128 ptrs), NOT per ptr → atomic op rate is
~1/batch of the free rate. Node = `{void** ptrs, Span** spans, uint32_t count, Node* next}`
with inline fixed arrays sized to `kThreadCacheMaxPerClass`, allocated from a per-Slab
bootstrap free-pool (recycled like VmRegion's DecommitEntry pool, so steady-state alloc-free).

### Steps
1. Add to `Slab`: `std::atomic<PendingNode*> pending_head_{nullptr}` + a bootstrap node
   free-pool (`pending_pool_` guarded by a tiny pool lock, or itself a Treiber stack).
2. `pushPending(ptrs, spans, count)`: grab/alloc a node, memcpy the resolved ptrs+spans in,
   CAS-push to `pending_head_`. (memcpy is the cost we pay to keep nodes out of user pages.)
3. `drainPending()` (caller holds lock_): `exchange(nullptr)`, walk nodes, `deallocateOne`
   each, recycle nodes. Call at the TOP of `allocate()`, `allocateBatch()`, and the
   lock-winning path of the new drain.
4. Rework `ThreadCache::drain` path: instead of unconditional `deallocateBatchResolved`
   (which always locks), call a new `Slab::deallocateBatchDeferred(ptrs, spans, count)`:
   `if (lock_.tryLock()) { drainPending(); for-each deallocateOne; unlock; }`
   `else { pushPending(...); }`. The counting-sort/grouping in `drainRangeToSlabs` stays
   (it already produces per-arena resolved runs — exactly what a node needs).
5. Flush on teardown: `scavenge()` and `SmashHeap::lock()` drain pending first; thread exit
   `drainAll` must also flush (push-then-someone-drains, or force-lock+drain).
6. Bound unbounded growth: if `pending` depth exceeds K nodes, the next freer force-locks and
   drains (back-pressure) so memory can't blow up under a stuck owner.

### Correctness focus
- ABA on the Treiber stack: nodes come from a pool and could be reused → use a tagged head
  or rely on the fact that only a lock-holder pops (single consumer) so push/pop don't race
  on the same node. Single-consumer (lock-holder) + MP producers = classic safe MPSC.
- Deferred span pointer must be the free-time span (captured in node) — never re-resolve.
- Compression: a deferred ptr is still bitmap-allocated until applied, so the page stays
  live; applying later only flips bitmap bits + list membership — no data-page touch.

### Risks / measure
- memcpy of ptrs+spans per contended batch — should be ≪ the lock-wait it replaces; verify.
- Could regress the UNCONTENDED case if `tryLock`+`drainPending` adds overhead → guard:
  drainPending fast-exits on `pending_head_==nullptr` (one relaxed load).
- Gate behind `SMASH_DEFER_FREE` (default on once validated) for clean A/B.
- Success metric: Larson 64t variance collapses + median up toward the 100M the 128-arena
  experiment showed; NO regression at 1/8/32t or on phong/numa/threadtest/sqlite; 16/16 ctest.

---

## Approach B — True per-core slab lanes (faithful Hoard port) — DO SECOND

Idea: give each (core, size_class) its own slab lane with its OWN lock, so threads on
different cores never share a slab lock. Index lanes by `currentCpu()` (already added:
sched_getcpu / pthread_cpu_number_np). This is what actually delivers Hoard's win — a hash
tweak can't (proven: the SMASH_CPU_ARENA experiment regressed because lanes were still
shared/masked to 64 arenas).

### Design sketch
- Replace/augment the `slabs_[arena * kNumClasses + sc]` array with a per-core dimension:
  `slabs_[core_lane * kNumClasses + sc]`, `core_lane = currentCpu() % kNumCoreLanes`,
  `kNumCoreLanes` ≈ min(ncores, cap). malloc refill AND free/drain both route by current CPU
  → same-core ops hit the same lane (no concurrency on a core → near-zero contention),
  cross-core ops hit different lanes (no shared lock).
- Cross-CPU free problem: a chunk allocated on core X may be freed on core Y. Y's drain must
  return it to *some* lane. Options: (a) return to Y's lane (objects migrate between lanes —
  needs span ownership to be lane-agnostic; span already records arena_id, would record
  lane), or (b) remote-free queue back to X's lane (combine with Approach A's stack, keyed by
  the span's home lane). (b) composes cleanly with A and preserves locality.
- Compression interaction: arena routing drove page homogeneity. Per-core lanes are a FINER
  split (more lanes) → equal-or-better homogeneity per the corrected analysis, but MORE
  partial spans (footprint). Must measure RSS + ratio, not just throughput. This is the real
  cost knob, and why B is bigger/riskier than A.

### Open questions to resolve before building B
- Lane count vs arena count vs compression cohorts — does per-core conflict with the
  existing cold/hot sub-arena (`kColdArenaFeedback`) routing? Likely need lane ⟂ arena
  (two independent axes) or fold them.
- Migration churn: threads migrating cores spread one logical stream across lanes → worse
  homogeneity + more partial spans. May need core→lane stickiness (pin lane on first use).
- Memory blowup: kNumCoreLanes × kNumClasses × (partial span) could be large on 192-core
  hosts. Cap lanes well below ncores (e.g. 16-32) and accept some sharing.

### Success metric for B
- Larson 64t approaching jemalloc (220M) / closing the gap A leaves; held within footprint
  and compression-ratio guardrails (bench_rss / bench_sqlite ratio unchanged). Gate behind a
  knob; compare against A-only and baseline.

---

## Sequencing
1. A: implement, validate (ctest + Larson/numa/phong/threadtest/sqlite A/B), land gated.
2. Re-profile Larson 64t with A on — see how much contention remains and where.
3. B: only if A leaves a meaningful gap; scope by the residual profile. B may reduce to just
   "more/per-core lanes feeding A's deferred-free" rather than a full redesign.

---

## OUTCOME (2026-06-07)

**A landed and SUCCEEDED.** Larson 64t: cliff (28-66M, wild variance) → **222M stable =
jemalloc parity (220M)**. Also +9% @32t, neutral @8t. No regressions (phong/threadtest/numa
neutral; compression bench_rss 64% / sqlite 57% intact; 16/16 ctest + 10/10 contention).
Gated SMASH_DEFER_FREE, default ON.

**B (per-core lanes) NOT pursued — A made it unnecessary.** Step-2 re-profile of Larson 64t
with A on: drainRangeToSlabs (was 85%, the cliff) is GONE from the top; time is now normal
allocator work (mallocSlow ~29%, xxfree ~17%) — no slab-lock contention left to attack. B
targets contention that no longer dominates. Revisit ONLY with new profile evidence
(a different workload showing residual slab-lock contention) or to chase Hoard's 310M, which
is a DIFFERENT gap (superblock allocator design, not lock contention).
