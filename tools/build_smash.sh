#!/usr/bin/env bash
set -e
cd /local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler/smash/build
cmake .. -DCMAKE_BUILD_TYPE=Release >/tmp/smash_cmake.log 2>&1 || { echo "CMAKE FAILED"; tail -20 /tmp/smash_cmake.log; exit 1; }
make -j$(nproc) libsmash 2>/tmp/smash_make.log || make -j$(nproc) 2>/tmp/smash_make.log || { echo "MAKE FAILED"; tail -40 /tmp/smash_make.log; exit 1; }
echo "BUILD OK"
ls -la /local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler/smash/build/libsmash.so
