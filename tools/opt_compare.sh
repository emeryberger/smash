#!/usr/bin/env bash
# Compare optimization stacking on neuron-cc full-mode. Robust version:
# - clean wall-time around the compile only
# - RSS sampler is a plain background while-loop keyed on the python PID
#   (no flag file; dies when the watched PID exits) so it can never hang the
#   harness.
#   E: profile + thrash-backoff ON (default)      — the new optimization
#   G: profile + thrash-backoff OFF (control)      — isolate the backoff effect
#   (GC-off dropped: A/B/C/D already showed GC-disable is net-negative.)
set -uo pipefail
NCC=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler
SMASH=$NCC/smash/build_fix/libsmash.so
PYTHON=/opt/venvs/3.13/bin/python3
HLO=$NCC/test7_fsdp_c474_b8_63l_135416_def_modular.hlo
PROFILE=${PROFILE:-$(ls -t $NCC/smash/build_fix/profgen_*/profile.bin 2>/dev/null | head -1)}
OUT=$NCC/smash/build_fix/opt_$(date +%Y%m%d_%H%M%S)
mkdir -p "$OUT"
echo "profile: $PROFILE"
echo "label,status,wall_s,peak_mb,avg_mb,compress,decompress,churn_pct" > "$OUT/summary.csv"

tree_rss(){
  local watch=${1:-} total=0 stack="${1:-}" seen=""
  [ -z "$watch" ] && { echo 0; return; }
  while [ -n "$stack" ]; do
    local cur=${stack%% *}; stack=${stack#"$cur"}; stack=${stack# }
    seen="$seen $cur"
    local kids; kids=$(pgrep -P "$cur" 2>/dev/null||true); stack="$stack $kids"
  done
  for x in $seen; do
    local v; v=$(awk '/^VmRSS:/{print $2}' "/proc/$x/status" 2>/dev/null||echo 0); total=$((total+v))
  done
  echo "$total"
}

run(){
  local label="$1"; shift
  local log="$OUT/${label}.log" rss="$OUT/${label}.rss.csv" neff="$OUT/${label}.neff"
  echo "=== $label ===  extra: $*"
  ( export PYTHONPATH="$NCC:$NCC/build/python/3.13/install" PYTHONMALLOC=malloc \
      LD_PRELOAD="$SMASH" SMASH_STATS=1 SMASH_BUCKET_STATS=1 \
      SMASH_VM_GIB=48 SMASH_DEFER_MADVISE=1
    for kv in "$@"; do export "$kv"; done
    exec "$PYTHON" -m neuronxcc.driver.CommandDriver compile --framework XLA "$HLO" \
      --target trn2 --output "$neff" > "$log" 2>&1 ) &
  local pid=$!
  # Sampler keyed directly on the compile PID; exits when it does.
  ( echo "s,kb"; while kill -0 "$pid" 2>/dev/null; do echo "$(date +%s),$(tree_rss "$pid")"; sleep 0.5; done ) > "$rss" &
  local sp=$!
  local st=$(date +%s)
  wait "$pid"; local rc=$?
  local wall=$(($(date +%s)-st))
  kill "$sp" 2>/dev/null||true; wait "$sp" 2>/dev/null||true
  local status=FAIL; [ -f "$neff" ] && status=PASS
  local peak=$(awk -F, 'NR>1&&$2>0{if($2>m)m=$2}END{print int(m/1024)}' "$rss")
  local avg=$(awk -F, 'NR>1&&$2>0{s+=$2;n++}END{if(n>0)print int(s/n/1024);else print 0}' "$rss")
  local cd=$(awk '/\[smash bucket\]/ && $3 ~ /^[0-9]+$/ {c+=$(NF-2);d+=$(NF-1)} END{printf "%d %d %.1f",c,d,(c>0?d*100.0/c:0)}' "$log")
  local comp=$(echo "$cd"|awk '{print $1}'); local dec=$(echo "$cd"|awk '{print $2}'); local cp=$(echo "$cd"|awk '{print $3}')
  echo "  $label: status=$status rc=$rc wall=${wall}s peak=${peak}MB avg=${avg}MB compress=$comp decompress=$dec churn=${cp}%"
  echo "$label,$status,$wall,$peak,$avg,$comp,$dec,$cp" >> "$OUT/summary.csv"
}

run E_thrash_on          SMASH_COLD_TIMEOUT_SEC=10 SMASH_PROFILE_FILE="$PROFILE"
run G_thrash_off_control SMASH_COLD_TIMEOUT_SEC=10 SMASH_PROFILE_FILE="$PROFILE" SMASH_PROFILE_THRASH_BACKOFF=0

echo ""; echo "=== SUMMARY ==="; column -t -s, "$OUT/summary.csv"
echo "OUT: $OUT"
