#!/usr/bin/env bash
# run_linux_experiments.sh -- Build smash and run all paper experiments on Linux.
#
# Designed for a fresh Ubuntu 22.04/24.04 EC2 instance.
# Idempotent: safe to re-run (skips already-installed deps, already-cloned repos).
#
# Usage:
#   bash bench/run_linux_experiments.sh          # full run (3 runs each)
#   bash bench/run_linux_experiments.sh --quick   # smoke test (1 run, smaller datasets)
#
# Output: paper_results/linux/  (ablation, compress-only, value-add)
set -euo pipefail

# ── Configuration ──────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SMASH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${SMASH_DIR}/build"
RESULTS_DIR="${SMASH_DIR}/paper_results/linux"

RUNS=3
QUICK=""
MC_KEYS=500000
COOL=30
SERVE=20

for arg in "$@"; do
    case "$arg" in
        --quick) QUICK="--quick"; RUNS=1; MC_KEYS=200000; COOL=15; SERVE=10 ;;
        --runs=*) RUNS="${arg#--runs=}" ;;
        *) echo "Unknown argument: $arg"; exit 1 ;;
    esac
done

NCPU=$(nproc 2>/dev/null || echo 4)

# Allocator install paths (built from source below)
JEMALLOC_PREFIX="/usr/local"
JEMALLOC_LIB="${JEMALLOC_PREFIX}/lib/libjemalloc.so"
MIMALLOC_PREFIX="/usr/local"
# mimalloc v3 installs to lib/libmimalloc.so; v2 installs to lib/mimalloc-2.1/libmimalloc.so
MIMALLOC_LIB="${MIMALLOC_PREFIX}/lib/libmimalloc.so"

# Smash libraries (built below)
SMASH_LIB="${BUILD_DIR}/libsmash.so"
SMASH_CO_LIB="${BUILD_DIR}/libsmash_compress_only.so"
SMASH_NOOPT_LIB="${BUILD_DIR}/libsmash_noopt.so"

BENCH_SQLITE="${BUILD_DIR}/bench/bench_sqlite"
BENCH_DEPS_BIN="${BUILD_DIR}/bench/deps/bin"
MEMCACHED="${BENCH_DEPS_BIN}/memcached"

log()  { echo ">>> [$(date '+%H:%M:%S')] $*"; }
fail() { echo "ERROR: $*" >&2; exit 1; }

# ══════════════════════════════════════════════════════════════════════════════
# Phase 1: Install system dependencies
# ══════════════════════════════════════════════════════════════════════════════

install_system_deps() {
    log "Installing system dependencies via apt..."
    sudo apt-get update -qq
    sudo apt-get install -y -qq \
        build-essential cmake git python3 python3-pip \
        liblz4-dev libzstd-dev \
        libevent-dev libssl-dev \
        autoconf automake libtool pkg-config \
        bc netcat-openbsd iproute2 \
        libsnappy-dev libgflags-dev \
        2>&1 | tail -1
    log "System dependencies installed."
}

# ══════════════════════════════════════════════════════════════════════════════
# Phase 2: Build jemalloc 5.3.0 and mimalloc 3.3.0 from source
# ══════════════════════════════════════════════════════════════════════════════

build_jemalloc() {
    if [[ -f "$JEMALLOC_LIB" ]]; then
        log "jemalloc already installed at $JEMALLOC_LIB -- skipping."
        return
    fi
    log "Building jemalloc 5.3.0 from source..."
    local tmpdir
    tmpdir=$(mktemp -d)
    (
        cd "$tmpdir"
        git clone --depth 1 --branch 5.3.0 https://github.com/jemalloc/jemalloc.git
        cd jemalloc
        ./autogen.sh --prefix="${JEMALLOC_PREFIX}"
        make -j"${NCPU}"
        sudo make install
    )
    rm -rf "$tmpdir"
    sudo ldconfig
    log "jemalloc 5.3.0 installed to ${JEMALLOC_PREFIX}."
}

