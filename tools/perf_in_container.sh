#!/usr/bin/env bash
# Lightweight system-wide perf profile of one full-mode smash compile, in a
# --privileged container (perf_event_open). FLAT profile only: no call-graph,
# no dwarf — the prior dwarf+199Hz system-wide run generated 27 GB. Flat at
# 99 Hz is a few hundred MB and directly answers "where do the cycles go"
# (which DSO / which symbol). Mirrors ./local run's mounts/env so the source
# tree's absolute symlinks resolve.
set -uo pipefail

ROOT=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler
IMAGE=480188351710.dkr.ecr.us-west-2.amazonaws.com/neuron-compiler/build-image-x86_64:1.0.4242.0
EXTRA_ENV=${1:-"SMASH_VM_GIB=48 SMASH_COLD_TIMEOUT_SEC=10 SMASH_DEFER_MADVISE=1"}
SUBDIR=perf_$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/smash/build_fix/$SUBDIR
mkdir -p "$OUT"

ENVFLAGS=""
for var in $EXTRA_ENV; do ENVFLAGS="$ENVFLAGS -e $var"; done

cat > "$OUT/inner.sh" << INNER
#!/usr/bin/env bash
set -uo pipefail
source $ROOT/container-build/env.sh
ROOT=$ROOT
SMASH=\$ROOT/smash/build_fix/libsmash.so
PYTHON=/opt/venvs/3.13/bin/python3
HLO=\$ROOT/test7_fsdp_c474_b8_63l_135416_def_modular.hlo
OUT=$OUT
export PYTHONMALLOC=malloc LD_PRELOAD="\$SMASH" SMASH_STATS=1 SMASH_BUCKET_STATS=1
echo "perf_event_paranoid=\$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null)"
start=\$(date +%s)
# FLAT, system-wide, 99 Hz, cycles, NO call-graph. Keeps perf.data small.
perf record -F 99 -a -e cycles -o "\$OUT/perf.data" -- \
  "\$PYTHON" -m neuronxcc.driver.CommandDriver compile --framework XLA "\$HLO" \
    --target trn2 --output "\$OUT/out.neff" > "\$OUT/compile.log" 2>&1
rc=\$?
wall=\$((\$(date +%s)-start))
status="FAIL"; [ -f "\$OUT/out.neff" ] && status="PASS"
echo "RESULT status=\$status rc=\$rc wall=\${wall}s perf_data=\$(du -h "\$OUT/perf.data" 2>/dev/null|cut -f1)"
echo "=== top DSOs (where cycles live) ==="
perf report -i "\$OUT/perf.data" --stdio --sort dso --percent-limit 0.3 2>/dev/null \
  | grep -vE '^#|^\$' | head -30 > "\$OUT/perf_dso.txt"
cat "\$OUT/perf_dso.txt"
echo "=== top symbols ==="
perf report -i "\$OUT/perf.data" --stdio --sort symbol --percent-limit 0.2 2>/dev/null \
  | grep -vE '^#|^\$' | head -60 > "\$OUT/perf_sym.txt"
cat "\$OUT/perf_sym.txt"
echo "=== top symbol+dso ==="
perf report -i "\$OUT/perf.data" --stdio --sort dso,symbol --percent-limit 0.2 2>/dev/null \
  | grep -vE '^#|^\$' | head -60 > "\$OUT/perf_symdso.txt"
echo "=== smash churn ==="
awk '/\[smash bucket\]/ && \$3 ~ /^[0-9]+\$/ {comp+=\$(NF-2); dec+=\$(NF-1)}
     END {printf "total_compress=%d total_decompress=%d churn=%.1f%%\n", comp, dec, (comp>0?dec*100.0/comp:0)}' "\$OUT/compile.log"
# Drop the raw perf.data to save space once reports are extracted.
rm -f "\$OUT/perf.data"
INNER
chmod +x "$OUT/inner.sh"

docker run -i --rm --privileged --network host \
  -v "$ROOT:$ROOT" \
  -v "$ROOT/.local-build:$ROOT/build" \
  -e MISE_DATA_DIR=/usr/local/share/mise -e MISE_CONFIG_DIR=/tmp/mise/config \
  -e MISE_STATE_DIR=/tmp/mise/state -e MISE_CACHE_DIR=/tmp/mise/cache -e MISE_OFFLINE=1 \
  -e MOUNT_POINT="$ROOT" \
  $ENVFLAGS -w "$ROOT" \
  "$IMAGE" bash "$OUT/inner.sh" 2>&1

echo "OUT: $OUT"
