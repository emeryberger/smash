"""CPython dict-heavy workload for bench_bundled_alloc.py.

Small objects (<= 512 B) come from obmalloc, whose 1 MiB arenas are mmap'd
directly (not malloc'd), so under LD_PRELOAD=libsmash they are only reachable
via SMASH_TRACK_EXTERNAL=1. Values are short repetitive strings (compressible).

    python3 bundled_alloc_python.py <n_entries> <cool_sec>

Prints FILLED, sleeps cool_sec, then re-reads every entry and prints
CHECK <ok|bad> <checksum>.
"""
import hashlib
import sys
import time

n = int(sys.argv[1])
cool = int(sys.argv[2])

d = {}
for i in range(n):
    d[f"key:{i:09d}"] = (i, f"value-{i % 1000:04d}-" * 3, [i, i + 1])
# Drop a quarter so obmalloc frees (and munmaps) some arenas.
for i in range(0, n, 4):
    del d[f"key:{i:09d}"]
print(f"FILLED entries={len(d)}", flush=True)
time.sleep(cool)

h = hashlib.sha256()
ok = True
for i in range(n):
    if i % 4 == 0:
        continue
    v = d.get(f"key:{i:09d}")
    if v is None or v[0] != i or v[1] != f"value-{i % 1000:04d}-" * 3 or v[2] != [i, i + 1]:
        ok = False
        print(f"bad entry {i}", file=sys.stderr)
        break
    h.update(v[1].encode())
print(f"CHECK {'ok' if ok else 'bad'} {h.hexdigest()[:16]}", flush=True)
sys.exit(0 if ok else 1)