build_mimalloc() {
    if [[ -f "$MIMALLOC_LIB" ]]; then
        log "mimalloc already installed at $MIMALLOC_LIB -- skipping."
        return
    fi
    log "Building mimalloc from source..."
    local tmpdir
    tmpdir=$(mktemp -d)
    (
        cd "$tmpdir"
        git clone --depth 1 --branch v3.3.0 https://github.com/microsoft/mimalloc.git 2>/dev/null \
            || git clone --depth 1 --branch v2.1.9 https://github.com/microsoft/mimalloc.git
        cd mimalloc
        mkdir -p build && cd build
        cmake .. -DCMAKE_BUILD_TYPE=Release \
                 -DCMAKE_INSTALL_PREFIX="${MIMALLOC_PREFIX}" \
                 -DMI_BUILD_TESTS=OFF \
                 -DMI_BUILD_SHARED=ON \
                 -DMI_BUILD_OBJECT=OFF
        make -j"${NCPU}"
        sudo make install
    )
    rm -rf "$tmpdir"
    sudo ldconfig
    # Find the actual .so if the default path doesn't exist
    if [[ ! -f "$MIMALLOC_LIB" ]]; then
        local found
        found=$(find "${MIMALLOC_PREFIX}/lib" -name 'libmimalloc.so*' -type f 2>/dev/null | head -1)
        if [[ -n "$found" ]]; then
            sudo ln -sf "$found" "$MIMALLOC_LIB"
        fi
    fi
    log "mimalloc installed to ${MIMALLOC_PREFIX}."
}

# ══════════════════════════════════════════════════════════════════════════════
# Phase 3: Clone alloc8 if needed, then build smash
# ══════════════════════════════════════════════════════════════════════════════

clone_alloc8() {
    local alloc8_dir="${SMASH_DIR}/../alloc8"
    if [[ -f "${alloc8_dir}/CMakeLists.txt" ]]; then
        log "alloc8 already present at ${alloc8_dir} -- skipping clone."
        return
    fi
    log "Cloning alloc8..."
    git clone https://github.com/emeryberger/alloc8.git "${alloc8_dir}"
    log "alloc8 cloned."
}

build_smash() {
    log "Building smash (with benchmarks)..."
    mkdir -p "$BUILD_DIR"
    (
        cd "$BUILD_DIR"
        cmake "$SMASH_DIR" \
            -DSMASH_BUILD_BENCH=ON \
            -DCMAKE_BUILD_TYPE=Release
        make -j"${NCPU}"
    )

    # Verify critical artifacts exist
    for lib in "$SMASH_LIB" "$SMASH_CO_LIB" "$SMASH_NOOPT_LIB"; do
        [[ -f "$lib" ]] || fail "Expected library not built: $lib"
    done
    [[ -f "$BENCH_SQLITE" ]] || fail "bench_sqlite not built"
    log "smash build complete."
}

build_bench_deps() {
    log "Building benchmark dependencies (Redis, Redis-Smash, Memcached, RocksDB, DuckDB)..."
    log "This may take 15-30 minutes on first run."
    (
        cd "$BUILD_DIR"
        make bench_deps -j"${NCPU}" 2>&1 || true
    )
    # Check what we got
    for bin in redis-server-libc redis-server-smash redis-cli memcached duckdb; do
        if [[ -f "${BENCH_DEPS_BIN}/${bin}" ]]; then
            log "  OK: ${bin}"
        else
            log "  MISSING: ${bin} (some experiments may be skipped)"
        fi
    done
}

# ══════════════════════════════════════════════════════════════════════════════
# Phase 4: Run experiments
# ══════════════════════════════════════════════════════════════════════════════

run_ablation_and_compress_only() {
    log "Running ablation + compress-only experiments (${RUNS} runs)..."
    mkdir -p "$RESULTS_DIR"
    (
        cd "$BUILD_DIR"
        python3 "${SMASH_DIR}/bench/run_paper_experiments.py" \
            --runs "$RUNS" \
            --build-dir "$BUILD_DIR" \
            ${QUICK}
    )
    log "Ablation + compress-only experiments complete."
}

# ── Value-add multi-allocator experiment ──────────────────────────────────────
# Adapted from build/bench_value_add.sh for Linux (LD_PRELOAD, .so suffixes).

get_rss_mb() {
    local rss_kb
    rss_kb=$(ps -o rss= -p "$1" 2>/dev/null | tr -d ' ')
    if [[ -n "$rss_kb" && "$rss_kb" -gt 0 ]] 2>/dev/null; then
        echo "scale=1; $rss_kb / 1024" | bc -l
    else
        echo "0"
    fi
}

