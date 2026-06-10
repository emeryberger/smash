#!/usr/bin/env bash
# Capture smash stats + per-bucket churn (no perf) for one full-mode compile.
set -uo pipefail
NCC=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler
SMASH=$NCC/smash/build_fix/libsmash.so
PYTHON=/opt/venvs/3.13/bin/python3
HLO=$NCC/test7_fsdp_c474_b8_63l_135416_def_modular.hlo
LABEL=${1:-smash_stats}
EXTRA_ENV=${2:-"SMASH_VM_GIB=48 SMASH_COLD_TIMEOUT_SEC=10 SMASH_DEFER_MADVISE=1"}
OUT=$NCC/smash/build_fix/stats_$(date +%Y%m%d_%H%M%S)
mkdir -p "$OUT"
export PYTHONPATH="$NCC:$NCC/build/python/3.13/install"

get_tree_rss() {
  local root_pid=$1 total=0
  local pids=$(pgrep -P "$root_pid" 2>/dev/null || true); pids="$root_pid $pids"
  for pid in $pids; do local c=$(pgrep -P "$pid" 2>/dev/null||true); pids="$pids $c"; done
  for pid in $pids; do local r=$(awk '/^VmRSS:/{print $2}' /proc/$pid/status 2>/dev/null||echo 0); total=$((total+r)); done
  echo $total
}
sample_rss() { local pid=$1 of=$2 n=0; echo "sample,rss_kb">"$of"
  while kill -0 "$pid" 2>/dev/null; do echo "$n,$(get_tree_rss "$pid")">>"$of"; n=$((n+1)); sleep 0.5; done; }

logfile="$OUT/${LABEL}.log"; rsslog="$OUT/${LABEL}.rss.csv"; outneff="$OUT/${LABEL}.neff"
echo "=== $LABEL ===  env: $EXTRA_ENV"
export PYTHONMALLOC=malloc LD_PRELOAD="$SMASH" SMASH_STATS=1 SMASH_BUCKET_STATS=1
for var in $EXTRA_ENV; do export "$var"; done
start=$(date +%s)
"$PYTHON" -m neuronxcc.driver.CommandDriver compile --framework XLA "$HLO" \
  --target trn2 --output "$outneff" > "$logfile" 2>&1 &
pid=$!; sample_rss $pid "$rsslog" & sp=$!
wait $pid; rc=$?; kill $sp 2>/dev/null||true; wait $sp 2>/dev/null||true
wall=$(($(date +%s)-start))
status="FAIL"; [ -f "$outneff" ] && status="PASS"
peak=$(awk -F, 'NR>1&&$2>0{if($2>m)m=$2}END{print int(m/1024)}' "$rsslog")
avg=$(awk -F, 'NR>1&&$2>0{s+=$2;n++}END{if(n>0)print int(s/n/1024);else print 0}' "$rsslog")
echo "RESULT: $LABEL status=$status rc=$rc wall=${wall}s peak=${peak}MB avg=${avg}MB"
echo "$LABEL,$status,$rc,$wall,$peak,$avg" >> "$OUT/../stats_summary.csv"

echo ""; echo "=== CHURN: aggregate compress/decompress across all pids ==="
# Sum the per-bucket compress/decompress over every process's dump.
awk '/\[smash bucket\]/ && $3 ~ /^[0-9]+$/ {comp+=$(NF-2); dec+=$(NF-1)}
     END {printf "total_compress=%d total_decompress=%d churn_ratio=%.1f%%\n", comp, dec, (comp>0?dec*100.0/comp:0)}' "$logfile"
echo ""; echo "=== top buckets by samples (merged tail) ==="
grep "\[smash bucket\]" "$logfile" | grep -vE "arena  sc" | sort -k5 -rn | head -25
echo ""; echo "=== page-state summary (one per pid, last) ==="
grep "\[smash stats\]" "$logfile" | tail -12
echo "OUT: $OUT"
