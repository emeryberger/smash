#!/usr/bin/env bash
# A/B: MADV_FREE (lazy, fewer IPIs) vs MADV_DONTNEED (eager, more RSS reclaim)
# on neuron-cc full-mode. Measures wall + correct whole-tree RSS. 2 runs each.
set -uo pipefail
NCC=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler
SMASH=$NCC/smash/build_fix/libsmash.so
PYTHON=/opt/venvs/3.13/bin/python3
HLO=$NCC/test7_fsdp_c474_b8_63l_135416_def_modular.hlo
PROFILE=${PROFILE:-$(ls -t $NCC/smash/build_fix/profgen_*/profile.bin 2>/dev/null|head -1)}
OUT=$NCC/smash/build_fix/madv_$(date +%Y%m%d_%H%M%S)
mkdir -p "$OUT"
export PYTHONPATH="$NCC:$NCC/build/python/3.13/install"
echo "profile: $PROFILE"
echo "label,status,wall_s,peak_mb,avg_mb" > "$OUT/summary.csv"
get_tree_rss(){ local r=$1 t=0; local p=$(pgrep -P "$r" 2>/dev/null||true); p="$r $p"
  for x in $p; do local c=$(pgrep -P "$x" 2>/dev/null||true); p="$p $c"; done
  for x in $p; do local v=$(awk '/^VmRSS:/{print $2}' /proc/$x/status 2>/dev/null||echo 0); t=$((t+v)); done; echo $t; }
sample(){ local pid=$1 of=$2 n=0; echo "s,kb">"$of"; while kill -0 "$pid" 2>/dev/null; do echo "$n,$(get_tree_rss $pid)">>"$of"; n=$((n+1)); sleep 0.5; done; }
run(){ local label="$1"; shift
  local log="$OUT/$label.log" rss="$OUT/$label.rss.csv" neff="$OUT/$label.neff"
  echo "=== $label === $*"
  export PYTHONMALLOC=malloc LD_PRELOAD="$SMASH" SMASH_VM_GIB=48 SMASH_COLD_TIMEOUT_SEC=10 SMASH_DEFER_MADVISE=1 SMASH_PROFILE_FILE="$PROFILE"
  for kv in "$@"; do export "$kv"; done
  local st=$(date +%s)
  "$PYTHON" -m neuronxcc.driver.CommandDriver compile --framework XLA "$HLO" --target trn2 --output "$neff" > "$log" 2>&1 &
  local pid=$!; sample $pid "$rss" & local sp=$!
  wait $pid; kill $sp 2>/dev/null||true; wait $sp 2>/dev/null||true
  local wall=$(($(date +%s)-st)) status=FAIL; [ -f "$neff" ] && status=PASS
  local peak=$(awk -F, 'NR>1&&$2>0{if($2>m)m=$2}END{print int(m/1024)}' "$rss")
  local avg=$(awk -F, 'NR>1&&$2>0{s+=$2;n++}END{if(n>0)print int(s/n/1024);else print 0}' "$rss")
  echo "  $label: $status wall=${wall}s peak=${peak}MB avg=${avg}MB"
  echo "$label,$status,$wall,$peak,$avg" >> "$OUT/summary.csv"
  for kv in "$@"; do unset "${kv%%=*}" 2>/dev/null||true; done; unset LD_PRELOAD
}
run dontneed_1
run madvfree_1 SMASH_MADV_FREE=1
run dontneed_2
run madvfree_2 SMASH_MADV_FREE=1
echo "=== SUMMARY ==="; column -t -s, "$OUT/summary.csv"; echo "OUT: $OUT"
