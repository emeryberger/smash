#!/usr/bin/env bash
# bench_mongodb.sh - MongoDB A/B comparison: system malloc vs Smash
#
# MongoDB 8.0 macOS ARM64 ships with allocator=system, so DYLD_INSERT_LIBRARIES
# can intercept all WiredTiger cache allocations. However, the official binary
# uses hardened runtime — we must re-sign with ad-hoc entitlements to allow
# DYLD injection.
#
# Known limitation: WiredTiger's 4 eviction threads continuously scan all cached
# pages, keeping them "warm" from Smash's perspective. This means Smash cannot
# identify cold pages via access monitoring. We set SMASH_NO_MONITOR=1 and rely
# on time-based cold detection + deferred phase activation.
#
# Additionally, Smash's kevent/mach_msg/recv/send interposers conflict with
# MongoDB's ASIO-based networking, causing connection timeouts once Phase 2/3
# activates. We use SMASH_DEFER_PHASES_MS to load data before compression starts.
#
# Requires: mongod binary (downloaded), mongosh in PATH
#
# Usage:
#   bash bench_mongodb.sh [--quick]
#
# Compatible with bash 3.2+ (macOS default).

set -euo pipefail

# ── Paths ───────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${REPO_DIR}/build"
SMASH_LIB="${BUILD_DIR}/libsmash.dylib"

# MongoDB binary location
MONGOD_DIR="${REPO_DIR}/tmp/mongodb-macos-aarch64-8.0.4"
MONGOD="${MONGOD_DIR}/bin/mongod"

# ── Platform detection ──────────────────────────────────────────────────────
if [[ "$(uname)" != "Darwin" ]]; then
    echo "SKIP: This benchmark requires macOS (DYLD interposition)"
    echo "Linux support requires building MongoDB with --allocator=system"
    exit 0
fi

PRELOAD_VAR="DYLD_INSERT_LIBRARIES"

# ── Parse flags ─────────────────────────────────────────────────────────────
NUM_DOCS=500000
VALUE_SIZE=512
COOL_DURATION=30
SERVE_DURATION=15
CACHE_SIZE_GB="0.5"

for arg in "$@"; do
    case "$arg" in
        --quick)
            NUM_DOCS=200000
            COOL_DURATION=20
            SERVE_DURATION=10
            ;;
        *) echo "Unknown flag: $arg"; exit 1 ;;
    esac
done

HOT_DOCS=$((NUM_DOCS / 20))   # top 5%
COLD_DOCS=$((NUM_DOCS / 10))  # bottom 10%

# Defer phases long enough for fill + serve to complete
DEFER_MS=$(( (COOL_DURATION + SERVE_DURATION + 30) * 1000 ))

# ── Check prerequisites ─────────────────────────────────────────────────────
if [[ ! -x "$MONGOD" ]]; then
    echo "SKIP: mongod not found at ${MONGOD}"
    echo "Download:"
    echo "  cd ${REPO_DIR}/tmp"
    echo "  curl -sL https://fastdl.mongodb.org/osx/mongodb-macos-arm64-8.0.4.tgz | tar xz"
    exit 0
fi

if ! command -v mongosh &>/dev/null; then
    echo "SKIP: mongosh not found in PATH"
    echo "Install: brew install mongosh"
    exit 0
fi

if [[ ! -f "${SMASH_LIB}" ]]; then
    echo "ERROR: libsmash not found at ${SMASH_LIB}"
    echo "Build with: cd build && cmake .. && make -j"
    exit 1
fi

# ── Re-sign mongod if needed ───────────────────────────────────────────────
# The official MongoDB binary has hardened runtime (flags=0x10000) which blocks
# DYLD_INSERT_LIBRARIES. Re-sign with ad-hoc + entitlements to permit injection.
ensure_mongod_signed() {
    local entitlements
    entitlements=$(codesign -d --entitlements - "$MONGOD" 2>&1 || true)
    if echo "$entitlements" | grep -q "allow-dyld-environment-variables"; then
        return 0  # Already properly signed
    fi

    echo "  Re-signing mongod with DYLD-permissive entitlements..."
    local ent_plist="${REPO_DIR}/tmp/mongod_entitlements.plist"
    cat > "$ent_plist" << 'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.security.cs.allow-unsigned-executable-memory</key>
    <true/>
    <key>com.apple.security.cs.disable-library-validation</key>
    <true/>
    <key>com.apple.security.cs.allow-dyld-environment-variables</key>
    <true/>
</dict>
</plist>
PLIST
    codesign --force --sign - --entitlements "$ent_plist" "$MONGOD"
    echo "  Done."
}

