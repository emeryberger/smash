#!/usr/bin/env bash
set -uo pipefail
cd /local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler/smash
CMD=$(python3 - <<'PY'
import json,re
db=json.load(open("build_bench2/compile_commands.json"))
for e in db:
    if e["file"].endswith("src/smash_heap.cpp"):
        c=re.sub(r'^\S+c\+\+','clang++',e["command"]); c=re.sub(r'-o\s+\S+','',c); c=re.sub(r'-c\s','-fsyntax-only ',c)
        for f in ['-fno-lifetime-dse','-flifetime-dse=1','-mno-omit-leaf-frame-pointer']: c=c.replace(f,'')
        print(c+' -Wthread-safety'); break
PY
)
eval "$CMD" 2>/tmp/ts.out
echo "=== ALL warnings (full) ==="; grep -A2 "warning:" /tmp/ts.out | head -20
