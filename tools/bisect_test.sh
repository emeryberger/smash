#!/usr/bin/env bash
# git bisect test for the sqlite RSS-reduction regression.
# Exit 0 = GOOD (smash reduces RSS during cooling: post_cool < 0.9*peak)
# Exit 1 = BAD  (RSS grows/doesn't drop)
# Exit 125 = SKIP (build failed — can't test this commit)
set -uo pipefail
SROOT=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler/smash
BD=$SROOT/bisect_build
# Reconfigure fresh each time is slow; reuse build dir, just rebuild smash+bench.
if [ ! -f "$BD/CMakeCache.txt" ]; then
  rm -rf "$BD"; mkdir -p "$BD"
  cmake -S "$SROOT" -B "$BD" -DCMAKE_BUILD_TYPE=Release -DSMASH_BUILD_BENCH=ON \
    -DSMASH_BUILD_BENCH_DEPS=OFF -DSMASH_BUILD_BENCH_ALLOCATORS=OFF >/dev/null 2>&1 || exit 125
fi
# Build smash lib + bench_sqlite at this commit
cmake --build "$BD" --target smash bench_sqlite -j32 >/tmp/bisect_build.log 2>&1 || {
  # CMakeLists or source may differ across commits; try a clean reconfigure once
  rm -rf "$BD"; mkdir -p "$BD"
  cmake -S "$SROOT" -B "$BD" -DCMAKE_BUILD_TYPE=Release -DSMASH_BUILD_BENCH=ON \
    -DSMASH_BUILD_BENCH_DEPS=OFF -DSMASH_BUILD_BENCH_ALLOCATORS=OFF >/dev/null 2>&1 || exit 125
  cmake --build "$BD" --target smash bench_sqlite -j32 >/tmp/bisect_build.log 2>&1 || exit 125
}
LIB="$BD/libsmash.so"; BENCH="$BD/bench/bench_sqlite"
[ -f "$LIB" ] && [ -f "$BENCH" ] || exit 125
# Run sqlite under smash, parse peak vs post-cool RSS
OUT=$(LD_PRELOAD="$LIB" SMASH_VERY_COLD_TICKS=5 SMASH_COLD_TIMEOUT_SEC=2 SMASH_DEFER_MADVISE=1 \
  timeout 120 "$BENCH" 2>&1)
peak=$(echo "$OUT" | awk '/METRIC peak_rss_mb/{print $3}' | tail -1)
postcool=$(echo "$OUT" | awk '/METRIC post_cool_rss_mb/{print $3}' | tail -1)
# Some older commits may lack post_cool METRIC; fall back to min_rss
[ -z "$postcool" ] && postcool=$(echo "$OUT" | awk '/METRIC min_rss_mb/{print $3}' | tail -1)
[ -z "$peak" -o -z "$postcool" ] && { echo "PARSE-FAIL peak=$peak postcool=$postcool"; exit 125; }
echo "commit=$(git rev-parse --short HEAD) peak=$peak postcool=$postcool"
# GOOD if post-cool dropped to < 90% of peak
awk -v p="$peak" -v c="$postcool" 'BEGIN{ exit !(c < 0.9*p) }' && { echo GOOD; exit 0; } || { echo BAD; exit 1; }
