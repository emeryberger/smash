#!/usr/bin/env bash
# Test batched mprotect (SMASH_P2_CHUNK=1) vs per-page (control) on neuron-cc.
# perf flat profile showed ~11% of cycles in TLB-shootdown IPIs from per-page
# mprotect(PROT_NONE). P2_CHUNK coalesces contiguous compressed pages into one
# mprotect (up to 16 pages = 1 IPI instead of 16). Stats disabled (printStats
# was 1.3%) so we measure the real path.
#   H: control  (per-page mprotect, current default)
#   I: P2_CHUNK (batched mprotect over coalesced runs)
set -uo pipefail
NCC=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler
SMASH=$NCC/smash/build_fix/libsmash.so
PYTHON=/opt/venvs/3.13/bin/python3
HLO=$NCC/test7_fsdp_c474_b8_63l_135416_def_modular.hlo
PROFILE=${PROFILE:-$(ls -t $NCC/smash/build_fix/profgen_*/profile.bin 2>/dev/null | head -1)}
OUT=$NCC/smash/build_fix/p2_$(date +%Y%m%d_%H%M%S)
mkdir -p "$OUT"
echo "profile: $PROFILE"
echo "label,status,wall_s" > "$OUT/summary.csv"

run(){
  local label="$1"; shift
  local log="$OUT/${label}.log" neff="$OUT/${label}.neff"
  echo "=== $label ===  extra: $*"
  ( export PYTHONPATH="$NCC:$NCC/build/python/3.13/install" PYTHONMALLOC=malloc \
      LD_PRELOAD="$SMASH" SMASH_VM_GIB=48 SMASH_DEFER_MADVISE=1 \
      SMASH_COLD_TIMEOUT_SEC=10 SMASH_PROFILE_FILE="$PROFILE"
    for kv in "$@"; do export "$kv"; done
    exec "$PYTHON" -m neuronxcc.driver.CommandDriver compile --framework XLA "$HLO" \
      --target trn2 --output "$neff" > "$log" 2>&1 ) &
  local pid=$! st=$(date +%s)
  wait "$pid"
  local wall=$(($(date +%s)-st))
  local status=FAIL; [ -f "$neff" ] && status=PASS
  echo "  $label: status=$status wall=${wall}s"
  echo "$label,$status,$wall" >> "$OUT/summary.csv"
}

# Two runs each to smooth variance.
run H_control_1
run I_p2chunk_1  SMASH_P2_CHUNK=1
run H_control_2
run I_p2chunk_2  SMASH_P2_CHUNK=1

echo ""; echo "=== SUMMARY ==="; column -t -s, "$OUT/summary.csv"
echo "OUT: $OUT"
