#!/usr/bin/env bash
# Test with FIXAV mode (strictest ordering)
set -euo pipefail

NCC=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler
SMASH=$NCC/smash/build/libsmash.so
PYTHON=/opt/venvs/3.13/bin/python3
HLO=$NCC/test7_fsdp_c474_b8_63l_135416_def_modular.hlo
OUT=$NCC/smash/build/debug_fixav_$(date +%Y%m%d_%H%M%S)
RUNS=${1:-3}

mkdir -p "$OUT"

export PYTHONPATH="$NCC:$NCC/build/python/3.13/install"

run_compile() {
  local label="$1"
  local run_num="$2"
  local extra_env="$3"
  
  local outneff="$OUT/${label}_run${run_num}.neff"
  local logfile="$OUT/${label}_run${run_num}.log"
  
  echo -n "  Run $run_num: "
  
  rm -f "$outneff"
  
  local start_time=$(date +%s)
  
  export PYTHONMALLOC=malloc
  export LD_PRELOAD="$SMASH"
  
  # Set config vars
  for var in $extra_env; do
    export "$var"
  done
  
  "$PYTHON" -m neuronxcc.driver.CommandDriver compile \
    --framework XLA "$HLO" --target trn2 --output "$outneff" \
    > "$logfile" 2>&1 || true
  
  local end_time=$(date +%s)
  local wall_time=$((end_time - start_time))
  
  local status="FAIL"
  local error_type="unknown"
  if [ -f "$outneff" ]; then
    status="PASS"
    error_type="none"
  else
    if grep -q "bad_alloc" "$logfile" 2>/dev/null; then
      error_type="bad_alloc"
    elif grep -q "Assertion" "$logfile" 2>/dev/null; then
      error_type=$(grep "Assertion" "$logfile" | head -1 | cut -c1-80)
    elif grep -q "SIGSEGV\|signal 11" "$logfile" 2>/dev/null; then
      error_type="SIGSEGV"
    elif grep -q "overlapping" "$logfile" 2>/dev/null; then
      error_type="overlapping"
    elif grep -q "abruptly" "$logfile" 2>/dev/null; then
      error_type="process_aborted"
    fi
  fi
  
  echo "$status (${wall_time}s) $error_type"
  echo "$label,$run_num,$status,$wall_time,$error_type" >> "$OUT/results.csv"
  
  # Clean up
  unset LD_PRELOAD 2>/dev/null || true
  for var in $extra_env; do
    local name="${var%%=*}"
    unset "$name" 2>/dev/null || true
  done
}

echo "label,run,status,wall_s,error_type" > "$OUT/results.csv"

echo "=== Smash FIXAV Debug Test ==="
echo "Running $RUNS iterations with FIXAV=1"
echo ""

# FIXAV mode
echo "fixav (SMASH_FIXAV=1):"
for i in $(seq 1 $RUNS); do
  run_compile "fixav" "$i" "SMASH_VM_GIB=48 SMASH_COLD_TIMEOUT_SEC=10 SMASH_FIXAV=1"
done

echo ""
echo "=== Summary ==="
echo ""
echo "Pass rates:"
for cfg in fixav; do
  total=$(grep "^$cfg," "$OUT/results.csv" | wc -l)
  if [ "$total" -gt 0 ]; then
    pass=$(grep "^$cfg," "$OUT/results.csv" | grep ",PASS," | wc -l)
    echo "  $cfg: $pass/$total"
  fi
done

echo ""
echo "Results in: $OUT/results.csv"
cat "$OUT/results.csv"
