#!/bin/bash
# Install dependencies for Smash paper experiments
# Usage: ./scripts/install_bench_deps.sh [--all|--rocksdb|--duckdb|--memcached|--redis] [--local]
#
# --local: Install from source to ~/.local (no sudo required)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
LOCAL_PREFIX="$HOME/.local"
LOCAL_INSTALL=false

# Detect OS and package manager
detect_pkg_mgr() {
    if [[ "$OSTYPE" == "linux-gnu"* ]]; then
        if command -v apt-get &> /dev/null; then
            PKG_MGR="apt"
        elif command -v yum &> /dev/null; then
            PKG_MGR="yum"
        elif command -v dnf &> /dev/null; then
            PKG_MGR="dnf"
        else
            PKG_MGR="none"
        fi
    elif [[ "$OSTYPE" == "darwin"* ]]; then
        PKG_MGR="brew"
    else
        PKG_MGR="none"
    fi
}

# ── Redis ────────────────────────────────────────────────────────────────────

install_redis_local() {
    echo "=== Installing Redis from source ==="
    REDIS_VERSION="7.2.4"
    TMPDIR=$(mktemp -d)
    cd "$TMPDIR"

    curl -sL "https://github.com/redis/redis/archive/refs/tags/${REDIS_VERSION}.tar.gz" | tar xz
    cd redis-${REDIS_VERSION}
    make -j$(nproc)

    mkdir -p "$LOCAL_PREFIX/bin"
    cp src/redis-server src/redis-cli src/redis-benchmark "$LOCAL_PREFIX/bin/"

    cd /
    rm -rf "$TMPDIR"
    echo "Redis installed to $LOCAL_PREFIX/bin/"
    "$LOCAL_PREFIX/bin/redis-server" --version
}

install_redis() {
    if $LOCAL_INSTALL; then
        install_redis_local
        return
    fi

    echo "=== Installing Redis ==="
    if [[ "$PKG_MGR" == "brew" ]]; then
        brew install redis
    elif [[ "$PKG_MGR" == "apt" ]]; then
        sudo apt-get update
        sudo apt-get install -y redis-server redis-tools
    elif [[ "$PKG_MGR" == "yum" ]] || [[ "$PKG_MGR" == "dnf" ]]; then
        sudo $PKG_MGR install -y redis || install_redis_local
    else
        install_redis_local
    fi
}

# ── Memcached ────────────────────────────────────────────────────────────────

install_libevent_local() {
    echo "Installing libevent..."
    LIBEVENT_VERSION="2.1.12-stable"
    TMPDIR=$(mktemp -d)
    cd "$TMPDIR"

    curl -sL "https://github.com/libevent/libevent/releases/download/release-${LIBEVENT_VERSION}/libevent-${LIBEVENT_VERSION}.tar.gz" | tar xz
    cd libevent-${LIBEVENT_VERSION}
    ./configure --prefix="$LOCAL_PREFIX" --disable-openssl
    make -j$(nproc)
    make install

    cd /
    rm -rf "$TMPDIR"
}

install_memcached_local() {
    echo "=== Installing Memcached from source ==="

    # Check if libevent is installed locally
    if [[ ! -f "$LOCAL_PREFIX/lib/libevent.so" ]] && [[ ! -f "$LOCAL_PREFIX/lib/libevent.a" ]]; then
        install_libevent_local
    fi

    MEMCACHED_VERSION="1.6.23"
    TMPDIR=$(mktemp -d)
    cd "$TMPDIR"

    curl -sL "https://memcached.org/files/memcached-${MEMCACHED_VERSION}.tar.gz" | tar xz
    cd memcached-${MEMCACHED_VERSION}
    ./configure --prefix="$LOCAL_PREFIX" --with-libevent="$LOCAL_PREFIX"
    make -j$(nproc)
    make install

    cd /
    rm -rf "$TMPDIR"
    echo "Memcached installed to $LOCAL_PREFIX/bin/"
    "$LOCAL_PREFIX/bin/memcached" -h 2>&1 | head -1
}

