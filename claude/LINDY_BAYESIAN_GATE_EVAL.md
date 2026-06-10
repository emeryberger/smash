# Evaluating the cold-age compress gate: (a) Lindy-threshold vs (b) full Bayesian

Goal: replace the reactive exponential backoff with a PROACTIVE gate keyed on observed
cold age, so the second run keeps BOTH high RSS reduction AND low cold latency (today it
sacrifices RSS: run2 64%→25% RSS to win 17µs→5µs cold — see memory
`smash-compression-churn-secondrun`). User's Lindy intuition: a page cold for k ticks is
likely to stay cold ~k more (heavy-tailed inter-access = decreasing hazard with age).

## Facts that constrain the design (from the code)
- Tick = `kCompressIntervalMs` = **1000ms**. `cold_count_` is per-page uint8 (0-255 seconds).
  Coarse — the natural Lindy unit is "seconds cold", which is exactly what we have.
- **Per-page state is EXPENSIVE**: each per-page array = kVmMaxPages(16M) × 1B ≈ 16 MB
  (bootstrap). ~8 already exist (compressed_, accessed_, cold_count_, recompress_count_,
  page_tier_, shadow_tick_, deferred_*). Adding per-page floats/histograms is a hard no.
- **Per-bucket state is CHEAP and already persisted**: kBucketTableLen = 64×~40 ≈ 2560
  buckets; the 16-byte `Persist` record already carries thrash_rate, stable_cold, best_tier,
  cost EMAs across runs. This is the natural home for a learned prior.
- The compress/skip decision is per-page in `phase2Range` but the LEARNING signal
  (thrash events) is naturally per-bucket. Fault path (compressor_thread.h:~4170) already
  bumps per-page rc + per-bucket EMA on each COMPRESSED→fault — i.e. we already observe
  "page was compressed then re-touched". What we DON'T currently record: the cold-AGE at
  which a page was compressed when it later thrashed (the key Lindy training signal).

---

## Option (a): Lindy-threshold — per-bucket cold-age multiplier

**Model.** Keep the single scalar decision "compress when cold_count ≥ eff_floor", but make
eff_floor a *learned per-bucket function of demonstrated cold-survival* rather than a
reactive doubling. Lindy: require a page to have been cold for `k` ticks where `k` is set so
that, empirically, pages in this bucket that survived `k` cold ticks rarely get re-touched.

**Mechanism.**
- Per bucket, track the cold-age distribution of pages that thrashed: when a page faults back
  from COMPRESSED, record the cold_count it HAD when compressed (stash that one byte in
  CompressedPageInfo, which we already keep per compressed page — near-zero extra cost).
- Maintain a per-bucket EMA / small quantile estimate of "thrash cold-age" → set
  eff_floor = that quantile × safety. Pages that reach it have outlived the bucket's typical
  re-access window → Lindy-safe to compress.
- Persist the quantile in the Persist record (replaces/augments the blunt thrash_rate mult).

**Cost.** ~1 byte per compressed page (cold-age-at-compress, in the existing
CompressedPageInfo) + a couple of u16 per bucket (EMA/quantile). A few comparisons in
phase2Range. Negligible memory, no per-page float state. ~50-100 LOC.

**Pros.** Cheap; fits the existing scalar-floor gate and persistence exactly; directly
encodes the Lindy insight (decision = observed cold age vs learned bucket survival window);
proactive (acts at compress time on cold-age, not after a thrash). Reuses persisted state →
good second-run behavior by construction. Easy to A/B against current backoff (it IS the same
gate with a smarter floor). Easy to reason about / fail safe (degrades to a fixed floor).

**Cons.** It's a point estimate (one threshold per bucket), not a probability — can't trade
RSS vs latency continuously; the "× safety" factor is a tuning knob. Assumes the bucket is
homogeneous in cold-behavior (mostly true — that's WHY arenas exist, to group same-origin
allocations). A bucket with bimodal cold-ages (some pages truly cold, some hot) gets one
compromise threshold. Doesn't directly use compression ROI / RSS pressure to move the bar.

---

## Option (b): full Bayesian — per-bucket posterior over inter-access time

