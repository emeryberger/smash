#!/usr/bin/env bash
# Final benchmark with CORRECT whole-tree RSS (reuses the bench_fix_profile.sh
# sampler that produced valid 17.6GB numbers). Measures the stacked wins:
# profile-merge fix + P2_CHUNK default-on, across chunk sizes, vs baselines.
set -uo pipefail
NCC=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler
SMASH=$NCC/smash/build_fix/libsmash.so
JEMALLOC=$NCC/smash/build/allocators/lib/libjemalloc.so
PYTHON=/opt/venvs/3.13/bin/python3
HLO=$NCC/test7_fsdp_c474_b8_63l_135416_def_modular.hlo
PROFILE=${PROFILE:-$(ls -t $NCC/smash/build_fix/profgen_*/profile.bin 2>/dev/null | head -1)}
OUT=$NCC/smash/build_fix/final_$(date +%Y%m%d_%H%M%S)
mkdir -p "$OUT"
export PYTHONPATH="$NCC:$NCC/build/python/3.13/install"
echo "profile: $PROFILE"
echo "label,status,wall_s,peak_mb,avg_mb" > "$OUT/summary.csv"

get_tree_rss() {
  local root_pid=$1 total=0
  local pids=$(pgrep -P "$root_pid" 2>/dev/null || true); pids="$root_pid $pids"
  for pid in $pids; do local c=$(pgrep -P "$pid" 2>/dev/null || true); pids="$pids $c"; done
  for pid in $pids; do local rss=$(awk '/^VmRSS:/{print $2}' /proc/$pid/status 2>/dev/null || echo 0); total=$((total + rss)); done
  echo $total
}
sample_rss() { local pid=$1 outfile=$2 n=0; echo "sample,rss_kb" > "$outfile"
  while kill -0 "$pid" 2>/dev/null; do echo "$n,$(get_tree_rss "$pid")" >> "$outfile"; n=$((n+1)); sleep 0.5; done; }

run_compile() {
  local label="$1" preload="${2:-}" extra_env="${3:-}"
  local outneff="$OUT/${label}.neff" logfile="$OUT/${label}.log" rsslog="$OUT/${label}.rss.csv"
  echo "=== $label ===  env: $extra_env"
  rm -f "$outneff"
  export PYTHONMALLOC=malloc
  if [ -n "$preload" ]; then export LD_PRELOAD="$preload"; else unset LD_PRELOAD 2>/dev/null||true; fi
  if [ -n "$extra_env" ]; then for var in $extra_env; do export "$var"; done; fi
  local start=$(date +%s)
  "$PYTHON" -m neuronxcc.driver.CommandDriver compile --framework XLA "$HLO" \
    --target trn2 --output "$outneff" > "$logfile" 2>&1 &
  local pid=$!
  sample_rss $pid "$rsslog" & local sampler=$!
  wait $pid; kill $sampler 2>/dev/null||true; wait $sampler 2>/dev/null||true
  local wall=$(($(date +%s)-start))
  local peak=$(awk -F, 'NR>1&&$2>0{if($2>m)m=$2}END{print int(m/1024)}' "$rsslog")
  local avg=$(awk -F, 'NR>1&&$2>0{s+=$2;n++}END{if(n>0)print int(s/n/1024);else print 0}' "$rsslog")
  local status=FAIL; [ -f "$outneff" ] && status=PASS
  echo "  $label: status=$status wall=${wall}s peak=${peak}MB avg=${avg}MB"
  echo "$label,$status,$wall,$peak,$avg" >> "$OUT/summary.csv"
  unset LD_PRELOAD 2>/dev/null||true
  if [ -n "$extra_env" ]; then for var in $extra_env; do unset "${var%%=*}" 2>/dev/null||true; done; fi
}

S="SMASH_VM_GIB=48 SMASH_COLD_TIMEOUT_SEC=10 SMASH_DEFER_MADVISE=1 SMASH_PROFILE_FILE=$PROFILE"
run_compile jemalloc "$JEMALLOC" ""
run_compile smash_chunk16 "$SMASH" "$S SMASH_PROTECT_CHUNK_PAGES=16"
run_compile smash_chunk64 "$SMASH" "$S SMASH_PROTECT_CHUNK_PAGES=64"
run_compile smash_chunk128 "$SMASH" "$S SMASH_PROTECT_CHUNK_PAGES=128"

echo ""; echo "=== SUMMARY ==="; column -t -s, "$OUT/summary.csv"
echo "OUT: $OUT"
