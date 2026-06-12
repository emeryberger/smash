#!/bin/bash
# run_alloc_speed.sh — Compare allocation speed across allocators.
# Run from the build directory.
#
# Usage: bash ../bench/run_alloc_speed.sh [--threads N] [--duration SEC]
#
# Outputs a comparison table of threadtest / larson / linux-scalability
# for glibc, jemalloc, mimalloc, and smash.

set -euo pipefail

THREADS=${THREADS:-$(nproc)}
DURATION=${DURATION:-5}
ITERATIONS=${ITERATIONS:-2000000}
BENCH=./bench/bench_alloc_speed

# Parse args
for arg in "$@"; do
    case $arg in
        --threads=*) THREADS="${arg#*=}" ;;
        --duration=*) DURATION="${arg#*=}" ;;
        --iterations=*) ITERATIONS="${arg#*=}" ;;
    esac
done

if [ ! -f "$BENCH" ]; then
    echo "ERROR: $BENCH not found. Build with -DSMASH_BUILD_BENCH=ON" >&2
    exit 1
fi

ARGS="--threads $THREADS --duration $DURATION --iterations $ITERATIONS"

# Detect allocator libraries
SMASH_LIB=""
[ -f ./libsmash.so ] && SMASH_LIB=./libsmash.so
[ -f ./libsmash.dylib ] && SMASH_LIB=./libsmash.dylib

JEMALLOC_LIB=""
for p in /usr/lib64/libjemalloc.so /usr/local/lib/libjemalloc.so /opt/homebrew/lib/libjemalloc.dylib; do
    [ -f "$p" ] && JEMALLOC_LIB="$p" && break
done

MIMALLOC_LIB=""
for p in /usr/local/lib64/libmimalloc.so /usr/local/lib/libmimalloc.so /opt/homebrew/lib/libmimalloc.dylib; do
    [ -f "$p" ] && MIMALLOC_LIB="$p" && break
done

# Platform preload var
if [ "$(uname)" = "Darwin" ]; then
    PRELOAD_VAR=DYLD_INSERT_LIBRARIES
else
    PRELOAD_VAR=LD_PRELOAD
fi

echo "=== Allocation Speed Comparison ==="
echo "Threads: $THREADS  Duration: ${DURATION}s  Iterations: $ITERATIONS"
echo "Platform: $(uname -m) $(uname -s)"
echo ""

echo "--- glibc (system malloc) ---"
$BENCH $ARGS
echo ""

if [ -n "$JEMALLOC_LIB" ]; then
    echo "--- jemalloc ($JEMALLOC_LIB) ---"
    env $PRELOAD_VAR="$JEMALLOC_LIB" $BENCH $ARGS
    echo ""
fi

if [ -n "$MIMALLOC_LIB" ]; then
    echo "--- mimalloc ($MIMALLOC_LIB) ---"
    env $PRELOAD_VAR="$MIMALLOC_LIB" $BENCH $ARGS
    echo ""
fi

if [ -n "$SMASH_LIB" ]; then
    echo "--- smash ($SMASH_LIB) ---"
    env $PRELOAD_VAR="$SMASH_LIB" $BENCH $ARGS
    echo ""
fi
