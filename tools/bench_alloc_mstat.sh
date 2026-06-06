#!/usr/bin/env bash
# Rigorous allocator comparison on smash's own benchmarks, memory via mstat
# (cgroup v2 memory.current — shared pages counted once, true footprint).
# Runs a given workload binary under {glibc, jemalloc, mimalloc, smash} via
# LD_PRELOAD, N times each, and records mstat peak/avg + the bench's own
# METRIC lines (throughput, latency, RSS-reduction).
set -uo pipefail
SMASH_ROOT=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler/smash
ALLOC=$SMASH_ROOT/build/allocators/lib
SMASH_LIB=$SMASH_ROOT/build_bench2/libsmash.so
WORKLOAD=${WORKLOAD:?set WORKLOAD to the bench binary}
WL_ARGS=${WL_ARGS:-}
RUNS=${RUNS:-3}
LABEL=${LABEL:-$(basename "$WORKLOAD")}
OUT=${OUT:-$SMASH_ROOT/build_bench2/allocbench_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$OUT"
echo "alloc,run,status,wall_s,mstat_peak_mib,mstat_avg_mib,ops_per_sec,rss_reduction_pct,cool_reduction_pct" > "$OUT/$LABEL.csv"

# smash config for these benches: full mode, aggressive-ish cold timeout so
# cooling-phase compression actually fires within the bench window.
SMASH_ENV="SMASH_COLD_TIMEOUT_SEC=2 SMASH_DEFER_MADVISE=1"

run_one(){
  local alloc="$1" run="$2"
  local log="$OUT/${LABEL}_${alloc}_${run}.log" tsv="$OUT/${LABEL}_${alloc}_${run}.tsv"
  local pre=""
  case "$alloc" in
    glibc)    pre="";;
    jemalloc) pre="$ALLOC/libjemalloc.so";;
    mimalloc) pre="$ALLOC/libmimalloc.so";;
    smash)    pre="$SMASH_LIB";;
  esac
  ( [ -n "$pre" ] && export LD_PRELOAD="$pre"
    [ "$alloc" = "smash" ] && { for kv in $SMASH_ENV; do export "$kv"; done; }
    local st=$(date +%s)
    mstat -freq 20 -o "$tsv" -- "$WORKLOAD" $WL_ARGS > "$log" 2>&1
    echo $(($(date +%s)-st)) > "$OUT/.wall"
  )
  local wall=$(cat "$OUT/.wall" 2>/dev/null||echo 0)
  local status=PASS; grep -qiE "error|abort|segv|assert" "$log" 2>/dev/null && status=WARN
  local peak=$(awk -F'\t' 'NR>1{if($2>m)m=$2}END{printf "%.0f",m/1048576}' "$tsv" 2>/dev/null)
  local avg=$(awk -F'\t' 'NR>1&&$2>0{s+=$2;n++}END{if(n)printf "%.0f",s/n/1048576}' "$tsv" 2>/dev/null)
  local ops=$(awk '/METRIC ops_per_sec/{print $3}' "$log" 2>/dev/null|tail -1)
  local rssr=$(awk '/METRIC rss_reduction_pct/{print $3}' "$log" 2>/dev/null|tail -1)
  local coolr=$(awk '/METRIC cool_reduction_pct/{print $3}' "$log" 2>/dev/null|tail -1)
  echo "  $alloc run$run: peak=${peak}MiB avg=${avg}MiB ops=${ops:-NA} rss_red=${rssr:-NA}%"
  echo "$alloc,$run,$status,$wall,$peak,$avg,${ops:-},${rssr:-},${coolr:-}" >> "$OUT/$LABEL.csv"
}

echo "=== $LABEL : $WORKLOAD $WL_ARGS  (RUNS=$RUNS) ==="
for alloc in glibc jemalloc mimalloc smash; do
  echo "--- $alloc ---"
  for r in $(seq 1 $RUNS); do run_one "$alloc" "$r"; done
done
echo ""; echo "=== $LABEL summary (median over $RUNS runs) ==="
awk -F, 'NR>1{p[$1]=p[$1]" "$5; a[$1]=a[$1]" "$6; o[$1]=o[$1]" "$7; r[$1]=r[$1]" "$8}
END{ printf "%-10s %12s %12s %12s %10s\n","alloc","peak_mib","avg_mib","ops/sec","rss_red%";
 for(k in p){ n=split(p[k],pa," "); asort(pa); pm=pa[int((n+1)/2)];
   m=split(a[k],aa," "); asort(aa); am=aa[int((m+1)/2)];
   oo=split(o[k],oa," "); asort(oa); om=oa[int((oo+1)/2)];
   rr=split(r[k],ra," "); asort(ra); rm=ra[int((rr+1)/2)];
   printf "%-10s %12s %12s %12s %10s\n",k,pm,am,om,rm } }' "$OUT/$LABEL.csv" | sort
echo "OUT: $OUT"
