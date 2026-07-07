#!/bin/bash
# Drive full-scale workloads under the cohort-instrumented libsmash and
# capture the final per-run cohort tally. Full mode (small + large tracked).
set -u
cd /Users/emerydb/git/smash/build_cohort
LIB=./libsmash.dylib
COMMON="SMASH_COLD_TIMEOUT_SEC=4"
OUT=/Users/emerydb/git/smash/tmp
DB=/Users/emerydb/git/smash/tmp/rocksdb_cohort

run() {
  local name="$1"; shift
  echo "=== RUN $name ==="
  DYLD_INSERT_LIBRARIES=$LIB env $COMMON "$@" \
      1>"$OUT/${name}.out" 2>"$OUT/${name}.err"
  echo "  exit=$? -> $OUT/${name}.{out,err}"
}

# sqlite full: 500K rows, 15s serve (skewed hot re-access drives faults)
run sqlite_full ./bench/bench_sqlite --rows 500000 --cool 12 --serve 25

# rocksdb full: 500K keys, 512B values, hot recent-key reads during serve
rm -rf "$DB"
run rocksdb_full ./bench/bench_rocksdb --db "$DB" --keys 500000 --value-size 512 --cool 12 --serve 25
rm -rf "$DB"

echo "ALL DONE"