mc_send() {
    { cat; printf "quit\r\n"; } | nc -w 1 localhost "$1" >/dev/null 2>&1 || true
}

populate_mc() {
    local port="$1" corpus="$2" batch=10000
    head -n "$MC_KEYS" "$corpus" | awk '
    { printf "set key:%07d 0 0 %d\r\n%s\r\n", NR-1, length($0), $0; }
    ' > "${VA_TMP}/cmds.txt"
    mkdir -p "${VA_TMP}/batches"
    split -l "$batch" "${VA_TMP}/cmds.txt" "${VA_TMP}/batches/b_"
    for c in "${VA_TMP}"/batches/b_*; do mc_send "$port" < "$c"; done
    rm -rf "${VA_TMP}/batches" "${VA_TMP}/cmds.txt"
}

generate_corpus() {
    local duckdb_bin="$1" corpus_file="$2" sf="${3:-0.5}"
    if [[ -f "$corpus_file" ]]; then return; fi
    log "  Generating TPC-H corpus (sf=${sf})..."
    "$duckdb_bin" -c "
INSTALL tpch; LOAD tpch; INSTALL json; LOAD json;
CALL dbgen(sf=${sf});
COPY (
    SELECT o_orderkey as id, c_name as customer, c_address as addr,
           c_phone as phone, c_mktsegment as segment,
           o_orderstatus as status, o_totalprice as total,
           o_orderdate as date, o_orderpriority as priority,
           o_comment as order_note, c_comment as customer_note
    FROM orders JOIN customer ON o_custkey = c_custkey
    ORDER BY o_orderkey
) TO '${corpus_file}' (FORMAT JSON, ARRAY false);
"
}

run_mc_test() {
    local label="$1" port="$2" libs="$3" corpus="$4"
    local mc_opts="-p $port -m 1024 -t 1 -l 127.0.0.1 -o no_lru_crawler,no_lru_maintainer"

    if [[ -n "$libs" ]]; then
        LD_PRELOAD="$libs" "$MEMCACHED" $mc_opts &>/dev/null &
    else
        "$MEMCACHED" $mc_opts &>/dev/null &
    fi
    local pid=$!
    sleep 3
    if ! kill -0 "$pid" 2>/dev/null; then echo "FAIL 0 0"; return; fi

    populate_mc "$port" "$corpus"
    local fill
    fill=$(get_rss_mb "$pid")

    local rss_log="${VA_TMP}/${label}_${port}.log"
    : > "$rss_log"
    ( while kill -0 "$pid" 2>/dev/null; do get_rss_mb "$pid" >> "$rss_log"; sleep 1; done ) &
    local mon=$!

    sleep "$COOL"
    local cool
    cool=$(get_rss_mb "$pid")
    kill "$mon" 2>/dev/null; wait "$mon" 2>/dev/null || true

    local min_rss
    min_rss=$(awk 'BEGIN{m=999999}{v=$1+0; if(v>0 && v<m) m=v}END{print (m==999999?"0":m)}' "$rss_log")

    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null || true
    sleep 1
    echo "$fill $cool $min_rss"
}

run_sqlite_test() {
    local label="$1" libs="$2"
    local output
    if [[ -n "$libs" ]]; then
        output=$(LD_PRELOAD="$libs" "$BENCH_SQLITE" --quick 2>&1) || output="FAIL"
    else
        output=$("$BENCH_SQLITE" --quick 2>&1) || output="FAIL"
    fi
    if [[ "$output" == "FAIL" ]]; then echo "FAIL 0 0 0"; return; fi
    local peak cool min_rss rss_red
    peak=$(echo "$output" | awk '/^METRIC peak_rss_mb/{print $3}')
    cool=$(echo "$output" | awk '/^METRIC post_cool_rss_mb/{print $3}')
    min_rss=$(echo "$output" | awk '/^METRIC min_rss_mb/{print $3}')
    rss_red=$(echo "$output" | awk '/^METRIC rss_reduction_pct/{print $3}')
    echo "${peak:-0} ${cool:-0} ${min_rss:-0} ${rss_red:-0}"
}

