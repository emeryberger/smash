#!/bin/sh
# Paper workloads (full smash) with SMASH_TRACK_EXTERNAL=0 vs =1, interleaved:
# each round runs both settings in a random order, one run each.
#   bench/paper_te_ab.sh <build-dir> <out-dir> <rounds> [apps]
set -u
b=$(cd "$1" && pwd); out=$2; rounds=$3; apps=${4:-sqlite,rocksdb,memcached,redis,redis_ext}
src=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$out"; out=$(cd "$out" && pwd)
for r in $(seq 1 "$rounds"); do
  for te in $(shuf -e 0 1); do
    echo "round $r TE=$te $(date +%T)"
    (cd "$b" && SMASH_TRACK_EXTERNAL=$te python3 "$src/bench/run_paper_experiments.py" --compress-only-only \
       --configs full_smash --apps "$apps" --runs 1 --output-dir "$out/te$te-r$r" > "$out/te$te-r$r.log" 2>&1)
  done
done
echo done
