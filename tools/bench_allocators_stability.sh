#!/usr/bin/env bash
# Test allocator stability - run multiple iterations with each allocator
# to determine if assertion failures are allocator-specific or general
set -euo pipefail

NCC=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler
ALLOC_DIR=$NCC/smash/build/allocators
SMASH=$NCC/smash/build/libsmash.so
PYTHON=/opt/venvs/3.13/bin/python3
HLO=$NCC/test7_fsdp_c474_b8_63l_135416_def_modular.hlo
OUT=$NCC/smash/build/stability_bench_$(date +%Y%m%d_%H%M%S)
RUNS=3

mkdir -p "$OUT"

export PYTHONPATH="$NCC:$NCC/build/python/3.13/install"

run_compile() {
  local label="$1"
  local run_num="$2"
  local preload="${3:-}"
  local extra_env="${4:-}"

  local outneff="$OUT/${label}_run${run_num}.neff"
  local logfile="$OUT/${label}_run${run_num}.log"

  echo -n "  Run $run_num: "

  rm -f "$outneff"

  local start_time=$(date +%s)

  # Set up environment
  export PYTHONMALLOC=malloc
  if [ -n "$preload" ]; then
    export LD_PRELOAD="$preload"
  else
    unset LD_PRELOAD 2>/dev/null || true
  fi

  if [ -n "$extra_env" ]; then
    for var in $extra_env; do
      export "$var"
    done
  fi

  # Run compilation
  "$PYTHON" -m neuronxcc.driver.CommandDriver compile \
    --framework XLA "$HLO" --target trn2 --output "$outneff" \
    > "$logfile" 2>&1
  local rc=$?

  local end_time=$(date +%s)
  local wall_time=$((end_time - start_time))

  # Check result
  local status="FAIL"
  local error_type="unknown"
  if [ -f "$outneff" ]; then
    status="PASS"
    error_type="none"
  else
    # Extract error type from log
    if grep -q "bad_alloc" "$logfile" 2>/dev/null; then
      error_type="bad_alloc"
    elif grep -q "Assertion" "$logfile" 2>/dev/null; then
      error_type=$(grep "Assertion" "$logfile" | head -1 | sed 's/.*Assertion/Assertion/' | cut -c1-60)
    elif grep -q "SIGSEGV\|signal 11" "$logfile" 2>/dev/null; then
      error_type="SIGSEGV"
    fi
  fi

  echo "$status (${wall_time}s) $error_type"
  echo "$label,$run_num,$status,$wall_time,$error_type" >> "$OUT/results.csv"

  # Clean up exports
  unset LD_PRELOAD 2>/dev/null || true
  if [ -n "$extra_env" ]; then
    for var in $extra_env; do
      local name="${var%%=*}"
      unset "$name" 2>/dev/null || true
    done
  fi
}

echo "label,run,status,wall_s,error_type" > "$OUT/results.csv"

echo "=== Allocator Stability Test ==="
echo "Running $RUNS iterations of each allocator"
echo ""

# glibc
echo "glibc:"
for i in $(seq 1 $RUNS); do
  run_compile "glibc" "$i" "" ""
done

# jemalloc
echo ""
echo "jemalloc:"
for i in $(seq 1 $RUNS); do
  run_compile "jemalloc" "$i" "$ALLOC_DIR/lib/libjemalloc.so" ""
done

# mimalloc
echo ""
echo "mimalloc:"
for i in $(seq 1 $RUNS); do
  run_compile "mimalloc" "$i" "$ALLOC_DIR/lib64/libmimalloc.so" ""
done

# tcmalloc - need to build it first or find it
TCMALLOC=""
if [ -f "$ALLOC_DIR/lib/libtcmalloc.so" ]; then
  TCMALLOC="$ALLOC_DIR/lib/libtcmalloc.so"
elif [ -f "/usr/lib64/libtcmalloc_minimal.so" ]; then
  TCMALLOC="/usr/lib64/libtcmalloc_minimal.so"
elif [ -f "/usr/lib64/libtcmalloc.so.4" ]; then
  TCMALLOC="/usr/lib64/libtcmalloc.so.4"
fi

if [ -n "$TCMALLOC" ]; then
  echo ""
  echo "tcmalloc ($TCMALLOC):"
  for i in $(seq 1 $RUNS); do
    run_compile "tcmalloc" "$i" "$TCMALLOC" ""
  done
else
  echo ""
  echo "tcmalloc: not found, skipping"
fi

# smash full mode
echo ""
echo "smash_full:"
for i in $(seq 1 $RUNS); do
  run_compile "smash_full" "$i" "$SMASH" "SMASH_VM_GIB=48 SMASH_COLD_TIMEOUT_SEC=10 SMASH_DEFER_MADVISE=1"
done

echo ""
echo "=== Summary ==="
echo ""
echo "Pass rates:"
for alloc in glibc jemalloc mimalloc tcmalloc smash_full; do
  total=$(grep "^$alloc," "$OUT/results.csv" | wc -l)
  if [ "$total" -gt 0 ]; then
    pass=$(grep "^$alloc," "$OUT/results.csv" | grep ",PASS," | wc -l)
    echo "  $alloc: $pass/$total"
  fi
done

echo ""
echo "Failure breakdown:"
grep ",FAIL," "$OUT/results.csv" | cut -d, -f1,5 | sort | uniq -c || echo "  (no failures)"

echo ""
echo "Results in: $OUT/results.csv"
cat "$OUT/results.csv"
