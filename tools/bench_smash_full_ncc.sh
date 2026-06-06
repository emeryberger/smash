#!/usr/bin/env bash
# Benchmark smash FULL MODE on neuron-cc test7_full compilation
# Full mode: smash manages ALL allocations (slab + large)
set -euo pipefail

NCC=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler
SMASH=$NCC/smash/build/libsmash.so
PYTHON=/opt/venvs/3.13/bin/python3
HLO=$NCC/test7_fsdp_c474_b8_63l_135416_def_modular.hlo
OUT=$NCC/smash/build/smash_full_bench_$(date +%Y%m%d_%H%M%S)

mkdir -p "$OUT"

export PYTHONPATH="$NCC:$NCC/build/python/3.13/install"

# Get total RSS of a process and all its descendants (in KB)
get_tree_rss() {
  local root_pid=$1
  local total=0
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

# RSS sampler
sample_rss() {
  local pid=$1
  local outfile=$2
  echo "sample,rss_kb" > "$outfile"
  local n=0
  while kill -0 "$pid" 2>/dev/null; do
    local rss=$(get_tree_rss "$pid")
    echo "$n,$rss" >> "$outfile"
    n=$((n + 1))
    sleep 0.5
  done
}

run_compile() {
  local label="$1"
  local preload="${2:-}"
  local extra_env="${3:-}"

  local outneff="$OUT/${label}.neff"
  local logfile="$OUT/${label}.log"
  local rsslog="$OUT/${label}.rss.csv"

  echo ""
  echo "=========================================="
  echo "=== $label ==="
  echo "=========================================="
  echo "Extra env: $extra_env"

  rm -f "$outneff"

  local start_time=$(date +%s)

  # Set up environment
  export PYTHONMALLOC=malloc
  if [ -n "$preload" ]; then
    export LD_PRELOAD="$preload"
  else
    unset LD_PRELOAD 2>/dev/null || true
  fi

  # Parse extra_env and export each var
  if [ -n "$extra_env" ]; then
    for var in $extra_env; do
      export "$var"
    done
  fi

  # Start compilation in background
  "$PYTHON" -m neuronxcc.driver.CommandDriver compile \
    --framework XLA "$HLO" --target trn2 --output "$outneff" \
    > "$logfile" 2>&1 &
  local pid=$!

  # Start RSS sampling
  sample_rss $pid "$rsslog" &
  local sampler_pid=$!

  # Wait for compilation
  wait $pid
  local rc=$?

  # Stop sampler
  kill $sampler_pid 2>/dev/null || true
  wait $sampler_pid 2>/dev/null || true

  local end_time=$(date +%s)
  local wall_time=$((end_time - start_time))

  # Compute stats
  local peak_kb=$(awk -F, 'NR>1 && $2>0 {if($2>max)max=$2} END{print max+0}' "$rsslog")
  local avg_kb=$(awk -F, 'NR>1 && $2>0 {sum+=$2; n++} END{if(n>0)print int(sum/n); else print 0}' "$rsslog")
  local peak_mb=$((peak_kb / 1024))
  local avg_mb=$((avg_kb / 1024))

  # Check if neff was created and log has "Compiler status PASS"
  local status="FAIL"
  if [ -f "$outneff" ] && grep -q "Compiler status PASS" "$logfile" 2>/dev/null; then
    status="PASS"
  fi

  echo "  status=$status rc=$rc  wall=${wall_time}s  peak=${peak_mb}MB  avg=${avg_mb}MB"
  echo "$label,$status,$rc,$wall_time,$peak_mb,$avg_mb" >> "$OUT/summary.csv"

  # Show error if failed
  if [ "$status" = "FAIL" ]; then
    echo "  Last 10 lines of log:"
    tail -10 "$logfile"
  fi

  # Clean up exports
  unset LD_PRELOAD 2>/dev/null || true
  if [ -n "$extra_env" ]; then
    for var in $extra_env; do
      local name="${var%%=*}"
      unset "$name" 2>/dev/null || true
    done
  fi

  return 0
}

echo "label,status,rc,wall_s,peak_mb,avg_mb" > "$OUT/summary.csv"

# Smash FULL mode tests
echo ""
echo "=== Smash FULL mode tests ==="
echo "libsmash.so: $SMASH"
echo "RTLD_DEEPBIND: disabled by default in JobRegistry.py"

# Test 1: Full mode with 48 GiB VM region
run_compile "smash_full_48g_10s" "$SMASH" \
  "SMASH_VM_GIB=48 SMASH_COLD_TIMEOUT_SEC=10 SMASH_DEFER_MADVISE=1"

# Test 2: Full mode with 48 GiB and 5s cold timeout
run_compile "smash_full_48g_5s" "$SMASH" \
  "SMASH_VM_GIB=48 SMASH_COLD_TIMEOUT_SEC=5 SMASH_DEFER_MADVISE=1"

# Test 3: Full mode with 48 GiB and 1s cold timeout (aggressive)
run_compile "smash_full_48g_1s" "$SMASH" \
  "SMASH_VM_GIB=48 SMASH_COLD_TIMEOUT_SEC=1 SMASH_DEFER_MADVISE=1"

echo ""
echo "=========================================="
echo "=== Summary ==="
echo "=========================================="
cat "$OUT/summary.csv" | column -t -s,

echo ""
echo "Results in: $OUT"
