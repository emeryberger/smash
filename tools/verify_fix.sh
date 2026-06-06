#!/usr/bin/env bash
# Verify the decompress-restore TOCTOU fix on neuron-cc full mode.
set -uo pipefail

NCC=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler
SMASH=$NCC/smash/build_fix/libsmash.so
PYTHON=/opt/venvs/3.13/bin/python3
HLO=$NCC/test7_fsdp_c474_b8_63l_135416_def_modular.hlo
OUT=$NCC/smash/build_fix/verify_$(date +%Y%m%d_%H%M%S)
RUNS=${1:-5}

mkdir -p "$OUT"
export PYTHONPATH="$NCC:$NCC/build/python/3.13/install"
echo "label,run,status,wall_s,error_type" > "$OUT/results.csv"

run_compile() {
  local label="$1" run_num="$2" extra_env="$3"
  local outneff="$OUT/${label}_run${run_num}.neff"
  local logfile="$OUT/${label}_run${run_num}.log"
  echo -n "  Run $run_num: "
  rm -f "$outneff"
  local start_time=$(date +%s)
  export PYTHONMALLOC=malloc
  export LD_PRELOAD="$SMASH"
  for var in $extra_env; do export "$var"; done
  "$PYTHON" -m neuronxcc.driver.CommandDriver compile \
    --framework XLA "$HLO" --target trn2 --output "$outneff" > "$logfile" 2>&1 || true
  local end_time=$(date +%s)
  local wall_time=$((end_time - start_time))
  local status="FAIL" error_type="unknown"
  if [ -f "$outneff" ]; then status="PASS"; error_type="none"
  else
    if grep -q "overlapping" "$logfile" 2>/dev/null; then error_type="overlapping"
    elif grep -q "BIR verification" "$logfile" 2>/dev/null; then error_type="bir_verify"
    elif grep -q "Assertion" "$logfile" 2>/dev/null; then error_type="assertion"
    elif grep -q "could not find any ready inst" "$logfile" 2>/dev/null; then error_type="sched"
    elif grep -q "bad_alloc" "$logfile" 2>/dev/null; then error_type="bad_alloc"
    elif grep -q "abruptly" "$logfile" 2>/dev/null; then error_type="proc_abort"
    fi
  fi
  echo "$status (${wall_time}s) $error_type"
  echo "$label,$run_num,$status,$wall_time,$error_type" >> "$OUT/results.csv"
  unset LD_PRELOAD 2>/dev/null || true
  for var in $extra_env; do unset "${var%%=*}" 2>/dev/null || true; done
}

echo "=== Verify decompress-restore TOCTOU fix (full mode) ==="
echo "libsmash: $SMASH (built $(date -r $SMASH '+%Y-%m-%d %H:%M' 2>/dev/null))"
echo ""
echo "full_default:"
for i in $(seq 1 $RUNS); do
  run_compile "full_default" "$i" "SMASH_VM_GIB=48 SMASH_COLD_TIMEOUT_SEC=10 SMASH_DEFER_MADVISE=1"
done

echo ""
echo "=== Summary ==="
pass=$(grep ",PASS," "$OUT/results.csv" | wc -l)
total=$(($(wc -l < "$OUT/results.csv")-1))
echo "full_default: $pass/$total PASS"
echo ""
cat "$OUT/results.csv"