install_memcached() {
    if $LOCAL_INSTALL; then
        install_memcached_local
        return
    fi

    echo "=== Installing Memcached ==="
    if [[ "$PKG_MGR" == "brew" ]]; then
        brew install memcached libmemcached
    elif [[ "$PKG_MGR" == "apt" ]]; then
        sudo apt-get update
        sudo apt-get install -y memcached libmemcached-dev libmemcached-tools
    elif [[ "$PKG_MGR" == "yum" ]] || [[ "$PKG_MGR" == "dnf" ]]; then
        sudo $PKG_MGR install -y memcached libmemcached-devel || install_memcached_local
    else
        install_memcached_local
    fi
}

# ── RocksDB ──────────────────────────────────────────────────────────────────

install_rocksdb_local() {
    echo "=== Installing RocksDB from source ==="
    ROCKSDB_VERSION="9.0.0"
    TMPDIR=$(mktemp -d)
    cd "$TMPDIR"

    echo "Downloading RocksDB v${ROCKSDB_VERSION}..."
    curl -sL "https://github.com/facebook/rocksdb/archive/refs/tags/v${ROCKSDB_VERSION}.tar.gz" | tar xz
    cd "rocksdb-${ROCKSDB_VERSION}"

    echo "Building RocksDB (this may take a while)..."
    # Build static lib for local install (avoids LD_LIBRARY_PATH issues)
    make static_lib -j$(nproc)

    mkdir -p "$LOCAL_PREFIX/lib" "$LOCAL_PREFIX/include"
    cp librocksdb.a "$LOCAL_PREFIX/lib/"
    cp -r include/rocksdb "$LOCAL_PREFIX/include/"

    cd /
    rm -rf "$TMPDIR"
    echo "RocksDB installed to $LOCAL_PREFIX"
}

install_rocksdb() {
    if $LOCAL_INSTALL; then
        install_rocksdb_local
        return
    fi

    echo "=== Installing RocksDB ==="
    if [[ "$PKG_MGR" == "brew" ]]; then
        brew install rocksdb
    elif [[ "$PKG_MGR" == "apt" ]]; then
        sudo apt-get update
        sudo apt-get install -y librocksdb-dev || install_rocksdb_local
    elif [[ "$PKG_MGR" == "yum" ]] || [[ "$PKG_MGR" == "dnf" ]]; then
        # Install build dependencies first
        sudo $PKG_MGR install -y gflags-devel snappy-devel zlib-devel \
            bzip2-devel lz4-devel libzstd-devel 2>/dev/null || true
        install_rocksdb_local
    else
        install_rocksdb_local
    fi
}

# ── DuckDB ───────────────────────────────────────────────────────────────────

install_duckdb() {
    echo "=== Installing DuckDB ==="
    DUCKDB_VERSION="1.0.0"

    if [[ "$PKG_MGR" == "brew" ]] && ! $LOCAL_INSTALL; then
        brew install duckdb
    else
        # Install CLI
        TMPDIR=$(mktemp -d)
        cd "$TMPDIR"

        if [[ "$(uname -m)" == "x86_64" ]]; then
            ARCH="amd64"
        elif [[ "$(uname -m)" == "aarch64" ]] || [[ "$(uname -m)" == "arm64" ]]; then
            ARCH="aarch64"
        else
            echo "Unsupported architecture: $(uname -m)"
            return 1
        fi

        echo "Downloading DuckDB CLI..."
        curl -sL "https://github.com/duckdb/duckdb/releases/download/v${DUCKDB_VERSION}/duckdb_cli-linux-${ARCH}.zip" -o duckdb.zip
        unzip -q duckdb.zip

        mkdir -p "$LOCAL_PREFIX/bin"
        mv duckdb "$LOCAL_PREFIX/bin/"
        chmod +x "$LOCAL_PREFIX/bin/duckdb"

        cd /
        rm -rf "$TMPDIR"
    fi

    # Install Python package
    echo "Installing DuckDB Python package..."
    pip3 install --user duckdb 2>/dev/null || pip install --user duckdb 2>/dev/null || true

    echo "DuckDB installed"
    "$LOCAL_PREFIX/bin/duckdb" --version 2>/dev/null || duckdb --version
}