# ── Ports ───────────────────────────────────────────────────────────────────
BASELINE_PORT=27100
SMASH_PORT=27101

# ── Temp directory ──────────────────────────────────────────────────────────
TMPDIR_BENCH="${REPO_DIR}/tmp/bench_mongodb_$$"
mkdir -p "$TMPDIR_BENCH"
METRIC_FILE="${TMPDIR_BENCH}/metrics.txt"
: > "$METRIC_FILE"

# ── Cleanup trap ────────────────────────────────────────────────────────────
MONGOD_PID=""
MONITOR_PID=""

cleanup() {
    if [[ -n "$MONITOR_PID" ]]; then
        kill "$MONITOR_PID" 2>/dev/null || true
        wait "$MONITOR_PID" 2>/dev/null || true
    fi
    if [[ -n "$MONGOD_PID" ]]; then
        kill "$MONGOD_PID" 2>/dev/null || true
        wait "$MONGOD_PID" 2>/dev/null || true
    fi
    # Also kill by port in case PID tracking failed
    pkill -f "mongod.*--port ${BASELINE_PORT}" 2>/dev/null || true
    pkill -f "mongod.*--port ${SMASH_PORT}" 2>/dev/null || true
    rm -rf "$TMPDIR_BENCH"
}
trap cleanup EXIT

# ── Helper: get RSS via ps (KB → MB) ───────────────────────────────────────
get_rss_mb() {
    local pid="$1"
    local rss_kb
    rss_kb=$(ps -o rss= -p "$pid" 2>/dev/null | tr -d ' ')
    if [[ -n "$rss_kb" && "$rss_kb" -gt 0 ]] 2>/dev/null; then
        echo "scale=1; $rss_kb / 1024" | bc -l
    else
        echo "0"
    fi
}

# ── Helper: emit/lookup metrics ─────────────────────────────────────────────
emit_metric() {
    echo "METRIC $1 $2" >> "$METRIC_FILE"
}

metric_val() {
    local name="$1"
    awk -v m="$name" '$1=="METRIC" && $2==m {print $3}' "$METRIC_FILE"
}

# ── Helper: print comparison row ────────────────────────────────────────────
print_row() {
    local label="$1" base_metric="$2" smash_metric="$3"
    local base smash
    base=$(metric_val "$base_metric")
    smash=$(metric_val "$smash_metric")
    base="${base:-N/A}"
    smash="${smash:-N/A}"

    if [[ "$base" == "N/A" || "$smash" == "N/A" || "$base" == "0" ]]; then
        printf "  %-25s %10s MB %10s MB %10s\n" "$label" "$base" "$smash" "N/A"
        return
    fi

    local delta
    delta=$(echo "scale=1; (($smash - $base) / $base) * 100" | bc -l 2>/dev/null || echo "N/A")
    printf "  %-25s %10s MB %10s MB %9s%%\n" "$label" "$base" "$smash" "$delta"
}

# ── Helper: populate MongoDB ────────────────────────────────────────────────
populate_mongodb() {
    local port="$1"
    local num_docs="$2"

    echo "  Populating ${num_docs} documents on port ${port}..."

    mongosh --port "$port" --quiet --eval "
        const db = db.getSiblingDB('bench');
        db.data.drop();

        const batchSize = 10000;
        const numDocs = ${num_docs};
        const valueSize = ${VALUE_SIZE};

        function makeValue(id) {
            const base = JSON.stringify({
                id: id,
                name: 'user_' + id,
                email: 'user' + id + '@example.com',
                address: '123 Main St, City ' + (id % 100) + ', State ' + (id % 50),
                phone: '+1-555-' + String(id % 10000).padStart(4, '0'),
                notes: 'This is a note for user ' + id + '. It contains some repetitive text that should compress well. '
            });
            if (base.length >= valueSize) return base.substring(0, valueSize);
            return base + ' '.repeat(valueSize - base.length);
        }

        let inserted = 0;
        while (inserted < numDocs) {
            const batch = [];
            const end = Math.min(inserted + batchSize, numDocs);
            for (let i = inserted; i < end; i++) {
                batch.push({ _id: i, value: makeValue(i) });
            }
            db.data.insertMany(batch, { ordered: false });
            inserted = end;
            if (inserted % 100000 === 0) {
                print('    ' + inserted + '/' + numDocs + ' docs...');
            }
        }
        print('  Inserted ' + inserted + ' documents.');
    " 2>&1 | grep -v "^$"
}

