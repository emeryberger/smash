# Soft-dirty-driven ROI churn avoidance

## The ask
Don't just back off after thrash — make the COMPRESS DECISION churn-aware by
estimating ROI from the soft-dirty write signal: a page whose soft-dirty bit has
stayed unset across many ticks is (a) Lindy-likely to stay cold and (b) provably
write-stable, so its compressed blob won't be invalidated soon. Conversely, defer
pages whose bucket keeps getting re-written.

## What already exists (verified)
- Soft-dirty (Linux, default ON via SMASH_SOFTDIRTY) reads PTE bit 55 per tick
  (readSoftDirty, compressor_thread.h:2653) → sets accessed_[i] → phase1Range
  resets cold_count_[i]=0 on a set bit, else cold_count_[i]++ (line 1011-1027).
  So **cold_count_ IS the consecutive-write-clean-tick counter** under soft-dirty.
- ROI benefit is ALREADY linear in cold_count:
  benefit = kPageSize * (ratio/255) * cold_count   (compression_roi.h:computeROI).
  So longer clean streaks already raise ROI.
- Reactive backoff (recompress_count_ << shift) handles re-dirty AFTER it happens,
  decoupled from ROI. The fault handler bumps recompress_count_ on ANY fault
  (read or write) of a COMPRESSED page (compressor_thread.h:~4170).

## The gap (what's genuinely missing)
1. ROI rewards long clean streaks but never PENALIZES expected re-dirtying — churn
   risk is absent from the cost/benefit. Re-dirty is only handled reactively.
2. cold_count under soft-dirty is WRITE-clean, but the reactive backoff and the
   fault handler treat any ACCESS (read OR write) as a thrash. A read-hot /
   write-cold page (classic thrash victim under the old PROT_READ monitor) is
   correctly clean to soft-dirty but still gets penalized on read faults. The
   write-specific signal is not exploited end-to-end.

## Design: discount ROI benefit by expected blob survival, learned per-bucket
Replace the crude linear-in-cold_count benefit with a residence estimate that
folds in re-dirty probability:

  effective_benefit = kPageSize * (ratio/255) * E[ticks_blob_survives]
  E[ticks_blob_survives] ≈ cold_count * (1 - P_redirty_bucket)

where P_redirty_bucket is a per-(arena,size_class) EMA of "fraction of pages we
compressed in this bucket that were WRITTEN again (soft-dirty set, not merely
read) within the reclaim window." Compress iff discounted ROI >= threshold.

Why this should beat the failed Lindy/Bayes gates (which keyed on access-age):
- **Write-specific**: the observation is "did soft-dirty get set after compress",
  not "did the page fault". Distinguishes re-dirty (blob invalidated, real waste)
  from read-fault (blob still valid after decompress). New signal the cold_count
  age models couldn't see. This is the user's exact point.
- **Continuous ROI modifier, not a binary floor gate**: when P_redirty is
  uninformative/zero the discount → 1.0 and behavior == baseline ROI by
  construction. Can't regress where there's no churn (the s=0.6/1.2 tie cases).
- **Direct measurement, not inferred hazard**: P_redirty is observed, not a
  Pareto/Gamma posterior over coarse 0-255 cold_count. Matches the signal's
  resolution.

## Mechanism (concrete)
1. New observation: when a COMPRESSED page faults back, the fault path already
   knows it's being accessed. To classify WRITE vs READ re-dirty cheaply: on the
   NEXT soft-dirty tick after a page returns to ACTIVE, if its soft-dirty bit is
   set → it was a write re-dirty (count as re-dirty event for the bucket); if it
   went cold again without soft-dirty → it was a read fault (NOT a re-dirty;
   the blob would have survived). Stash a 1-bit "recently decompressed, watching
   for write" per page (reuse a spare bit; or piggyback on existing per-page state).
2. Per-bucket P_redirty EMA (x256 fixed point) in SizeClassStats, updated from
   those classified events. Persist it in the 16-byte Persist record (there are
   spare/repurposable bytes; keep static_assert(==16)). Good second-run behavior.
3. computeROI gains an optional redirty_discount_x256 arg (default 256 = no
   discount, exact current behavior). phase2Range passes the bucket's
   (256 - P_redirty_x256). Benefit scaled by it. shouldCompress threshold
   unchanged → a high-redirty bucket's pages fall below threshold and defer,
   a write-stable bucket's pages clear it and compress.
4. Gate behind SMASH_SOFTDIRTY_ROI (default OFF until measured). When off, the
   discount is hard-1.0 → byte-identical to today.

## Validation plan
- bench_zipf_reaccess is the discriminating workload (cold-age predicts
  re-access). Add a WRITE-vs-READ knob: re-access should READ hot objects and
  only WRITE a subset, so the read-hot/write-cold distinction the soft-dirty
  signal exploits actually exists (current bench writes p[size/2] on every
  access — make the write conditional on a --write-pct).
- A/B baseline-backoff vs softdirty-roi across zipf-s {0.6,0.9,1.2} AND
  write-pct {100, 20}. The win condition: at low write-pct (read-hot/write-cold),
  softdirty-roi keeps RSS reduction HIGH (compresses the write-cold pages the
  reactive backoff wrongly defers) while slow-ops stays low. Interleaved reps
  (slow-ops% is high-variance — learned that the hard way).
- Must hold: 16/16 ctest; compression suite (bench_rss/sqlite) RSS unchanged
  when gate off; no throughput regression.
