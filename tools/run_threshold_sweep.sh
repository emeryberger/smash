#!/usr/bin/env bash
# Sweep SMASH_LARGE_ONLY_THRESHOLD on neuron-cc test7_full to find whether
# routing more allocations through smash improves the RSS picture.
#
# Configurations:
#   - baseline: no smash
#   - smash_thr_16k: default 16 KiB threshold (the current run)
#   - smash_thr_4k:  4 KiB threshold (catches more of the slab)
#   - smash_thr_1k:  1 KiB threshold (catches almost everything sized)
set -euo pipefail

NCC=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler
SMASH=$NCC/smash/build/libsmash.so
SAMPLER=$NCC/smash/tools/rss_sampler.py
PYTHON=/opt/venvs/3.13/bin/python3
HLO=$NCC/test7_fsdp_c474_b8_63l_135416_def_modular.hlo
OUT=$NCC/smash/build/threshold_sweep
PROFILE=$OUT/smash_profile.bin

rm -rf "$OUT"
mkdir -p "$OUT"

export PYTHONPATH="$NCC:$NCC/build/python/3.13/install"

run() {
  local label="$1"; shift
  local outneff="$OUT/${label}.neff"
  local logfile="$OUT/${label}.log"
  local rsslog="$OUT/${label}.rss.csv"

  echo "[$(date +%T)] === $label start ==="
  rm -f "$outneff"
  "$PYTHON" "$SAMPLER" --interval 0.5 --label "$label" --out "$rsslog" -- \
    env "$@" \
    "$PYTHON" -m neuronxcc.driver.CommandDriver compile \
      --framework XLA "$HLO" --target trn2 --output "$outneff" \
    > "$logfile" 2>&1
  local rc=$?
  echo "[$(date +%T)] === $label rc=$rc ==="
}

# Baseline once.
run "baseline" PYTHONMALLOC=malloc

# Sweep three thresholds. Use the same profile file across runs.
for thr in 16384 4096 1024; do
  label_base="smash_thr_${thr}"
  run "${label_base}_cold" \
    LD_PRELOAD=$SMASH \
    PYTHONMALLOC=malloc \
    SMASH_LARGE_ONLY=1 \
    SMASH_LARGE_ONLY_THRESHOLD=$thr \
    SMASH_COLD_TIMEOUT_SEC=10 \
    SMASH_PROFILE_FILE=$PROFILE \
    SMASH_PROFILE_FILE_RW=1
done

echo ""
echo "=== Summary ==="
printf '%-30s  %10s  %10s  %10s\n' "config" "peak_MB" "avg_MB" "wall_s"
for f in "$OUT"/*.rss.csv; do
  awk -F, -v fn="$f" 'NR>1 {if ($3>peak) peak=$3; sum+=$3; n++} END {
    label=fn; sub(/.*\//, "", label); sub(/\.rss\.csv$/, "", label);
    if (n) printf "%-30s  %10d  %10d  %10.1f\n", label, peak/1024, (sum/n)/1024, $2
  }' "$f"
done

echo ""
echo "=== Profile ==="
ls -la "$PROFILE" 2>&1 || echo "(none)"
