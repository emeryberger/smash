#!/bin/sh
# Round-3 measurement chain for the SMASH_TRACK_EXTERNAL auto default
# (cloudnew): fork_stress with TE unset, bundled allocators at the default
# cold timeout (TE 0 vs auto), paper workloads and quick-CI A/B (0 vs auto).
#   bench/te_auto_chain.sh <out-dir>
set -u
out=$1; mkdir -p "$out"
echo "build $(date +%T)"
ninja -C build-linux -j"${JOBS:-32}" > "$out/build.log" 2>&1 || { echo "build-linux FAILED"; exit 1; }
ninja -C build-paper -j"${JOBS:-32}" smash smash_noopt smash_compress_only bench_sqlite bench_rocksdb >> "$out/build.log" 2>&1 \
  || { echo "build-paper FAILED"; exit 1; }
echo "fork $(date +%T)"; sh bench/fork_stress_loop.sh build-linux 0-1 20 > "$out/fs_auto.log" 2>&1
echo "bundled $(date +%T)"
SMASH_TMP=$PWD/tmp python3 bench/bench_bundled_alloc.py --build-dir build-linux --valkey-dir tmp/deps/valkey/src \
  --python python3.12 --mimalloc-inc tmp/deps/mimalloc/include --mimalloc-lib tmp/deps/mimalloc/build \
  --tcmalloc-name tcmalloc --apps valkey,mi,tc,python --configs full:0,full:auto,large:0,large:auto \
  --cold-timeout "" --runs 5 --cool 40 --mib 1024 --py-entries 3000000 --valkey-ops 3000000 --port 16541 \
  --json "$out/bb10.json" > "$out/bb10.log" 2>&1
echo "paper $(date +%T)"; SETTINGS="0 auto" sh bench/paper_te_ab.sh build-paper "$out/paper" 5 > "$out/paper.log" 2>&1
echo "ab $(date +%T)"; python3 bench/ab_track_external.py --build-dir build-linux --runs 5 --settings 0,auto > "$out/ab.log" 2>&1
echo "done $(date +%T)"
