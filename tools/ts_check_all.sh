#!/usr/bin/env bash
set -uo pipefail
cd /local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler/smash
TOTAL_TS=0
for tu in src/smash_heap.cpp src/linux_syscall_wrappers.cpp; do
  CMD=$(python3 - "$tu" <<'PY'
import json,re,sys
tu=sys.argv[1]
db=json.load(open("build_bench2/compile_commands.json"))
for e in db:
    if e["file"].endswith(tu):
        c=re.sub(r'^\S+c\+\+','clang++',e["command"]); c=re.sub(r'-o\s+\S+','',c); c=re.sub(r'-c\s','-fsyntax-only ',c)
        for f in ['-fno-lifetime-dse','-flifetime-dse=1','-mno-omit-leaf-frame-pointer']: c=c.replace(f,'')
        print(c+' -Wthread-safety'); break
PY
)
  [ -z "$CMD" ] && { echo "$tu: not in compile_commands, skip"; continue; }
  eval "$CMD" 2>/tmp/ts_$$.out
  n=$(grep -c "thread-safety-analysis" /tmp/ts_$$.out)
  echo "$tu: $n thread-safety warnings"
  grep "thread-safety-analysis" /tmp/ts_$$.out | head -10
  TOTAL_TS=$((TOTAL_TS+n))
done
echo "=== TOTAL thread-safety warnings across compiled TUs: $TOTAL_TS ==="
