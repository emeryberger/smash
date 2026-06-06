#!/usr/bin/env bash
# Test one commit's libsmash against the FIXED build_bench2 bench infra.
# Usage: bisect_libonly.sh <commit>
# Checks out src/+include/ at <commit>, rebuilds smash in build_bench2, runs the
# prebuilt allocator-agnostic bench_sqlite, prints peak/postcool and GOOD/BAD.
set -uo pipefail
SROOT=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler/smash
cd "$SROOT"
C="$1"
git checkout -q "$C" -- src include 2>/dev/null || { echo "$C CHECKOUT-FAIL"; exit 125; }
if ! cmake --build build_bench2 --target smash -j32 >/tmp/bl.log 2>&1; then
  echo "$C BUILD-FAIL"; git checkout -q 0f688dc -- src include; exit 125
fi
OUT=$(LD_PRELOAD="$SROOT/build_bench2/libsmash.so" SMASH_VERY_COLD_TICKS=5 SMASH_COLD_TIMEOUT_SEC=2 SMASH_DEFER_MADVISE=1 \
  timeout 120 "$SROOT/build_bench2/bench/bench_sqlite" 2>&1)
peak=$(echo "$OUT"|awk '/METRIC peak_rss_mb/{print $3}'|tail -1)
pc=$(echo "$OUT"|awk '/METRIC post_cool_rss_mb/{print $3}'|tail -1)
[ -z "$pc" ] && pc=$(echo "$OUT"|awk '/METRIC min_rss_mb/{print $3}'|tail -1)
git checkout -q 0f688dc -- src include 2>/dev/null
[ -z "$peak" -o -z "$pc" ] && { echo "$C PARSE-FAIL"; exit 125; }
verdict=$(awk -v p="$peak" -v c="$pc" 'BEGIN{print (c<0.9*p)?"GOOD":"BAD"}')
echo "$(git rev-parse --short $C) peak=$peak postcool=$pc $verdict"