# ── Helper: serve hot reads ─────────────────────────────────────────────────
serve_hot_reads() {
    local port="$1"
    local duration="$2"
    local hot_range="$3"
    local metric_label="$4"

    echo "  Serving hot reads for ${duration}s (top ${hot_range} docs)..."

    local total_ops
    total_ops=$(mongosh --port "$port" --quiet --eval "
        const db = db.getSiblingDB('bench');
        const duration = ${duration};
        const hotStart = ${NUM_DOCS} - ${hot_range};
        const hotEnd = ${NUM_DOCS};

        let ops = 0;
        const endTime = Date.now() + duration * 1000;
        while (Date.now() < endTime) {
            for (let i = 0; i < 100; i++) {
                const id = hotStart + Math.floor(Math.random() * (hotEnd - hotStart));
                db.data.findOne({ _id: id });
                ops++;
            }
        }
        print(ops);
    " 2>&1 | tail -1)

    if [[ "$duration" -gt 0 && -n "$total_ops" && "$total_ops" =~ ^[0-9]+$ ]]; then
        local ops_sec=$((total_ops / duration))
        emit_metric "${metric_label}_serve_ops" "$ops_sec"
        echo "  Hot reads: ${ops_sec} ops/s"
    fi
}

# ── Helper: access cold documents ───────────────────────────────────────────
access_cold_docs() {
    local port="$1"
    local cold_start="$2"
    local cold_count="$3"

    echo "  Accessing ${cold_count} cold documents starting at ${cold_start}..."

    mongosh --port "$port" --quiet --eval "
        const db = db.getSiblingDB('bench');
        const start = ${cold_start};
        const count = ${cold_count};

        let found = 0;
        for (let i = start; i < start + count; i++) {
            const doc = db.data.findOne({ _id: i });
            if (doc) found++;
        }
        print('  Found ' + found + '/' + count + ' cold documents.');
    " 2>&1 | grep -v "^$"
}

# ── Run one full pass ───────────────────────────────────────────────────────
run_mongodb_pass() {
    local label="$1"
    local port="$2"
    local use_smash="$3"

    echo ""
    echo "[${label}] Starting mongod on port ${port}..."

    local dbpath="${TMPDIR_BENCH}/${label}_db"
    mkdir -p "$dbpath"
    local logpath="${TMPDIR_BENCH}/${label}_mongod.log"

    local mongod_args=(
        --port "$port"
        --dbpath "$dbpath"
        --logpath "$logpath"
        --wiredTigerCacheSizeGB "$CACHE_SIZE_GB"
        --setParameter diagnosticDataCollectionEnabled=false
        --setParameter honorSystemUmask=true
        --fork
    )

    if [[ "$use_smash" == "yes" ]]; then
        env "${PRELOAD_VAR}=${SMASH_LIB}" \
            MallocNanoZone=0 \
            SMASH_NO_MONITOR=1 \
            SMASH_DEFER_PHASES_MS="${DEFER_MS}" \
            SMASH_COLD_TIMEOUT_SEC=5 \
            "$MONGOD" "${mongod_args[@]}" 2>&1 | grep -v "^$" || true
    else
        "$MONGOD" "${mongod_args[@]}" 2>&1 | grep -v "^$" || true
    fi

    sleep 3
    MONGOD_PID=$(pgrep -f "mongod.*--port ${port}" | head -1)

    if [[ -z "$MONGOD_PID" ]]; then
        echo "  mongod failed to start. Log:"
        tail -5 "$logpath" 2>/dev/null || true
        return 1
    fi

    # Verify connectivity
    local verify=""
    local attempt=0
    while [[ $attempt -lt 10 ]]; do
        verify=$(mongosh --port "$port" --quiet --eval "print('ok')" 2>/dev/null || true)
        if [[ "$verify" == "ok" ]]; then break; fi
        attempt=$((attempt + 1))
        sleep 1
    done

    if [[ "$verify" != "ok" ]]; then
        echo "  mongod not responding on port ${port}"
        kill "$MONGOD_PID" 2>/dev/null || true
        wait "$MONGOD_PID" 2>/dev/null || true
        MONGOD_PID=""
        return 1
    fi
    echo "  mongod running (pid ${MONGOD_PID})"

    # Phase 1: Fill
    echo "[${label}] Phase 1: Fill ${NUM_DOCS} documents..."
    local fill_start fill_end fill_sec fill_ops
    fill_start=$(date +%s)
    populate_mongodb "$port" "$NUM_DOCS"
    fill_end=$(date +%s)
    fill_sec=$((fill_end - fill_start))
    if [[ $fill_sec -lt 1 ]]; then fill_sec=1; fi
    fill_ops=$((NUM_DOCS / fill_sec))
    emit_metric "${label}_fill_sec" "$fill_sec"
    emit_metric "${label}_fill_ops" "$fill_ops"
    echo "  Fill: ${fill_sec}s (${fill_ops} inserts/s)"

    local fill_rss
    fill_rss=$(get_rss_mb "$MONGOD_PID")
    emit_metric "${label}_fill_rss_mb" "$fill_rss"
    echo "  Fill RSS: ${fill_rss} MB"

    # Start RSS monitor
    local rss_log="${TMPDIR_BENCH}/${label}_rss.log"
    : > "$rss_log"
    (
        while kill -0 "$MONGOD_PID" 2>/dev/null; do
            get_rss_mb "$MONGOD_PID" >> "$rss_log"
            sleep 1
        done
    ) &
    MONITOR_PID=$!

    # Phase 2: Cool
    echo "[${label}] Phase 2: Cool for ${COOL_DURATION}s..."
    local sec=1
    while [[ $sec -le $COOL_DURATION ]]; do
        sleep 1
        if [[ $((sec % 5)) -eq 0 ]] || [[ $sec -eq 1 ]]; then
            local rss
            rss=$(get_rss_mb "$MONGOD_PID")
            echo "    t=${sec}s: RSS=${rss} MB"
        fi
        sec=$((sec + 1))
    done

    local cool_rss
    cool_rss=$(get_rss_mb "$MONGOD_PID")
    emit_metric "${label}_cool_rss_mb" "$cool_rss"
    echo "  Post-cool RSS: ${cool_rss} MB"

    # Phase 3: Serve hot 5%
    echo "[${label}] Phase 3: Serve hot ${HOT_DOCS} docs for ${SERVE_DURATION}s..."
    serve_hot_reads "$port" "$SERVE_DURATION" "$HOT_DOCS" "$label"

    local serve_rss
    serve_rss=$(get_rss_mb "$MONGOD_PID")
    emit_metric "${label}_serve_rss_mb" "$serve_rss"
    echo "  Serve RSS: ${serve_rss} MB"

    # Phase 4: Cold re-access
    echo "[${label}] Phase 4: Access ${COLD_DOCS} cold documents..."
    local cold_start_t cold_end_t cold_sec cold_ops
    cold_start_t=$(date +%s)
    access_cold_docs "$port" 0 "$COLD_DOCS"
    cold_end_t=$(date +%s)
    cold_sec=$((cold_end_t - cold_start_t))
    if [[ $cold_sec -lt 1 ]]; then cold_sec=1; fi
    cold_ops=$((COLD_DOCS / cold_sec))
    emit_metric "${label}_cold_sec" "$cold_sec"
    emit_metric "${label}_cold_ops" "$cold_ops"
    echo "  Cold re-access: ${cold_sec}s (${cold_ops} reads/s)"

    local cold_rss
    cold_rss=$(get_rss_mb "$MONGOD_PID")
    emit_metric "${label}_cold_rss_mb" "$cold_rss"
    echo "  Cold re-access RSS: ${cold_rss} MB"

    # Stop monitor
    kill "$MONITOR_PID" 2>/dev/null || true
    wait "$MONITOR_PID" 2>/dev/null || true
    MONITOR_PID=""

    # Extract peak/min/steady from RSS log
    if [[ -s "$rss_log" ]]; then
        awk -v lbl="$label" '
        BEGIN { peak=0; min=999999; last=0; }
        {
            v = $1 + 0;
            if (v > peak) peak = v;
            if (v > 0 && v < min) min = v;
            last = v;
        }
        END {
            printf "METRIC %s_peak_rss_mb %s\n", lbl, peak;
            printf "METRIC %s_min_rss_mb %s\n", lbl, (min == 999999 ? 0 : min);
            printf "METRIC %s_steady_rss_mb %s\n", lbl, last;
        }' "$rss_log" >> "$METRIC_FILE"

        # AUC (MB*s)
        awk 'BEGIN {sum=0} {sum += $1 + 0} END {printf "METRIC '"$label"'_auc_mbs %.0f\n", sum}' "$rss_log" >> "$METRIC_FILE"
    fi

    # Stop mongod
    kill "$MONGOD_PID" 2>/dev/null || true
    wait "$MONGOD_PID" 2>/dev/null || true
    MONGOD_PID=""
    sleep 1
}

# ── Main ────────────────────────────────────────────────────────────────────
echo ""
echo "========================================================================"
echo "  MongoDB A/B Comparison: System Malloc vs Smash"
echo "========================================================================"
echo ""
echo "Docs: ${NUM_DOCS}, Value size: ${VALUE_SIZE}B, Cache: ${CACHE_SIZE_GB} GB"
echo "Cool: ${COOL_DURATION}s, Serve: ${SERVE_DURATION}s, Defer: ${DEFER_MS}ms"
echo ""

# Ensure mongod can accept DYLD injection
ensure_mongod_signed

# Run baseline (no Smash)
run_mongodb_pass "baseline" "$BASELINE_PORT" "no"

# Run Smash
SMASH_OK=1
run_mongodb_pass "smash" "$SMASH_PORT" "yes" || SMASH_OK=0

# ── Display results ─────────────────────────────────────────────────────────
echo ""
echo "  ════════════════════════════════════════════════════════════════════════"
printf "  %-25s %12s %12s %10s\n" "Metric" "Baseline" "Smash" "Delta"
echo "  ────────────────────────────────────────────────────────────────────────"

if [[ "$SMASH_OK" -eq 0 ]]; then
    echo "  NOTE: Smash run failed. Showing baseline only."
    for metric in fill_rss_mb peak_rss_mb cool_rss_mb serve_rss_mb min_rss_mb steady_rss_mb cold_rss_mb; do
        base=$(metric_val "baseline_${metric}")
        printf "  %-25s %10s MB %10s MB %10s\n" "${metric}" "${base:-N/A}" "N/A" "N/A"
    done
else
    print_row "Fill RSS"           "baseline_fill_rss_mb"    "smash_fill_rss_mb"
    print_row "Peak RSS"           "baseline_peak_rss_mb"    "smash_peak_rss_mb"
    print_row "Post-cool RSS"      "baseline_cool_rss_mb"    "smash_cool_rss_mb"
    print_row "Serve RSS"          "baseline_serve_rss_mb"   "smash_serve_rss_mb"
    print_row "Min RSS"            "baseline_min_rss_mb"     "smash_min_rss_mb"
    print_row "Steady RSS"         "baseline_steady_rss_mb"  "smash_steady_rss_mb"
    print_row "Cold re-access RSS" "baseline_cold_rss_mb"    "smash_cold_rss_mb"

    echo "  ────────────────────────────────────────────────────────────────────────"

    # AUC comparison
    base_auc=$(metric_val "baseline_auc_mbs")
    smash_auc=$(metric_val "smash_auc_mbs")
    if [[ -n "$base_auc" && -n "$smash_auc" && "$base_auc" != "0" ]]; then
        auc_delta=$(echo "scale=1; (($smash_auc - $base_auc) * 100) / $base_auc" | bc -l 2>/dev/null || echo "N/A")
        printf "  %-25s %10s    %10s    %9s%%\n" "AUC (MB*s)" "$base_auc" "$smash_auc" "$auc_delta"
    fi

    # Throughput
    echo ""
    echo "  Throughput:"
    echo "  ────────────────────────────────────────────────────────────────────────"
    base_fill=$(metric_val "baseline_fill_ops")
    smash_fill=$(metric_val "smash_fill_ops")
    base_serve=$(metric_val "baseline_serve_ops")
    smash_serve=$(metric_val "smash_serve_ops")
    base_cold=$(metric_val "baseline_cold_ops")
    smash_cold=$(metric_val "smash_cold_ops")

    printf "  %-25s %10s    %10s\n" "Fill (inserts/s)" "${base_fill:-N/A}" "${smash_fill:-N/A}"
    printf "  %-25s %10s    %10s\n" "Serve hot (ops/s)" "${base_serve:-N/A}" "${smash_serve:-N/A}"
    printf "  %-25s %10s    %10s\n" "Cold read (ops/s)" "${base_cold:-N/A}" "${smash_cold:-N/A}"
fi

echo ""
echo "  ════════════════════════════════════════════════════════════════════════"
echo ""
echo "NOTE: WiredTiger runs 4 eviction threads that continuously scan cached"
echo "pages, preventing Smash from identifying cold pages for compression."
echo "Effective compression requires either:"
echo "  1. A dataset larger than WiredTiger's cache (forcing eviction → cold pages)"
echo "  2. Modifying WiredTiger to expose page-access hooks to Smash"
echo ""
echo "Done."
