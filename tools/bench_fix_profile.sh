#!/usr/bin/env bash
# Performance benchmark for the TOCTOU-fixed full-mode smash on neuron-cc.
# Phase 0: glibc baseline + jemalloc baseline (reference RSS/time)
# Phase 1: smash full-mode profile-generation run (writes profile)
# Phase 2: smash full-mode using the saved profile
# Measures wall time + peak/avg RSS across the whole process tree.
set -uo pipefail

NCC=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler
SMASH=$NCC/smash/build_fix/libsmash.so
JEMALLOC=$NCC/smash/build/allocators/lib/libjemalloc.so
PYTHON=/opt/venvs/3.13/bin/python3
HLO=$NCC/test7_fsdp_c474_b8_63l_135416_def_modular.hlo
OUT=$NCC/smash/build_fix/perf_$(date +%Y%m%d_%H%M%S)
PROFILE=$OUT/smash_profile.bin

mkdir -p "$OUT"
export PYTHONPATH="$NCC:$NCC/build/python/3.13/install"

get_tree_rss() {
  local root_pid=$1 total=0
  local pids=$(pgrep -P "$root_pid" 2>/dev/null || true)
  pids="$root_pid $pids"
  for pid in $pids; do
    local children=$(pgrep -P "$pid" 2>/dev/null || true)
    pids="$pids $children"
  done
  for pid in $pids; do
    local rss=$(awk '/^VmRSS:/{print $2}' /proc/$pid/status 2>/dev/null || echo 0)
    total=$((total + rss))
  done
  echo $total
}

sample_rss() {
  local pid=$1 outfile=$2 n=0
  echo "sample,rss_kb" > "$outfile"
  while kill -0 "$pid" 2>/dev/null; do
    echo "$n,$(get_tree_rss "$pid")" >> "$outfile"
    n=$((n + 1))
    sleep 0.5
  done
}

run_compile() {
  local label="$1" preload="${2:-}" extra_env="${3:-}"
  local outneff="$OUT/${label}.neff" logfile="$OUT/${label}.log" rsslog="$OUT/${label}.rss.csv"
  echo ""
  echo "=== $label ===  env: $extra_env"
  rm -f "$outneff"
  local start_time=$(date +%s)
  export PYTHONMALLOC=malloc
  if [ -n "$preload" ]; then export LD_PRELOAD="$preload"; else unset LD_PRELOAD 2>/dev/null || true; fi
  for var in $extra_env; do export "$var"; done

  "$PYTHON" -m neuronxcc.driver.CommandDriver compile \
    --framework XLA "$HLO" --target trn2 --output "$outneff" > "$logfile" 2>&1 &
  local pid=$!
  sample_rss $pid "$rsslog" & local sampler_pid=$!
  wait $pid; local rc=$?
  kill $sampler_pid 2>/dev/null || true; wait $sampler_pid 2>/dev/null || true

  local end_time=$(date +%s) wall_time=$(( $(date +%s) - start_time ))
  local peak_kb=$(awk -F, 'NR>1 && $2>0 {if($2>max)max=$2} END{print max+0}' "$rsslog")
  local avg_kb=$(awk -F, 'NR>1 && $2>0 {sum+=$2; n++} END{if(n>0)print int(sum/n); else print 0}' "$rsslog")
  local peak_mb=$((peak_kb / 1024)) avg_mb=$((avg_kb / 1024))
  local status="FAIL"; [ -f "$outneff" ] && status="PASS"
  echo "  status=$status rc=$rc  wall=${wall_time}s  peak=${peak_mb}MB  avg=${avg_mb}MB"
  echo "$label,$status,$rc,$wall_time,$peak_mb,$avg_mb" >> "$OUT/summary.csv"
  [ "$status" = "FAIL" ] && { echo "  last 12 log lines:"; tail -12 "$logfile"; }
  unset LD_PRELOAD 2>/dev/null || true
  for var in $extra_env; do unset "${var%%=*}" 2>/dev/null || true; done
  return 0
}

echo "label,status,rc,wall_s,peak_mb,avg_mb" > "$OUT/summary.csv"
echo "=== Perf bench: TOCTOU-fixed full-mode smash ==="
echo "libsmash: $SMASH (built $(date -r "$SMASH" '+%Y-%m-%d %H:%M' 2>/dev/null))"

# Phase 0: baselines
run_compile "glibc_baseline" "" ""
[ -f "$JEMALLOC" ] && run_compile "jemalloc_baseline" "$JEMALLOC" ""

# Phase 1: smash full-mode profile generation
run_compile "smash_profile_gen" "$SMASH" \
  "SMASH_VM_GIB=48 SMASH_COLD_TIMEOUT_SEC=10 SMASH_DEFER_MADVISE=1 SMASH_PROFILE_FILE=$PROFILE SMASH_PROFILE_FILE_RW=1"

# Phase 2: smash full-mode using the profile
if [ -f "$PROFILE" ]; then
  echo ""; echo "Profile created: $(ls -lh "$PROFILE" | awk '{print $5}')"
  run_compile "smash_with_profile" "$SMASH" \
    "SMASH_VM_GIB=48 SMASH_COLD_TIMEOUT_SEC=10 SMASH_DEFER_MADVISE=1 SMASH_PROFILE_FILE=$PROFILE"
else
  echo "WARNING: profile not created; running second pass without profile"
  run_compile "smash_no_profile_2" "$SMASH" \
    "SMASH_VM_GIB=48 SMASH_COLD_TIMEOUT_SEC=10 SMASH_DEFER_MADVISE=1"
fi

echo ""
echo "=== Summary ==="
cat "$OUT/summary.csv" | column -t -s,
echo ""
echo "Results in: $OUT"
