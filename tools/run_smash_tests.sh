#!/usr/bin/env bash
cd /local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler/smash/build_fix
ctest --output-on-failure 2>&1 | tail -40