# ── Python packages ──────────────────────────────────────────────────────────

install_python_deps() {
    echo "=== Installing Python dependencies ==="
    pip3 install --user numpy pandas polars scikit-learn networkx 2>/dev/null || \
    pip install --user numpy pandas polars scikit-learn networkx 2>/dev/null || true
    echo "Python dependencies installed"
}

# ── Rebuild Smash ────────────────────────────────────────────────────────────

rebuild_smash() {
    echo "=== Rebuilding Smash with new dependencies ==="
    cd "$PROJECT_DIR/build"

    CMAKE_ARGS="-DSMASH_BUILD_BENCH=ON"
    if $LOCAL_INSTALL; then
        CMAKE_ARGS="$CMAKE_ARGS -DCMAKE_PREFIX_PATH=$LOCAL_PREFIX"
    fi

    cmake .. $CMAKE_ARGS
    make -j$(nproc)
    echo "Smash rebuilt successfully"
}

# ── Help ─────────────────────────────────────────────────────────────────────

show_help() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --all        Install all dependencies"
    echo "  --rocksdb    Install RocksDB"
    echo "  --duckdb     Install DuckDB"
    echo "  --memcached  Install Memcached"
    echo "  --redis      Install Redis"
    echo "  --python     Install Python packages (polars, sklearn, etc.)"
    echo "  --rebuild    Rebuild Smash after installing dependencies"
    echo "  --local      Install from source to ~/.local (no sudo required)"
    echo "  --help       Show this help"
    echo ""
    echo "Examples:"
    echo "  $0 --all --rebuild           # Install everything with package manager"
    echo "  $0 --all --local --rebuild   # Install from source (no sudo)"
    echo "  $0 --redis --memcached --local"
}

# ── Main ─────────────────────────────────────────────────────────────────────

if [[ $# -eq 0 ]]; then
    show_help
    exit 0
fi

DO_ROCKSDB=false
DO_DUCKDB=false
DO_MEMCACHED=false
DO_REDIS=false
DO_PYTHON=false
DO_REBUILD=false

for arg in "$@"; do
    case $arg in
        --all)
            DO_ROCKSDB=true
            DO_DUCKDB=true
            DO_MEMCACHED=true
            DO_REDIS=true
            DO_PYTHON=true
            ;;
        --rocksdb)
            DO_ROCKSDB=true
            ;;
        --duckdb)
            DO_DUCKDB=true
            ;;
        --memcached)
            DO_MEMCACHED=true
            ;;
        --redis)
            DO_REDIS=true
            ;;
        --python)
            DO_PYTHON=true
            ;;
        --rebuild)
            DO_REBUILD=true
            ;;
        --local)
            LOCAL_INSTALL=true
            ;;
        --help)
            show_help
            exit 0
            ;;
        *)
            echo "Unknown option: $arg"
            show_help
            exit 1
            ;;
    esac
done

detect_pkg_mgr

# Ensure ~/.local/bin is in PATH
if [[ ":$PATH:" != *":$LOCAL_PREFIX/bin:"* ]]; then
    export PATH="$LOCAL_PREFIX/bin:$PATH"
    echo "Note: Add to your ~/.bashrc: export PATH=\"$LOCAL_PREFIX/bin:\$PATH\""
fi

# Run installations
$DO_REDIS && install_redis
$DO_MEMCACHED && install_memcached
$DO_ROCKSDB && install_rocksdb
$DO_DUCKDB && install_duckdb
$DO_PYTHON && install_python_deps
$DO_REBUILD && rebuild_smash

echo ""
echo "=== Installation complete ==="
echo "Installed to: $LOCAL_PREFIX/bin"
echo ""
echo "Run experiments with:"
echo "  cd build && python3 ../bench/run_paper_experiments.py --runs 3"
