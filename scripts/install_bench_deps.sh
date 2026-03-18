#!/bin/bash
# Install dependencies for Smash paper experiments
# Usage: ./scripts/install_bench_deps.sh [--all|--rocksdb|--duckdb|--memcached|--redis]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Detect OS
if [[ "$OSTYPE" == "linux-gnu"* ]]; then
    if command -v apt-get &> /dev/null; then
        PKG_MGR="apt"
    elif command -v yum &> /dev/null; then
        PKG_MGR="yum"
    elif command -v dnf &> /dev/null; then
        PKG_MGR="dnf"
    else
        echo "Unsupported Linux package manager"
        exit 1
    fi
elif [[ "$OSTYPE" == "darwin"* ]]; then
    PKG_MGR="brew"
else
    echo "Unsupported OS: $OSTYPE"
    exit 1
fi

install_rocksdb() {
    echo "=== Installing RocksDB ==="

    if [[ "$PKG_MGR" == "brew" ]]; then
        brew install rocksdb
    elif [[ "$PKG_MGR" == "apt" ]]; then
        sudo apt-get update
        sudo apt-get install -y librocksdb-dev
        # If package not available, build from source
        if ! pkg-config --exists rocksdb 2>/dev/null; then
            install_rocksdb_source
        fi
    elif [[ "$PKG_MGR" == "yum" ]] || [[ "$PKG_MGR" == "dnf" ]]; then
        # Amazon Linux / RHEL - build from source
        install_rocksdb_source
    fi
}

install_rocksdb_source() {
    echo "Building RocksDB from source..."

    # Install build dependencies
    if [[ "$PKG_MGR" == "apt" ]]; then
        sudo apt-get install -y libgflags-dev libsnappy-dev zlib1g-dev \
            libbz2-dev liblz4-dev libzstd-dev
    elif [[ "$PKG_MGR" == "yum" ]] || [[ "$PKG_MGR" == "dnf" ]]; then
        sudo $PKG_MGR install -y gflags-devel snappy-devel zlib-devel \
            bzip2-devel lz4-devel libzstd-devel
    fi

    ROCKSDB_VERSION="9.0.0"
    TMPDIR=$(mktemp -d)
    cd "$TMPDIR"

    echo "Downloading RocksDB v${ROCKSDB_VERSION}..."
    curl -sL "https://github.com/facebook/rocksdb/archive/refs/tags/v${ROCKSDB_VERSION}.tar.gz" | tar xz
    cd "rocksdb-${ROCKSDB_VERSION}"

    echo "Building RocksDB (this may take a while)..."
    make shared_lib -j$(nproc)
    sudo make install-shared
    sudo ldconfig 2>/dev/null || true

    cd /
    rm -rf "$TMPDIR"
    echo "RocksDB installed successfully"
}

install_duckdb() {
    echo "=== Installing DuckDB ==="

    if [[ "$PKG_MGR" == "brew" ]]; then
        brew install duckdb
    else
        # Install CLI
        DUCKDB_VERSION="1.0.0"
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
        sudo mv duckdb /usr/local/bin/
        sudo chmod +x /usr/local/bin/duckdb

        cd /
        rm -rf "$TMPDIR"
    fi

    # Install Python package
    echo "Installing DuckDB Python package..."
    pip3 install --user duckdb || pip install --user duckdb

    echo "DuckDB installed successfully"
}

install_memcached() {
    echo "=== Installing Memcached ==="

    if [[ "$PKG_MGR" == "brew" ]]; then
        brew install memcached libmemcached
    elif [[ "$PKG_MGR" == "apt" ]]; then
        sudo apt-get update
        sudo apt-get install -y memcached libmemcached-dev libmemcached-tools
    elif [[ "$PKG_MGR" == "yum" ]] || [[ "$PKG_MGR" == "dnf" ]]; then
        sudo $PKG_MGR install -y memcached libmemcached-devel
    fi

    echo "Memcached installed successfully"
}

install_redis() {
    echo "=== Installing Redis ==="

    if [[ "$PKG_MGR" == "brew" ]]; then
        brew install redis
    elif [[ "$PKG_MGR" == "apt" ]]; then
        sudo apt-get update
        sudo apt-get install -y redis-server redis-tools
    elif [[ "$PKG_MGR" == "yum" ]] || [[ "$PKG_MGR" == "dnf" ]]; then
        sudo $PKG_MGR install -y redis
    fi

    echo "Redis installed successfully"
}

install_python_deps() {
    echo "=== Installing Python dependencies ==="
    pip3 install --user numpy pandas polars scikit-learn networkx || \
    pip install --user numpy pandas polars scikit-learn networkx
    echo "Python dependencies installed"
}

rebuild_smash() {
    echo "=== Rebuilding Smash with new dependencies ==="
    cd "$PROJECT_DIR/build"
    cmake .. -DSMASH_BUILD_BENCH=ON
    make -j$(nproc)
    echo "Smash rebuilt successfully"
}

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
    echo "  --help       Show this help"
    echo ""
    echo "Examples:"
    echo "  $0 --all --rebuild    # Install everything and rebuild"
    echo "  $0 --rocksdb --duckdb # Install just RocksDB and DuckDB"
}

# Parse arguments
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

# Run installations
$DO_ROCKSDB && install_rocksdb
$DO_DUCKDB && install_duckdb
$DO_MEMCACHED && install_memcached
$DO_REDIS && install_redis
$DO_PYTHON && install_python_deps
$DO_REBUILD && rebuild_smash

echo ""
echo "=== Installation complete ==="
echo "Run experiments with: cd build && python3 ../bench/run_paper_experiments.py --runs 3"
