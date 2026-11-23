#!/bin/sh
# fork_stress_test N times per mode, pinned to the given CPUs, with the given
# extra environment (e.g. SMASH_TRACK_EXTERNAL=1). Prints one line per run
# and a pass count per mode.
#   bench/fork_stress_loop.sh <build-dir> <cpus> <runs> [VAR=value ...]
set -u
b=$1; cpus=$2; runs=$3; shift 3
for lo in 0 1; do
  pass=0
  for i in $(seq 1 "$runs"); do
    out=$(env "$@" LD_PRELOAD="$b/libsmash.so" SMASH_LARGE_ONLY=$lo SMASH_DEFER_PHASES_MS=0 \
          SMASH_COLD_TIMEOUT_SEC=1 timeout 600 taskset -c "$cpus" "$b/tests/fork_stress_test" 150 6 2>&1)
    rc=$?
    [ $rc -eq 0 ] && pass=$((pass + 1))
    echo "lo=$lo run=$i rc=$rc $(echo "$out" | grep -E 'hung=|FAIL' | head -3 | tr '\n' ' ')"
  done
  echo "MODE lo=$lo pass=$pass/$runs"
done