run_value_add() {
    log "Running value-add multi-allocator experiment (${RUNS} runs)..."

    # Check prerequisites
    if [[ ! -f "$MEMCACHED" ]]; then
        log "WARNING: memcached not found at $MEMCACHED -- skipping memcached value-add."
    fi
    if [[ ! -f "$BENCH_SQLITE" ]]; then
        log "WARNING: bench_sqlite not found -- skipping SQLite value-add."
        return
    fi

    # Check for allocator libraries
    if [[ ! -f "$JEMALLOC_LIB" ]]; then
        log "WARNING: jemalloc not found at $JEMALLOC_LIB -- skipping jemalloc configs."
    fi
    if [[ ! -f "$MIMALLOC_LIB" ]]; then
        log "WARNING: mimalloc not found at $MIMALLOC_LIB -- skipping mimalloc configs."
    fi

    VA_TMP=$(mktemp -d)
    trap "rm -rf $VA_TMP" EXIT

    # Generate corpus for memcached (needs duckdb)
    local duckdb_bin="${BENCH_DEPS_BIN}/duckdb"
    local corpus="${VA_TMP}/corpus.jsonl"
    local have_corpus=false
    if [[ -f "$duckdb_bin" && -f "$MEMCACHED" ]]; then
        local sf=0.5
        [[ -n "$QUICK" ]] && sf=0.3
        generate_corpus "$duckdb_bin" "$corpus" "$sf"
        have_corpus=true
    fi

    # Build config list: label|mc_libs|sq_libs
    # On Linux, concatenate preload libs with colon
    declare -a CONFIGS=(
        "system_malloc||"
    )
    if [[ -f "$JEMALLOC_LIB" ]]; then
        CONFIGS+=("jemalloc|${JEMALLOC_LIB}|${JEMALLOC_LIB}")
    fi
    if [[ -f "$MIMALLOC_LIB" ]]; then
        CONFIGS+=("mimalloc|${MIMALLOC_LIB}|${MIMALLOC_LIB}")
    fi
    CONFIGS+=("sys+compress|${SMASH_CO_LIB}|${SMASH_CO_LIB}")
    if [[ -f "$JEMALLOC_LIB" ]]; then
        CONFIGS+=("je+compress|${JEMALLOC_LIB}:${SMASH_CO_LIB}|${JEMALLOC_LIB}:${SMASH_CO_LIB}")
    fi
    if [[ -f "$MIMALLOC_LIB" ]]; then
        CONFIGS+=("mi+compress|${MIMALLOC_LIB}:${SMASH_CO_LIB}|${MIMALLOC_LIB}:${SMASH_CO_LIB}")
    fi
    CONFIGS+=("smash_noopt|${SMASH_NOOPT_LIB}|${SMASH_NOOPT_LIB}")
    CONFIGS+=("full_smash|${SMASH_LIB}|${SMASH_LIB}")

    mkdir -p "$RESULTS_DIR"
    local MC_CSV="${RESULTS_DIR}/value_add_memcached.csv"
    local SQ_CSV="${RESULTS_DIR}/value_add_sqlite.csv"
    echo "config,run,fill_rss,cool_rss,min_rss" > "$MC_CSV"
    echo "config,run,peak_rss,cool_rss,min_rss,rss_reduction" > "$SQ_CSV"

    local PORT=11500
    for cfg in "${CONFIGS[@]}"; do
        IFS='|' read -r label mc_libs sq_libs <<< "$cfg"
        log "  Config: ${label}"

        for run in $(seq 1 "$RUNS"); do
            # Memcached
            if [[ -f "$MEMCACHED" && "$have_corpus" == true ]]; then
                local port=$PORT
                PORT=$((PORT + 1))
                local mc_result
                mc_result=$(run_mc_test "$label" "$port" "$mc_libs" "$corpus")
                read -r mc_fill mc_cool mc_min <<< "$mc_result"
                echo "    MC run${run}: fill=${mc_fill} cool=${mc_cool} min=${mc_min}"
                echo "${label},${run},${mc_fill},${mc_cool},${mc_min}" >> "$MC_CSV"
            fi

            # SQLite -- skip bare jemalloc/mimalloc (crash with static SQLite)
            if [[ "$label" == "jemalloc" || "$label" == "mimalloc" ]]; then
                echo "    SQ run${run}: SKIP (allocator crash with static SQLite)"
                echo "${label},${run},SKIP,SKIP,SKIP,SKIP" >> "$SQ_CSV"
            else
                local sq_result
                sq_result=$(run_sqlite_test "$label" "$sq_libs")
                read -r sq_peak sq_cool sq_min sq_red <<< "$sq_result"
                echo "    SQ run${run}: peak=${sq_peak} cool=${sq_cool} min=${sq_min} red=${sq_red}%"
                echo "${label},${run},${sq_peak},${sq_cool},${sq_min},${sq_red}" >> "$SQ_CSV"
            fi
        done
    done

    # Print summary tables
    echo ""
    echo "================================================================"
    echo "  Value-Add: Memcached Results (averaged)"
    echo "================================================================"
    printf "%-18s %9s %9s %9s %9s\n" "Config" "Fill RSS" "Cool RSS" "Min RSS" "Reduct."
    printf "%-18s %9s %9s %9s %9s\n" "------" "--------" "--------" "-------" "-------"
    python3 -c "
import csv, collections
rows = [r for r in csv.DictReader(open('$MC_CSV')) if r['fill_rss'] != 'FAIL']
groups = collections.OrderedDict()
for r in rows:
    c = r['config']; groups.setdefault(c, []).append(r)
for c, runs in groups.items():
    n = len(runs)
    fill = sum(float(r['fill_rss']) for r in runs) / n
    cool = sum(float(r['cool_rss']) for r in runs) / n
    mn = sum(float(r['min_rss']) for r in runs) / n
    red = (fill - mn) / fill * 100 if fill > 0 else 0
    print(f'{c:<18s} {fill:8.1f}M {cool:8.1f}M {mn:8.1f}M {red:7.1f}%')
" 2>/dev/null || log "  (no memcached data to summarize)"

    echo ""
    echo "================================================================"
    echo "  Value-Add: SQLite Results (averaged)"
    echo "================================================================"
    printf "%-18s %9s %9s %9s %9s\n" "Config" "Peak RSS" "Cool RSS" "Min RSS" "Reduct."
    printf "%-18s %9s %9s %9s %9s\n" "------" "--------" "--------" "-------" "-------"
    python3 -c "
import csv, collections
rows = [r for r in csv.DictReader(open('$SQ_CSV')) if r['peak_rss'] not in ('FAIL','SKIP')]
groups = collections.OrderedDict()
for r in rows:
    c = r['config']; groups.setdefault(c, []).append(r)
for c, runs in groups.items():
    n = len(runs)
    peak = sum(float(r['peak_rss']) for r in runs) / n
    cool = sum(float(r['cool_rss']) for r in runs) / n
    mn = sum(float(r['min_rss']) for r in runs) / n
    red = sum(float(r['rss_reduction']) for r in runs) / n
    print(f'{c:<18s} {peak:8.1f}M {cool:8.1f}M {mn:8.1f}M {red:7.1f}%')
" 2>/dev/null || log "  (no SQLite data to summarize)"

    log "Value-add results saved to ${RESULTS_DIR}/value_add_{memcached,sqlite}.csv"
}

