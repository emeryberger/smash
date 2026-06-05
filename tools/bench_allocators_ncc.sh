#!/usr/bin/env bash
# Benchmark different allocators on neuron-cc test7_full compilation
# Measures total RSS across all descendant processes
set -euo pipefail

NCC=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler
ALLOC_DIR=$NCC/smash/build/allocators
PYTHON=/opt/venvs/3.13/bin/python3
HLO=$NCC/test7_fsdp_c474_b8_63l_135416_def_modular.hlo
OUT=$NCC/smash/build/allocator_bench_$(date +%Y%m%d_%H%M%S)

mkdir -p "$OUT"

export PYTHONPATH="$NCC:$NCC/build/python/3.13/install"

# Get total RSS of a process and all its descendants (in KB)
get_tree_rss() {
  local root_pid=$1
  local total=0
  # Get all descendant PIDs using pgrep
  local pids=$(pgrep -P "$root_pid" 2>/dev/null || true)
  pids="$root_pid $pids"
  for pid in $pids; do
    # Recursively get children
    local children=$(pgrep -P "$pid" 2>/dev/null || true)
    pids="$pids $children"
  done
  # Sum RSS from /proc/*/status
  for pid in $pids; do
    local rss=$(awk '/^VmRSS:/{print $2}' /proc/$pid/status 2>/dev/null || echo 0)
    total=$((total + rss))
  done
  echo $total
}

# RSS sampler - samples every 0.5s, writes CSV
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

  rm -f "$outneff"

  local start_time=$(date +%s)

  # Build env string
  local env_cmd="env PYTHONMALLOC=malloc"
  if [ -n "$preload" ]; then
    env_cmd="$env_cmd LD_PRELOAD=$preload"
  fi
  if [ -n "$extra_env" ]; then
    env_cmd="$env_cmd $extra_env"
  fi

  # Start compilation in background
  $env_cmd "$PYTHON" -m neuronxcc.driver.CommandDriver compile \
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

  echo "  rc=$rc  wall=${wall_time}s  peak=${peak_mb}MB  avg=${avg_mb}MB"
  echo "$label,$rc,$wall_time,$peak_mb,$avg_mb" >> "$OUT/summary.csv"

  return $rc
}

echo "label,rc,wall_s,peak_mb,avg_mb" > "$OUT/summary.csv"

# Baseline: system malloc (glibc)
run_compile "glibc" "" ""

# jemalloc
if [ -f "$ALLOC_DIR/lib/libjemalloc.so" ]; then
  run_compile "jemalloc" "$ALLOC_DIR/lib/libjemalloc.so" ""
fi

# mimalloc
if [ -f "$ALLOC_DIR/lib64/libmimalloc.so" ]; then
  run_compile "mimalloc" "$ALLOC_DIR/lib64/libmimalloc.so" ""
fi

echo ""
echo "=========================================="
echo "=== Summary ==="
echo "=========================================="
cat "$OUT/summary.csv" | column -t -s,

echo ""
echo "Results in: $OUT"
