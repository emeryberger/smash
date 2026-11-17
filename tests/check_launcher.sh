#!/bin/sh
# bin/smash end-to-end check (ctest: test_launcher, test_launcher_large_only).
#   check_launcher.sh <bin/smash> <launcher_test> <SMASH_LARGE_ONLY> <workdir>
# The launcher must find libsmash relative to itself (no SMASH_LIB), preload
# it, pass --report/--large-only through, refuse `stats` on a process that
# cannot take SIGUSR2, and deliver it when --stats-signal was given.
set -u
smash=$1 prog=$2 lo=$3 dir=$4
rm -rf "$dir" && mkdir -p "$dir" || exit 1
fail() { echo "FAIL: $*"; for f in "$dir"/*; do echo "--- $f"; cat "$f"; done; exit 1; }
unset SMASH_LIB SMASH_LARGE_ONLY SMASH_REPORT SMASH_PROFILE SMASH_COLD_TIMEOUT_SEC
[ "$(uname -s)" = Darwin ] && { MallocNanoZone=0; export MallocNanoZone; }

"$smash" version > "$dir/version" 2>&1 || fail "smash version"
grep -q '^library: /' "$dir/version" || fail "version did not locate libsmash"

opt=; want_mode=full
[ "$lo" = 1 ] && { opt=--large-only; want_mode=large_only; }
"$smash" run $opt --profile balanced --report -- "$prog" > "$dir/out" 2> "$dir/err" || fail "smash run exited $?"
grep -q 'launcher_test: smash loaded' "$dir/out" || fail "libsmash not preloaded"
grep -q "^\[smash report\] pid=.* mode=$want_mode " "$dir/err" || fail "no report / wrong mode"
# A sub-second program under the default 10 s cold timeout compresses
# nothing; the report must say why.
grep -q 'hint: nothing was compressed because' "$dir/err" || fail "missing zero-compression hint"

"$smash" run --profile bogus -- "$prog" > /dev/null 2>&1 && fail "accepted --profile bogus"

# stats: refuse without SMASH_STATS_SIGNAL (SIGUSR2 would kill the target)...
"$smash" run $opt -- "$prog" wait 4 > /dev/null 2> "$dir/err2" & pid=$!
sleep 1
"$smash" stats $pid > "$dir/stats-refuse" 2>&1 && fail "stats did not refuse"
kill -0 $pid 2>/dev/null || fail "stats killed the target"
wait $pid || fail "target exited $? after refused stats"
# ...and deliver with --stats-signal.
"$smash" run $opt --stats-signal -- "$prog" wait 4 > /dev/null 2> "$dir/err3" & pid=$!
sleep 2
"$smash" stats $pid > "$dir/stats-ok" 2>&1 || fail "stats failed with --stats-signal"
wait $pid || fail "target exited $? after SIGUSR2"
grep -q '\[smash stats\]' "$dir/err3" || fail "no stats line from SIGUSR2"

if [ "$(uname -s)" = Darwin ]; then
    "$smash" run -- /usr/bin/true 2> "$dir/sip" || true
    grep -q 'SIP-protected' "$dir/sip" || fail "no SIP warning for /usr/bin/true"
fi
echo "PASS: launcher (SMASH_LARGE_ONLY=$lo)"
