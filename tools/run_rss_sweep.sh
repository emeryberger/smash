#!/usr/bin/env bash
# Sweep neuron-cc full-compile across (HLO size, smash config) and capture
# RSS / wall for each combination. Produces per-(hlo,config) RSS CSV plus a
# summary table.
#
# HLO sizes covered: test7 10pct (1.5 MB), 50pct (4.9 MB), full (9.3 MB).
# Configs:
#   - baseline   : no smash
#   - smash_cold : LARGE_ONLY + soft-dirty + writes profile to disk
#   - smash_warm : LARGE_ONLY + soft-dirty + reads profile from disk
#
# The profile is shared across HLOs deliberately — production usage trains
# once on a representative model and reuses everywhere.
set -euo pipefail

NCC=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler
SMASH=$NCC/smash/build/libsmash.so
SAMPLER=$NCC/smash/tools/rss_sampler.py
PYTHON=/opt/venvs/3.13/bin/python3
OUT=$NCC/smash/build/rss_sweep
PROFILE_FILE=$OUT/smash_profile.bin

declare -A HLOS=(
  [100pct]=$NCC/test7_fsdp_c474_b8_63l_135416_def_modular.hlo
)
# Note: smash_work/hlos/test7_*pct.hlo are partial cuts that fail baseline
# compile (IVRF100 validation error) — they're not valid standalone HLOs.
# If more HLO sizes are needed, generate fresh ones with `cut` from the full
# HLO via neuronxcc/starfish/util/hlo_bugpoint, then validate they compile
# clean before adding them here.

rm -rf "$OUT"
mkdir -p "$OUT"

export PYTHONPATH="$NCC:$NCC/build/python/3.13/install"

run() {
  local hlo_label="$1"; shift
  local cfg="$1"; shift
  local hlo="$1"; shift
  local label="${hlo_label}__${cfg}"
  local outneff="$OUT/${label}.neff"
  local logfile="$OUT/${label}.log"
  local rsslog="$OUT/${label}.rss.csv"

  echo "[$(date +%T)] === $label start (HLO=$(basename $hlo)) ==="
  rm -f "$outneff"
  "$PYTHON" "$SAMPLER" --interval 0.5 --label "$label" --out "$rsslog" -- \
    env "$@" \
    "$PYTHON" -m neuronxcc.driver.CommandDriver compile \
      --framework XLA "$hlo" --target trn2 --output "$outneff" \
    > "$logfile" 2>&1
  local rc=$?
  echo "[$(date +%T)] === $label rc=$rc ==="
}

# For each HLO size: baseline, profiling (cold), profile-consumption (warm).
# Profile file is shared — first cold run that writes it sets the table,
# subsequent warm runs reuse it.
for size in "${!HLOS[@]}"; do
  hlo=${HLOS[$size]}

  run "$size" "baseline" "$hlo" \
    PYTHONMALLOC=malloc

  run "$size" "smash_cold" "$hlo" \
    LD_PRELOAD=$SMASH \
    PYTHONMALLOC=malloc \
    SMASH_LARGE_ONLY=1 \
    SMASH_COLD_TIMEOUT_SEC=10 \
    SMASH_PROFILE_FILE=$PROFILE_FILE \
    SMASH_PROFILE_FILE_RW=1

  run "$size" "smash_warm" "$hlo" \
    LD_PRELOAD=$SMASH \
    PYTHONMALLOC=malloc \
    SMASH_LARGE_ONLY=1 \
    SMASH_COLD_TIMEOUT_SEC=10 \
    SMASH_PROFILE_FILE=$PROFILE_FILE
done

# Summary: per-run peak / avg RSS / wall.
echo ""
echo "=== Summary ==="
printf '%-30s  %10s  %10s  %10s  %10s\n' "label" "peak_MB" "avg_MB" "wall_s" "rc"
for f in "$OUT"/*.rss.csv; do
  awk -F, -v fn="$f" 'NR>1 {if ($3>peak) peak=$3; sum+=$3; n++} END {
    label=fn; sub(/.*\//, "", label); sub(/\.rss\.csv$/, "", label);
    if (n) printf "%-30s  %10d  %10d  %10.1f  %10s\n", label, peak/1024, (sum/n)/1024, $2, "?"
  }' "$f"
done

echo ""
echo "=== Profile file ==="
ls -la "$PROFILE_FILE" 2>&1 || echo "(no profile saved)"
