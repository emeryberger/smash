#!/usr/bin/env bash
set -uo pipefail
cd /local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler/smash
# Extract the exact compile command for smash_heap.cpp from compile_commands.json,
# swap c++ -> clang++ and add thread-safety flags, drop -o (syntax-only).
CMD=$(python3 - <<'PY'
import json,re
db=json.load(open("build_bench2/compile_commands.json"))
for e in db:
    if e["file"].endswith("src/smash_heap.cpp"):
        c=e["command"]
        c=re.sub(r'^\S+c\+\+','clang++',c)         # compiler -> clang++
        c=re.sub(r'-o\s+\S+','',c)                  # drop output
        c=re.sub(r'-c\s','-fsyntax-only ',c)        # syntax-only
        # strip gcc-only flags clang rejects
        for f in ['-fno-lifetime-dse','-flifetime-dse=1','-mno-omit-leaf-frame-pointer']:
            c=c.replace(f,'')
        print(c + ' -Wthread-safety')
        break
PY
)
echo "=== running clang -Wthread-safety on smash_heap.cpp ==="
eval "$CMD" 2>/tmp/ts.out
echo "clang exit=$?"
echo "=== thread-safety warnings ==="
grep -iE "warning:.*(thread.?safety|mutex|capability|guarded|requires|acquir|releas|holding)" /tmp/ts.out | head -40
echo "=== counts: warnings=$(grep -c 'warning:' /tmp/ts.out) errors=$(grep -c 'error:' /tmp/ts.out) ==="
echo "=== any hard errors (non-ts)? ==="; grep "error:" /tmp/ts.out | head -10
