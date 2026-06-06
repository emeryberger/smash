#!/usr/bin/env bash
# Redis allocator comparison with mstat (cgroup) accounting.
# Workload (documented best practice from CLAUDE.md): redis-server with bg tasks
# disabled, fill via redis-benchmark SET, optional DELETE 50%, cool, then GET.
# mstat wraps redis-server so its cgroup memory.current is tracked for the
# server's whole lifetime (fill+cool+serve). One alloc per invocation arg.
set -uo pipefail
SROOT=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler/smash
BIN=$SROOT/build_bench2/bench/deps/bin
ALLOC=$SROOT/build/allocators/lib
SMASH_LIB=$SROOT/build_bench2/libsmash.so
RS=$BIN/redis-server-libc; RB=$BIN/redis-benchmark; RC=$BIN/redis-cli
PORT=${PORT:-7711}
N=${N:-200000}           # keys
VSIZE=${VSIZE:-2048}     # value bytes (compressible-ish default redis data)
COOL=${COOL:-20}
VARIANT=${VARIANT:-standard}   # standard | extended (delete 50%)
OUT=${OUT:?set OUT}
mkdir -p "$OUT"
[ -f "$OUT/redis_${VARIANT}.csv" ] || echo "alloc,fill_rps,get_rps,peak_mib,min_after_cool_mib,avg_mib" > "$OUT/redis_${VARIANT}.csv"

REDIS_FLAGS="--port $PORT --hz 1 --dynamic-hz no --activedefrag no --activerehashing no \
  --lazyfree-lazy-user-del no --lazyfree-lazy-expire no --lazyfree-lazy-eviction no \
  --maxmemory-policy noeviction --save  --appendonly no --daemonize no --protected-mode no"

run_alloc(){
  local alloc="$1" pre="" env=""
  case "$alloc" in
    glibc) pre="";;
    jemalloc) pre="$ALLOC/libjemalloc.so";;
    mimalloc) pre="$ALLOC/libmimalloc.so";;
    smash) pre="$SMASH_LIB"; env="SMASH_COLD_TIMEOUT_SEC=3 SMASH_DEFER_MADVISE=1";;
  esac
  local tsv="$OUT/redis_${VARIANT}_${alloc}.tsv" log="$OUT/redis_${VARIANT}_${alloc}.log"
  # Launch server under mstat
  ( [ -n "$pre" ] && export LD_PRELOAD="$pre"; for kv in $env; do export "$kv"; done
    exec mstat -freq 10 -o "$tsv" -- $RS $REDIS_FLAGS ) > "$log" 2>&1 &
  local mpid=$!
  # wait for server ready
  for i in $(seq 1 50); do $RC -p $PORT ping 2>/dev/null | grep -q PONG && break; sleep 0.2; done
  # Fill
  local fill=$($RB -p $PORT -t set -n $N -d $VSIZE -r $((N*2)) -q 2>/dev/null | sed -E 's/.*: ([0-9.]+) requests.*/\1/' | head -1)
  if [ "$VARIANT" = "extended" ]; then
    # delete ~50% of keys
    $RC -p $PORT --scan 2>/dev/null | awk 'NR%2==0' | $RC -p $PORT -x del >/dev/null 2>&1 || true
  fi
  # Cool
  sleep $COOL
  # Serve (GET)
  local get=$($RB -p $PORT -t get -n $N -d $VSIZE -r $((N*2)) -q 2>/dev/null | sed -E 's/.*: ([0-9.]+) requests.*/\1/' | head -1)
  sleep 1
  # Shut the server down cleanly (redis exits 0) so mstat flushes its TSV.
  # Do NOT signal mstat ($mpid) — that aborts it before it writes the TSV.
  $RC -p $PORT shutdown nosave 2>/dev/null || true
  wait $mpid 2>/dev/null || true
  local peak=$(awk -F'\t' 'NR>1{if($2>m)m=$2}END{printf "%.0f",m/1048576}' "$tsv" 2>/dev/null)
  local avg=$(awk -F'\t' 'NR>1&&$2>0{s+=$2;n++}END{if(n)printf "%.0f",s/n/1048576}' "$tsv" 2>/dev/null)
  # min during cooling. IMPORTANT: trim the shutdown-teardown tail first — the
  # sampler keeps reading memory.current as redis-server exits, so the final
  # sample(s) capture the emptying cgroup, not idle reclaim (this artifact once
  # made mimalloc look like it reclaimed to 2 MiB when it was flat at 411).
  # Drop trailing samples < 50% of peak, then take the min over the last 45%.
  local mincool=$(awk -F'\t' 'NR>1{v[++n]=$2} END{pk=0;for(i=1;i<=n;i++)if(v[i]>pk)pk=v[i]; last=n;while(last>1&&v[last]<0.5*pk)last--; st=int(last*0.55);if(st<1)st=1; m=1e18;for(i=st;i<=last;i++)if(v[i]>0&&v[i]<m)m=v[i]; printf "%.0f",m/1048576}' "$tsv" 2>/dev/null)
  echo "  $alloc: fill=${fill} get=${get} peak=${peak}MiB min_cool=${mincool}MiB avg=${avg}MiB"
  echo "$alloc,${fill},${get},${peak},${mincool},${avg}" >> "$OUT/redis_${VARIANT}.csv"
}

echo "=== redis $VARIANT : N=$N vsize=$VSIZE cool=${COOL}s ==="
for a in "$@"; do run_alloc "$a"; done
echo "--- redis_$VARIANT.csv ---"; column -t -s, "$OUT/redis_${VARIANT}.csv"
