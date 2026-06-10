#!/usr/bin/env bash
# Memory benchmark using mstat (cgroup v2 memory.current of the whole process
# tree) instead of summed /proc RSS. RSS mismeasures MADV_FREE (pages stay
# resident until reclaimed) and double-counts shared pages; cgroup
# memory.current reflects real physical pressure.
#
# Runs each config in its OWN --privileged --cgroupns=private container with
# host mstat bind-mounted. mstat needs to manage cgroup controllers, which
# requires moving the container init out of the root cgroup first (the cgroup
# v2 "no internal processes" rule), done in the inner script.
#
# Usage: mstat_bench.sh <label> <ENV_KV ...>   (one config per invocation)
# Writes <label>.mstat.tsv + appends to mstat_summary.csv in $OUT (env or new).
set -uo pipefail
ROOT=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler
IMAGE=480188351710.dkr.ecr.us-west-2.amazonaws.com/neuron-compiler/build-image-x86_64:1.0.4242.0
LABEL=${1:?need label}; shift
EXTRA_ENV="$*"
OUT=${MSTAT_OUT:-$ROOT/smash/build_fix/mstat_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$OUT"
PROFILE=${PROFILE:-$(ls -t $ROOT/smash/build_fix/profgen_*/profile.bin 2>/dev/null|head -1)}
[ -f "$OUT/summary.csv" ] || echo "label,status,wall_s,peak_mib,avg_mib" > "$OUT/summary.csv"

ENVFLAGS=""
for kv in $EXTRA_ENV; do ENVFLAGS="$ENVFLAGS -e $kv"; done

cat > "$OUT/inner_$LABEL.sh" << INNER
#!/usr/bin/env bash
set -uo pipefail
# Move init out of the root cgroup so mstat can enable controllers in root.
mkdir -p /sys/fs/cgroup/init 2>/dev/null || true
echo \$\$ > /sys/fs/cgroup/init/cgroup.procs 2>/dev/null || true
source $ROOT/container-build/env.sh
export PYTHONMALLOC=malloc
# LD_PRELOAD_OVERRIDE lets the caller swap in jemalloc/glibc baselines; default
# is smash full-mode with the loaded profile. (The SMASH_* vars are harmless
# when LD_PRELOAD points elsewhere.)
if [ -n "\${LD_PRELOAD_OVERRIDE:-}" ]; then
  export LD_PRELOAD="\$LD_PRELOAD_OVERRIDE"
else
  export LD_PRELOAD=$ROOT/smash/build_fix/libsmash.so
  export SMASH_VM_GIB=48 SMASH_COLD_TIMEOUT_SEC=10 SMASH_DEFER_MADVISE=1 SMASH_PROFILE_FILE=$PROFILE
fi
TSV=$OUT/${LABEL}.mstat.tsv
# mstat hardcodes memory.max=16GiB on the cgroup it creates, which OOM-kills a
# ~17.5GB neuron-cc compile (F137). Background watcher: as soon as an mstat-*
# cgroup appears, raise its memory.max to "max" so accounting still works but
# nothing is capped. Loops until the compile is done.
( for _ in \$(seq 1 600); do
    for cg in /sys/fs/cgroup/mstat-*; do
      [ -e "\$cg/memory.max" ] && echo max > "\$cg/memory.max" 2>/dev/null
    done
    sleep 0.2
  done ) &
WATCHER=\$!
start=\$(date +%s)
# mstat discards its TSV if the wrapped child exits non-zero (it bails with
# exec.ExitError). neuron-cc's driver exits rc=245 even on success (known
# cleanup-path quirk), so wrap in 'bash -c "...; true"' to force exit 0 — the
# .neff presence is the real success signal, checked below.
mstat -freq 5 -o "\$TSV" -- \
  bash -c "/opt/venvs/3.13/bin/python3 -m neuronxcc.driver.CommandDriver compile \
    --framework XLA $ROOT/test7_fsdp_c474_b8_63l_135416_def_modular.hlo \
    --target trn2 --output $OUT/${LABEL}.neff > $OUT/${LABEL}.log 2>&1; true"
rc=\$?
kill \$WATCHER 2>/dev/null || true
wall=\$((\$(date +%s)-start))
status=FAIL; [ -f "$OUT/${LABEL}.neff" ] && status=PASS
peak=\$(awk -F'\t' 'NR>1{if(\$2>m)m=\$2}END{print int(m/1048576)}' "\$TSV" 2>/dev/null)
avg=\$(awk -F'\t' 'NR>1&&\$2>0{s+=\$2;n++}END{if(n>0)print int(s/n/1048576);else print 0}' "\$TSV" 2>/dev/null)
echo "RESULT $LABEL status=\$status rc=\$rc wall=\${wall}s peak=\${peak}MiB avg=\${avg}MiB"
echo "$LABEL,\$status,\$wall,\$peak,\$avg" >> $OUT/summary.csv
INNER
chmod +x "$OUT/inner_$LABEL.sh"

# For jemalloc/glibc baselines: caller passes LD_PRELOAD_OVERRIDE.
docker run -i --rm --privileged --cgroupns=private \
  -v "$ROOT:$ROOT" -v "$ROOT/.local-build:$ROOT/build" -v /usr/bin/mstat:/usr/bin/mstat:ro \
  -e MISE_DATA_DIR=/usr/local/share/mise -e MISE_OFFLINE=1 -e MOUNT_POINT="$ROOT" \
  $ENVFLAGS -w "$ROOT" \
  "$IMAGE" bash "$OUT/inner_$LABEL.sh" 2>&1 | grep -E "RESULT|Error|error|Traceback" | head -5
echo "OUT=$OUT"
