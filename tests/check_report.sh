#!/bin/sh
# SMASH_REPORT + SMASH_METRICS_FILE end-to-end check (ctest: test_report,
# test_report_large_only). Runs tests/report_test.cpp under interposition and
# asserts on the exit report and the Prometheus metrics file.
#   check_report.sh <libsmash> <report_test> <SMASH_LARGE_ONLY> <workdir>
set -u
lib=$1 prog=$2 lo=$3 dir=$4
rm -rf "$dir" && mkdir -p "$dir" || exit 1
fail() { echo "FAIL: $*"; echo "--- report"; cat "$dir"/report.* 2>/dev/null; exit 1; }

if [ "$(uname -s)" = Darwin ]; then
    set -- env DYLD_INSERT_LIBRARIES="$lib" MallocNanoZone=0
else
    set -- env LD_PRELOAD="$lib"
fi
"$@" SMASH_LARGE_ONLY="$lo" SMASH_DEFER_PHASES_MS=0 SMASH_COLD_TIMEOUT_SEC=1 \
    SMASH_REPORT="$dir/report.%p" SMASH_METRICS_FILE="$dir/metrics.%p.prom" \
    SMASH_METRICS_INTERVAL_SEC=1 "$prog" || fail "workload exited $?"

report=$(ls "$dir"/report.* 2>/dev/null | head -n 1)
metrics=$(ls "$dir"/metrics.*.prom 2>/dev/null | head -n 1)
[ -n "$report" ] || fail "no report file (SMASH_REPORT=path with %p)"
[ -n "$metrics" ] || fail "no metrics file (SMASH_METRICS_FILE with %p)"
cat "$report"

want_mode=full; [ "$lo" = 1 ] && want_mode=large_only
grep -q "^\[smash report\] pid=.* mode=$want_mode " "$report" || fail "header/mode"
ncomp=$(sed -n 's/.*compressed over the run: \([0-9]*\) pages.*/\1/p' "$report")
[ "${ncomp:-0}" -gt 0 ] || fail "compressed over the run = ${ncomp:-?}"
nf=$(sed -n 's/.*decompression faults: \([0-9]*\).*/\1/p' "$report")
[ "${nf:-0}" -gt 0 ] || fail "decompression faults = ${nf:-?}"
grep -q 'service time p50 <= [0-9]* us, p99 ' "$report" || fail "latency quantiles"
grep -q 'RSS: peak ' "$report" || fail "RSS line"
grep -q 'hint: nothing was compressed' "$report" && fail "zero-compression hint on a compressing run"

# Metrics: every sample line is `name{labels} value`, comments are HELP/TYPE.
bad=$(grep -v -E '^# (HELP|TYPE) smash_[a-z_]+ ' "$metrics" |
      grep -v -E '^smash_[a-z_]+(\{[a-z_]+="[^"]*"(,[a-z_]+="[^"]*")*\})? [-+0-9.eE]+$|^smash_fault_service_seconds_bucket\{le="\+Inf"\} [0-9]+$')
[ -z "$bad" ] || fail "malformed metrics lines: $bad"
val() { sed -n "s/^$1 \([0-9.]*\)$/\1/p" "$metrics"; }
[ "$(val smash_compressions_total)" -gt 0 ] || fail "smash_compressions_total"
[ "$(val smash_fault_service_seconds_count)" -gt 0 ] || fail "histogram count"
inf=$(sed -n 's/^smash_fault_service_seconds_bucket{le="+Inf"} \([0-9]*\)$/\1/p' "$metrics")
[ "$inf" = "$(val smash_fault_service_seconds_count)" ] || fail "+Inf bucket != _count"
grep -q "^smash_config_large_only $lo\$" "$metrics" || fail "smash_config_large_only"
ls "$dir"/*.tmp.* >/dev/null 2>&1 && fail "temp file left behind (rename not atomic?)"
echo "PASS: report + metrics (SMASH_LARGE_ONLY=$lo)"
