#!/usr/bin/env bash
set -e
SMASH=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler/smash
BUILD=$SMASH/build_fix
rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"
echo "=== whoami: $(whoami) uid=$(id -u) ==="
cmake .. -DCMAKE_BUILD_TYPE=Release -DSMASH_BUILD_BENCH=OFF >/tmp/smash_cmake.log 2>&1 || { echo "CMAKE FAILED"; tail -30 /tmp/smash_cmake.log; exit 1; }
make -j$(nproc) 2>/tmp/smash_make.log || { echo "MAKE FAILED"; tail -50 /tmp/smash_make.log; exit 1; }
echo "BUILD OK"
ls -la "$BUILD"/libsmash.so
