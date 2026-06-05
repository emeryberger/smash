#!/usr/bin/env bash
set -uo pipefail
NCC=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler
SMASH=$NCC/smash/build_fix/libsmash.so
PYTHON=/opt/venvs/3.13/bin/python3
HLO=$NCC/test7_fsdp_c474_b8_63l_135416_def_modular.hlo
OUT=$NCC/smash/build_fix/profgen_$(date +%Y%m%d_%H%M%S)
mkdir -p "$OUT"
PROFILE=$OUT/profile.bin
export PYTHONPATH="$NCC:$NCC/build/python/3.13/install"
export PYTHONMALLOC=malloc LD_PRELOAD="$SMASH"
export SMASH_VM_GIB=48 SMASH_COLD_TIMEOUT_SEC=10 SMASH_DEFER_MADVISE=1
export SMASH_PROFILE_FILE="$PROFILE" SMASH_PROFILE_FILE_RW=1
start=$(date +%s)
"$PYTHON" -m neuronxcc.driver.CommandDriver compile --framework XLA "$HLO" \
  --target trn2 --output "$OUT/out.neff" > "$OUT/compile.log" 2>&1
rc=$?; wall=$(($(date +%s)-start))
status=FAIL; [ -f "$OUT/out.neff" ] && status=PASS
echo "RESULT status=$status rc=$rc wall=${wall}s"
ls -la "$PROFILE" 2>/dev/null && echo "PROFILE_PATH=$PROFILE" || echo "NO PROFILE WRITTEN"
