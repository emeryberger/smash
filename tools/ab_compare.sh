#!/usr/bin/env bash
# A/B/C/D comparison on neuron-cc full-mode, capturing wall, RSS, and churn.
#   A: no profile               (unguided baseline)
#   B: with populated profile   (does the merged profile suppress churn?)
#   C: with profile + GC off    (does disabling Python cyclic GC cut churn?)
#   D: profile + GC off + longer cold timeout (less aggressive compression)
set -uo pipefail
NCC=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler
SMASH=$NCC/smash/build_fix/libsmash.so
PYTHON=/opt/venvs/3.13/bin/python3
HLO=$NCC/test7_fsdp_c474_b8_63l_135416_def_modular.hlo
GCOFF=$NCC/smash/tools/gcoff
PROFILE=${PROFILE:-$(ls -t $NCC/smash/build_fix/profgen_*/profile.bin 2>/dev/null | head -1)}
OUT=$NCC/smash/build_fix/ab_$(date +%Y%m%d_%H%M%S)
mkdir -p "$OUT"
echo "Using profile: $PROFILE"
echo "label,status,wall_s,peak_mb,avg_mb,compress,decompress,churn_pct" > "$OUT/summary.csv"

get_tree_rss(){ local r=$1 t=0; local p=$(pgrep -P "$r" 2>/dev/null||true); p="$r $p"
  for x in $p; do local c=$(pgrep -P "$x" 2>/dev/null||true); p="$p $c"; done
  for x in $p; do local v=$(awk '/^VmRSS:/{print $2}' /proc/$x/status 2>/dev/null||echo 0); t=$((t+v)); done; echo $t; }
sample(){ local pid=$1 of=$2 n=0; echo "s,kb">"$of"; while kill -0 "$pid" 2>/dev/null; do echo "$n,$(get_tree_rss $pid)">>"$of"; n=$((n+1)); sleep 0.5; done; }

run(){
  local label="$1"; shift
  local pp="$NCC:$NCC/build/python/3.13/install"
  [ "${GC:-on}" = "off" ] && pp="$GCOFF:$pp"
  local log="$OUT/${label}.log" rss="$OUT/${label}.rss.csv" neff="$OUT/${label}.neff"
  echo "=== $label ===  GC=${GC:-on} extra: $*"
  ( export PYTHONPATH="$pp" PYTHONMALLOC=malloc LD_PRELOAD="$SMASH" SMASH_STATS=1 SMASH_BUCKET_STATS=1
    export SMASH_VM_GIB=48 SMASH_DEFER_MADVISE=1
    for kv in "$@"; do export "$kv"; done
    "$PYTHON" -m neuronxcc.driver.CommandDriver compile --framework XLA "$HLO" \
      --target trn2 --output "$neff" > "$log" 2>&1 ) &
  local pid=$!; sample $pid "$rss" & local sp=$!
  local st=$(date +%s); wait $pid; local wall=$(($(date +%s)-st))
  kill $sp 2>/dev/null||true; wait $sp 2>/dev/null||true
  local status=FAIL; [ -f "$neff" ] && status=PASS
  local peak=$(awk -F, 'NR>1&&$2>0{if($2>m)m=$2}END{print int(m/1024)}' "$rss")
  local avg=$(awk -F, 'NR>1&&$2>0{s+=$2;n++}END{if(n>0)print int(s/n/1024);else print 0}' "$rss")
  local churn=$(awk '/\[smash bucket\]/ && $3 ~ /^[0-9]+$/ {c+=$(NF-2);d+=$(NF-1)} END{printf "%d %d %.1f",c,d,(c>0?d*100.0/c:0)}' "$log")
  local comp=$(echo $churn|awk '{print $1}'); local dec=$(echo $churn|awk '{print $2}'); local cp=$(echo $churn|awk '{print $3}')
  echo "  $label: status=$status wall=${wall}s peak=${peak}MB avg=${avg}MB compress=$comp decompress=$dec churn=${cp}%"
  echo "$label,$status,$wall,$peak,$avg,$comp,$dec,$cp" >> "$OUT/summary.csv"
}

GC=on  run A_noprofile         SMASH_COLD_TIMEOUT_SEC=10
GC=on  run B_profile          SMASH_COLD_TIMEOUT_SEC=10 SMASH_PROFILE_FILE="$PROFILE"
GC=off run C_profile_gcoff    SMASH_COLD_TIMEOUT_SEC=10 SMASH_PROFILE_FILE="$PROFILE"
GC=off run D_prof_gcoff_cold20 SMASH_PROFILE_FILE="$PROFILE" SMASH_COLD_TIMEOUT_SEC=20

echo ""; echo "=== SUMMARY ==="; column -t -s, "$OUT/summary.csv"
echo "OUT: $OUT"