**Model.** Per bucket, maintain a posterior over the inter-access-time distribution (Lindy ⇒
Pareto/log-normal, decreasing hazard). At decision time compute
`P(touched in next Δ | survived cold k) = hazard(k) · Δ`, then compress iff
`P_touch · thrash_cost < compression_ROI(ratio, cold_size)`. The per-bucket recompress
history updates the posterior (conjugate prior on the Pareto/exponential rate). Cold-age `k`
is the per-page evidence; bucket posterior is the shared prior.

**Mechanism.**
- Conjugate form keeps it tractable: model per-bucket re-access hazard as Exponential(λ) with
  a Gamma(α,β) prior on λ → posterior is Gamma(α+events, β+Σcold-survival-time). Both α,β are
  2 numbers per bucket, persistable. hazard at age k under the (heavy-tailed) marginal is a
  closed form (Pareto/Lomax: P(touch in Δ | age k) = 1−((k+β)/(k+β+Δ))^(α+n)).
- Decision compares that probability × cost against ROI — a real cost/benefit, so it can be
  driven by an explicit RSS-pressure or latency-budget term (continuous RSS↔latency trade).
- Update on every compress outcome (thrashed → +1 event with its cold-survival; stayed cold →
  censored observation extends β). Persist (α,β) per bucket.

**Cost.** 2 floats per bucket (~20 KB total) + a `pow`/division per eligible page per tick in
phase2Range. The per-page decision does a couple of FP ops — on ~thousands of eligible pages
per tick that's microseconds/tick, fine (phase2 already does ROI math per page). Censored-
observation bookkeeping is the fiddly part. ~200-300 LOC + careful numerics (avoid pow on hot
path — precompute per-bucket hazard LUT indexed by cold-age bucket each tick).

**Pros.** Principled and continuous: one knob (a cost/Δ or RSS-pressure term) slides the whole
RSS↔latency frontier — directly addresses the run-2 over-correction (could target "keep 64%
RSS at 8µs" instead of all-or-nothing). Naturally handles censored data (pages that stayed
cold are evidence too — current code ignores this). Posterior variance gives confidence →
explore/exploit for free (don't over-trust a bucket with 3 samples). Carries across runs as a
clean (α,β) sufficient statistic. Theoretically the "right" version of the user's idea.

**Cons.** More LOC, more numerics risk (FP determinism across the multi-worker compressor,
pow on a semi-hot path, censoring bugs). Harder to A/B-debug ("why didn't it compress this
page" requires inspecting a posterior). The Exponential/Pareto assumption is itself a model —
if real inter-access is multi-modal it's mis-specified just like (a), only more expensively.
Bucket homogeneity assumption is the SAME as (a) — Bayes doesn't fix a bad bucketing. Risk of
over-engineering a 1-second-granularity, 0-255-range signal: the posterior precision far
exceeds the resolution of the cold_count input.

---

## Recommendation

**Build (a) first; treat (b) as the upgrade if (a)'s point-estimate proves too blunt.**

Reasoning: (a) captures ~80% of the Lindy insight (proactive cold-age threshold, learned +
persisted per bucket) at ~20% of the cost and risk, and it drops into the existing
scalar-floor gate + Persist record with almost no new machinery. The decisive question — does
acting on *observed cold age* (vs reactive thrash-doubling) let run-2 keep RSS AND cut cold
latency — is answerable with (a). If (a) wins but its single threshold visibly leaves RSS↔
latency on the table (e.g. can't hit "64% RSS + 8µs" because one bucket threshold can't serve
both modes), THEN (b)'s continuous posterior + explicit cost term is justified — and by then
we'll have the per-bucket cold-age histogram (a)'s instrumentation produces, which is exactly
(b)'s training data. So (a) is also the data-collection step for (b); they compose.

Both share the same new signal to capture first: **cold-age-at-compress recorded at thrash
time, aggregated per bucket** (1 byte in CompressedPageInfo + per-bucket aggregate). Land that
instrumentation + (a)'s threshold, A/B on the second-run sqlite test (target: run2 keeps
≈64% RSS while cutting cold p50 toward 5µs — beating today's 25%/5µs). Escalate to (b) only on
evidence (a) is leaving measurable RSS↔latency frontier unclaimed.
