#!/usr/bin/env bash
# Instrument one full-mode smash compile on neuron-cc: perf system profile +
# smash stats + per-bucket churn. Produces:
#   - perf.data (system-wide, then perf report --stdio to perf_report.txt)
#   - <label>.log : compiler output + [smash debug]/[smash stats]/[smash bucket]
#   - <label>.rss.csv : RSS-over-time across process tree
set -uo pipefail

NCC=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler
SMASH=$NCC/smash/build_fix/libsmash.so
PYTHON=/opt/venvs/3.13/bin/python3
HLO=$NCC/test7_fsdp_c474_b8_63l_135416_def_modular.hlo
LABEL=${1:-smash_profiled}
EXTRA_ENV=${2:-"SMASH_VM_GIB=48 SMASH_COLD_TIMEOUT_SEC=10 SMASH_DEFER_MADVISE=1"}
OUT=$NCC/smash/build_fix/prof_$(date +%Y%m%d_%H%M%S)

mkdir -p "$OUT"
export PYTHONPATH="$NCC:$NCC/build/python/3.13/install"

get_tree_rss() {
  local root_pid=$1 total=0
  local pids=$(pgrep -P "$root_pid" 2>/dev/null || true)
  pids="$root_pid $pids"
  for pid in $pids; do
    local children=$(pgrep -P "$pid" 2>/dev/null || true); pids="$pids $children"
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
    echo "$n,$(get_tree_rss "$pid")" >> "$outfile"; n=$((n + 1)); sleep 0.5
  done
}

logfile="$OUT/${LABEL}.log"; rsslog="$OUT/${LABEL}.rss.csv"; outneff="$OUT/${LABEL}.neff"
echo "=== profiling $LABEL ===  env: $EXTRA_ENV"
export PYTHONMALLOC=malloc
export LD_PRELOAD="$SMASH"
export SMASH_STATS=1 SMASH_BUCKET_STATS=1 SMASH_DEBUG=1
for var in $EXTRA_ENV; do export "$var"; done

start=$(date +%s)
# System-wide perf record at modest frequency to keep overhead low (~2-3%).
perf record -F 199 -a -g --call-graph dwarf -o "$OUT/perf.data" -- \
  "$PYTHON" -m neuronxcc.driver.CommandDriver compile \
    --framework XLA "$HLO" --target trn2 --output "$outneff" > "$logfile" 2>&1 &
perfpid=$!
# RSS sampler tracks the python child (perf is parent). Find the python pid.
sleep 3
pypid=$(pgrep -f "CommandDriver compile" | head -1)
[ -n "$pypid" ] && sample_rss "$pypid" "$rsslog" & samplerpid=$!
wait $perfpid; rc=$?
kill $samplerpid 2>/dev/null || true; wait $samplerpid 2>/dev/null || true
end=$(date +%s); wall=$((end-start))

status="FAIL"; [ -f "$outneff" ] && status="PASS"
peak_kb=$(awk -F, 'NR>1 && $2>0 {if($2>max)max=$2} END{print max+0}' "$rsslog" 2>/dev/null)
avg_kb=$(awk -F, 'NR>1 && $2>0 {sum+=$2;n++} END{if(n>0)print int(sum/n); else print 0}' "$rsslog" 2>/dev/null)
echo "status=$status rc=$rc wall=${wall}s peak=$((peak_kb/1024))MB avg=$((avg_kb/1024))MB"

# Post-process perf into a text report (top symbols across the whole tree).
echo "=== generating perf report ==="
perf report -i "$OUT/perf.data" --stdio --percent-limit 0.5 2>/dev/null \
  | head -120 > "$OUT/perf_report.txt"
# Also a flat per-symbol summary.
perf report -i "$OUT/perf.data" --stdio -g none --percent-limit 0.3 2>/dev/null \
  | grep -vE '^#|^$' | head -60 > "$OUT/perf_flat.txt"

echo ""
echo "=== smash churn (per-bucket, from log) ==="
grep "\[smash bucket\]" "$logfile" | tail -40 || echo "(no bucket stats)"
echo ""
echo "=== smash page-state summaries (last per pid) ==="
grep "\[smash stats\]" "$logfile" | tail -10 || echo "(no stats)"
echo ""
echo "Output dir: $OUT"
