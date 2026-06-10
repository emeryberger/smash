#!/usr/bin/env bash
# Compare neuron-cc full-compile RSS / wall under three configurations:
#   (1) baseline: no smash
#   (2) LARGE_ONLY + soft-dirty default
#   (3) LARGE_ONLY + soft-dirty + warm profile (run #2's profile reused)
#
# Writes a JSON summary + per-run RSS CSV traces under $OUT.
set -euo pipefail

NCC=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler
SMASH=$NCC/smash/build/libsmash.so
HLO=$NCC/test7_fsdp_c474_b8_63l_135416_def_modular.hlo
OUT=$NCC/smash/build/rss_compare
SAMPLER=$NCC/smash/tools/rss_sampler.py
PYTHON=/opt/venvs/3.13/bin/python3
PROFILE_DIR=$OUT/profile

rm -rf "$OUT"
mkdir -p "$OUT" "$PROFILE_DIR"

export PYTHONPATH="$NCC:$NCC/build/python/3.13/install"

run() {
  local label="$1"; shift
  local outneff="$OUT/${label}.neff"
  local logfile="$OUT/${label}.log"
  local rsslog="$OUT/${label}.rss.csv"

  echo "[$(date +%T)] === $label start ==="
  rm -f "$outneff"
  # shellcheck disable=SC2068
  "$PYTHON" "$SAMPLER" --interval 0.5 --label "$label" --out "$rsslog" -- \
    env $@ \
    "$PYTHON" -m neuronxcc.driver.CommandDriver compile \
      --framework XLA "$HLO" --target trn2 --output "$outneff" \
    > "$logfile" 2>&1
  local rc=$?
  local neff_size=0
  [ -f "$outneff" ] && neff_size=$(stat -c%s "$outneff")
  echo "[$(date +%T)] === $label rc=$rc neff_bytes=$neff_size ==="
}

# (1) baseline
run "baseline" \
  PYTHONMALLOC=malloc

# (2) LARGE_ONLY + soft-dirty default + writes profile to PROFILE_DIR
run "smash_large_only" \
  LD_PRELOAD=$SMASH \
  PYTHONMALLOC=malloc \
  SMASH_LARGE_ONLY=1 \
  SMASH_COLD_TIMEOUT_SEC=10 \
  SMASH_PROFILE_DIR=$PROFILE_DIR

# (3) LARGE_ONLY + soft-dirty + reuse profile from (2)
run "smash_large_only_warm" \
  LD_PRELOAD=$SMASH \
  PYTHONMALLOC=malloc \
  SMASH_LARGE_ONLY=1 \
  SMASH_COLD_TIMEOUT_SEC=10 \
  SMASH_PROFILE_DIR=$PROFILE_DIR

echo ""
echo "=== Done. Summary at $OUT ==="
ls -la "$OUT"
