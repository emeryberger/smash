#!/usr/bin/env bash
# Paper-faithful app benchmark: capture the bench's own phase RSS (peak/cool/
# serve/min via getrusage) — the paper's metric — plus mstat cgroup trough as a
# cross-check, across glibc/jemalloc/mimalloc/smash. Smash env is set per the
# paper's run_paper_experiments.py for each workload.
set -uo pipefail
SROOT=/local/home/emerydb/new-kaena/KaenaCompilerContainerBuild/src/KaenaCompiler/smash
ALLOC=$SROOT/build/allocators/lib
SMASH_LIB=$SROOT/build_bench2/libsmash.so
WORKLOAD=${WORKLOAD:?}; WL_ARGS=${WL_ARGS:-}; SMASH_ENV=${SMASH_ENV:-}
RUNS=${RUNS:-2}; LABEL=${LABEL:?}; OUT=${OUT:?}
mkdir -p "$OUT"
echo "alloc,run,peak_rss,cool_rss,serve_rss,min_rss,ops,rss_red_pct,mstat_peak,mstat_min" > "$OUT/$LABEL.csv"
run1(){
  local alloc="$1" run="$2" pre="" env=""
  case "$alloc" in
    glibc) ;; jemalloc) pre="$ALLOC/libjemalloc.so";;
    mimalloc) pre="$ALLOC/libmimalloc.so";; smash) pre="$SMASH_LIB"; env="$SMASH_ENV";;
  esac
  local log="$OUT/${LABEL}_${alloc}_${run}.log" tsv="$OUT/${LABEL}_${alloc}_${run}.tsv"
  ( [ -n "$pre" ] && export LD_PRELOAD="$pre"; for kv in $env; do export "$kv"; done
    mstat -freq 10 -o "$tsv" -- "$WORKLOAD" $WL_ARGS ) > "$log" 2>&1
  local pk=$(awk '/METRIC peak_rss_mb/{print $3}' "$log"|tail -1)
  local cl=$(awk '/METRIC cool_rss_mb/{print $3}' "$log"|tail -1)
  local sv=$(awk '/METRIC serve_rss_mb/{print $3}' "$log"|tail -1)
  local mn=$(awk '/METRIC min_rss_mb/{print $3}' "$log"|tail -1)
  local op=$(awk '/METRIC ops_per_sec/{print $3}' "$log"|tail -1)
  local rr=$(awk '/METRIC rss_reduction_pct/{print $3}' "$log"|tail -1)
  local mpk=$(awk -F'\t' 'NR>1{if($2>m)m=$2}END{printf "%.0f",m/1048576}' "$tsv" 2>/dev/null)
  local mmin=$(awk -F'\t' 'NR>1{v[NR]=$2;n=NR}END{st=int(n*0.5);m=1e18;for(i=st;i<=n;i++)if(v[i]>0&&v[i]<m)m=v[i];printf "%.0f",m/1048576}' "$tsv" 2>/dev/null)
  echo "  $alloc r$run: peak=$pk cool=$cl serve=$sv min=$mn ops=$op red=${rr}% | mstat peak=$mpk min=$mmin"
  echo "$alloc,$run,$pk,$cl,$sv,$mn,$op,$rr,$mpk,$mmin" >> "$OUT/$LABEL.csv"
}
echo "=== $LABEL: $WORKLOAD $WL_ARGS | smash_env=[$SMASH_ENV] ==="
for a in glibc jemalloc mimalloc smash; do echo "--- $a ---"; for r in $(seq 1 $RUNS); do run1 "$a" "$r"; done; done
echo "--- $LABEL.csv ---"; column -t -s, "$OUT/$LABEL.csv"