# ══════════════════════════════════════════════════════════════════════════════
# Main
# ══════════════════════════════════════════════════════════════════════════════

main() {
    log "============================================================"
    log "  Smash Linux Experiment Runner"
    log "  Runs: ${RUNS}  Quick: ${QUICK:-no}"
    log "  Source: ${SMASH_DIR}"
    log "  Build:  ${BUILD_DIR}"
    log "  Output: ${RESULTS_DIR}"
    log "============================================================"
    echo ""

    # Verify we are on Linux
    if [[ "$(uname -s)" != "Linux" ]]; then
        fail "This script is designed for Linux. Detected: $(uname -s)"
    fi

    # Phase 1: System deps
    install_system_deps

    # Phase 2: Build allocators from source
    build_jemalloc
    build_mimalloc

    # Phase 3: Build smash + bench deps
    clone_alloc8
    build_smash
    build_bench_deps

    # Phase 4: Run experiments
    mkdir -p "$RESULTS_DIR"

    run_ablation_and_compress_only
    run_value_add

    # Final summary
    echo ""
    log "============================================================"
    log "  ALL EXPERIMENTS COMPLETE"
    log "============================================================"
    log "Results directory: ${RESULTS_DIR}"
    echo ""
    echo "Files:"
    ls -lh "$RESULTS_DIR"/ 2>/dev/null || true
    echo ""
    log "Done."
}

main "$@"
