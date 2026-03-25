#!/usr/bin/env bash
# run_ec2_benchmarks.sh - Launch EC2 instance with modern glibc and run all benchmarks
#
# This script:
#   1. Launches an Amazon Linux 2023 instance (glibc 2.34+, supports google/tcmalloc)
#   2. Syncs the smash source code
#   3. Builds everything including google/tcmalloc via Bazel
#   4. Runs all benchmarks (Redis, Memcached, RocksDB, allocator comparison)
#   5. Downloads results to local machine
#   6. Terminates the instance
#
# Prerequisites:
#   - AWS CLI configured with valid credentials (aws configure)
#   - An EC2 key pair (will prompt to create if needed)
#   - Default VPC with internet access
#
# Usage:
#   ./bench/run_ec2_benchmarks.sh [--instance-type TYPE] [--keep-instance] [--quick]
#
# Options:
#   --instance-type TYPE   EC2 instance type (default: c5.4xlarge for 16 vCPUs)
#   --keep-instance        Don't terminate instance after benchmarks
#   --quick                Run quick benchmarks (smaller workloads)
#   --key-name NAME        Use existing EC2 key pair

set -euo pipefail

# ═══════════════════════════════════════════════════════════════════════════════
# Configuration
# ═══════════════════════════════════════════════════════════════════════════════

INSTANCE_TYPE="c5.4xlarge"  # 16 vCPU, 32GB RAM - good for parallel builds
AMI_NAME_PATTERN="al2023-ami-2023*-x86_64"  # Amazon Linux 2023
KEEP_INSTANCE=false
QUICK_MODE=false
KEY_NAME=""
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RESULTS_DIR="$REPO_ROOT/paper_results/ec2_run_$(date +%Y%m%d_%H%M%S)"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --instance-type)
            INSTANCE_TYPE="$2"
            shift 2
            ;;
        --keep-instance)
            KEEP_INSTANCE=true
            shift
            ;;
        --quick)
            QUICK_MODE=true
            shift
            ;;
        --key-name)
            KEY_NAME="$2"
            shift 2
            ;;
        --help|-h)
            head -30 "$0" | grep "^#" | sed 's/^# *//'
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# ═══════════════════════════════════════════════════════════════════════════════
# Helper functions
# ═══════════════════════════════════════════════════════════════════════════════

log() {
    echo "[$(date '+%H:%M:%S')] $*"
}

error() {
    echo "[$(date '+%H:%M:%S')] ERROR: $*" >&2
    exit 1
}

cleanup() {
    if [[ -n "${INSTANCE_ID:-}" && "$KEEP_INSTANCE" == "false" ]]; then
        log "Terminating instance $INSTANCE_ID..."
        aws ec2 terminate-instances --instance-ids "$INSTANCE_ID" >/dev/null 2>&1 || true
    fi
    if [[ -n "${TMP_KEY_FILE:-}" && -f "$TMP_KEY_FILE" ]]; then
        rm -f "$TMP_KEY_FILE"
    fi
}

wait_for_ssh() {
    local host="$1"
    local key="$2"
    local max_attempts=30
    local attempt=0

    log "Waiting for SSH to become available..."
    while [[ $attempt -lt $max_attempts ]]; do
        if ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -i "$key" ec2-user@"$host" "echo ok" 2>/dev/null; then
            return 0
        fi
        attempt=$((attempt + 1))
        sleep 10
    done
    error "SSH not available after ${max_attempts} attempts"
}

# ═══════════════════════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════════════════════

trap cleanup EXIT

log "Smash EC2 Benchmark Runner"
log "=========================="

# Check AWS credentials
log "Checking AWS credentials..."
if ! aws sts get-caller-identity >/dev/null 2>&1; then
    error "AWS credentials not configured. Run: aws configure"
fi
AWS_ACCOUNT=$(aws sts get-caller-identity --query Account --output text)
AWS_REGION=$(aws configure get region || echo "us-east-1")
log "Account: $AWS_ACCOUNT, Region: $AWS_REGION"

# Find or create key pair
if [[ -z "$KEY_NAME" ]]; then
    KEY_NAME="smash-bench-$(whoami)"
fi
TMP_KEY_FILE="$HOME/.ssh/${KEY_NAME}.pem"

if aws ec2 describe-key-pairs --key-names "$KEY_NAME" >/dev/null 2>&1; then
    # Key exists in AWS
    if [[ ! -f "$TMP_KEY_FILE" ]]; then
        log "Key pair '$KEY_NAME' exists but local .pem missing - recreating..."
        aws ec2 delete-key-pair --key-name "$KEY_NAME"
        aws ec2 create-key-pair --key-name "$KEY_NAME" --query 'KeyMaterial' --output text > "$TMP_KEY_FILE"
        chmod 600 "$TMP_KEY_FILE"
        log "Key recreated and saved to $TMP_KEY_FILE"
    fi
else
    # Key doesn't exist - create it
    log "Creating new key pair: $KEY_NAME"
    mkdir -p "$(dirname "$TMP_KEY_FILE")"
    aws ec2 create-key-pair --key-name "$KEY_NAME" --query 'KeyMaterial' --output text > "$TMP_KEY_FILE"
    chmod 600 "$TMP_KEY_FILE"
    log "Key saved to $TMP_KEY_FILE"
fi

# Find latest Amazon Linux 2023 AMI
log "Finding latest Amazon Linux 2023 AMI..."
AMI_ID=$(aws ec2 describe-images \
    --owners amazon \
    --filters "Name=name,Values=$AMI_NAME_PATTERN" "Name=state,Values=available" \
    --query 'sort_by(Images, &CreationDate)[-1].ImageId' \
    --output text)
[[ "$AMI_ID" == "None" || -z "$AMI_ID" ]] && error "Could not find Amazon Linux 2023 AMI"
log "Using AMI: $AMI_ID"

# Create security group if needed
SG_NAME="smash-bench-sg"
SG_ID=$(aws ec2 describe-security-groups --group-names "$SG_NAME" --query 'SecurityGroups[0].GroupId' --output text 2>/dev/null || echo "")
if [[ -z "$SG_ID" || "$SG_ID" == "None" ]]; then
    log "Creating security group: $SG_NAME"
    SG_ID=$(aws ec2 create-security-group --group-name "$SG_NAME" --description "Smash benchmark access" --query 'GroupId' --output text)
    aws ec2 authorize-security-group-ingress --group-id "$SG_ID" --protocol tcp --port 22 --cidr 0.0.0.0/0
fi
log "Security group: $SG_ID"

# User data script to install dependencies
USER_DATA=$(cat <<'USERDATA'
#!/bin/bash
set -ex

# Install build dependencies
dnf install -y \
    git gcc gcc-c++ cmake ninja-build \
    autoconf automake libtool \
    java-17-amazon-corretto-devel \
    libevent-devel openssl-devel \
    zlib-devel bzip2-devel lz4-devel snappy-devel zstd-devel \
    python3 python3-pip \
    bc htop

# Install Bazel via Bazelisk
curl -Lo /usr/local/bin/bazel https://github.com/bazelbuild/bazelisk/releases/download/v1.25.0/bazelisk-linux-amd64
chmod +x /usr/local/bin/bazel

# Install jemalloc from source (AL2023 doesn't have it in repos)
cd /tmp
git clone --depth 1 --branch 5.3.0 https://github.com/jemalloc/jemalloc.git
cd jemalloc
./autogen.sh
./configure --prefix=/usr/local
make -j$(nproc)
make install
ldconfig

# Signal ready
touch /tmp/setup-complete
USERDATA
)

# Query actually available instance types in this region (8-16 vCPUs, current gen)
log "Querying available instance types in $AWS_REGION..."
AVAILABLE_TYPES=$(aws ec2 describe-instance-type-offerings \
    --location-type region \
    --query 'InstanceTypeOfferings[*].InstanceType' \
    --output text | tr '\t' '\n' | grep -E '\.(2xlarge|4xlarge)$' | sort -u)

# Filter to preferred types for benchmarking (compute/general purpose, 8-16 vCPU)
PREFERRED_PATTERNS="c7i c7a c6i c6a c5 m7i m7a m6i m6a m5 r7i r6i r5 t3"
INSTANCE_TYPES=()

# Add user-specified type first
INSTANCE_TYPES+=("$INSTANCE_TYPE")

# Add available types in preference order
for pattern in $PREFERRED_PATTERNS; do
    for size in 4xlarge 2xlarge; do
        candidate="${pattern}.${size}"
        if echo "$AVAILABLE_TYPES" | grep -q "^${candidate}$"; then
            # Avoid duplicates
            if [[ ! " ${INSTANCE_TYPES[*]} " =~ " ${candidate} " ]]; then
                INSTANCE_TYPES+=("$candidate")
            fi
        fi
    done
done

log "Will try instance types: ${INSTANCE_TYPES[*]:0:10}..."

# Get available AZs
AZS=($(aws ec2 describe-availability-zones --query 'AvailabilityZones[?State==`available`].ZoneName' --output text))
log "Available AZs: ${AZS[*]}"

# Launch instance - try each AZ and instance type
INSTANCE_ID=""
for az in "${AZS[@]}"; do
    # Find a subnet in this AZ
    SUBNET_ID=$(aws ec2 describe-subnets \
        --filters "Name=availability-zone,Values=$az" "Name=default-for-az,Values=true" \
        --query 'Subnets[0].SubnetId' --output text 2>/dev/null || echo "None")

    [[ "$SUBNET_ID" == "None" || -z "$SUBNET_ID" ]] && continue

    for itype in "${INSTANCE_TYPES[@]:0:15}"; do
        log "Trying $itype in $az..."
        RESULT=$(aws ec2 run-instances \
            --image-id "$AMI_ID" \
            --instance-type "$itype" \
            --key-name "$KEY_NAME" \
            --security-group-ids "$SG_ID" \
            --subnet-id "$SUBNET_ID" \
            --user-data "$USER_DATA" \
            --block-device-mappings '[{"DeviceName":"/dev/xvda","Ebs":{"VolumeSize":100,"VolumeType":"gp3"}}]' \
            --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=smash-benchmark}]" \
            --query 'Instances[0].InstanceId' \
            --output text 2>&1) || true

        if [[ "$RESULT" =~ ^i- ]]; then
            INSTANCE_ID="$RESULT"
            INSTANCE_TYPE="$itype"
            log "Successfully launched $itype in $az: $INSTANCE_ID"
            break 2
        elif [[ "$RESULT" == *"InsufficientInstanceCapacity"* ]]; then
            log "  No capacity"
        elif [[ "$RESULT" == *"Unsupported"* ]]; then
            log "  Unsupported in this AZ"
        else
            log "  Error: ${RESULT:0:100}"
        fi
    done
done

if [[ -z "$INSTANCE_ID" ]]; then
    echo ""
    echo "Could not launch any instance. Suggestions:"
    echo "  1. Try a different region: AWS_DEFAULT_REGION=us-east-1 $0"
    echo "  2. Try a specific type: $0 --instance-type t3.xlarge"
    echo "  3. Wait and retry (capacity fluctuates)"
    error "No instance capacity available"
fi
log "Instance ID: $INSTANCE_ID (type: $INSTANCE_TYPE)"

# Wait for instance to be running
log "Waiting for instance to start..."
aws ec2 wait instance-running --instance-ids "$INSTANCE_ID"

# Get public IP
PUBLIC_IP=$(aws ec2 describe-instances \
    --instance-ids "$INSTANCE_ID" \
    --query 'Reservations[0].Instances[0].PublicIpAddress' \
    --output text)
log "Public IP: $PUBLIC_IP"

# Wait for SSH
wait_for_ssh "$PUBLIC_IP" "$TMP_KEY_FILE"

# Wait for user-data setup to complete
log "Waiting for instance setup to complete..."
for i in {1..60}; do
    if ssh -o StrictHostKeyChecking=no -i "$TMP_KEY_FILE" ec2-user@"$PUBLIC_IP" "test -f /tmp/setup-complete" 2>/dev/null; then
        break
    fi
    if [[ $i -eq 60 ]]; then
        error "Instance setup timed out"
    fi
    sleep 10
done
log "Instance setup complete"

# Sync source code
log "Syncing source code to instance..."
rsync -avz --exclude=build --exclude=.git --exclude='*.o' --exclude='*.a' \
    -e "ssh -o StrictHostKeyChecking=no -i $TMP_KEY_FILE" \
    "$REPO_ROOT/" ec2-user@"$PUBLIC_IP":~/smash/

# Build and run benchmarks
BENCH_FLAGS=""
if [[ "$QUICK_MODE" == "true" ]]; then
    BENCH_FLAGS="--quick"
fi

log "Building and running benchmarks on EC2..."
ssh -o StrictHostKeyChecking=no -i "$TMP_KEY_FILE" ec2-user@"$PUBLIC_IP" bash -s "$BENCH_FLAGS" <<'REMOTE_SCRIPT'
#!/bin/bash
set -ex
BENCH_FLAGS="$1"

cd ~/smash
mkdir -p build && cd build

# Configure with benchmarks
cmake .. -DSMASH_BUILD_BENCH=ON -DSMASH_BUILD_BENCH_DEPS=ON -DCMAKE_BUILD_TYPE=Release

# Build everything (parallel)
make -j$(nproc)

# Build benchmark dependencies
make bench_deps

# Create results directory
RESULTS_DIR=~/smash/ec2_results
mkdir -p "$RESULTS_DIR"

# Print system info
echo "=== System Info ===" | tee "$RESULTS_DIR/system_info.txt"
uname -a | tee -a "$RESULTS_DIR/system_info.txt"
ldd --version 2>&1 | head -1 | tee -a "$RESULTS_DIR/system_info.txt"
nproc | tee -a "$RESULTS_DIR/system_info.txt"
free -h | tee -a "$RESULTS_DIR/system_info.txt"

# Run allocator comparison benchmark
echo ""
echo "=== Allocator Comparison ==="
python3 bench/bench_allocator_compare.py --output "$RESULTS_DIR/allocator_compare" --runs 3 \
    2>&1 | tee "$RESULTS_DIR/allocator_compare.log"

# Run Redis benchmark
echo ""
echo "=== Redis Benchmark ==="
bash bench/bench_redis.sh $BENCH_FLAGS 2>&1 | tee "$RESULTS_DIR/redis.log"

# Run Redis multi-allocator comparison
echo ""
echo "=== Redis Allocator Comparison ==="
bash bench/bench_redis_alloc.sh $BENCH_FLAGS 2>&1 | tee "$RESULTS_DIR/redis_alloc.log" || true

# Run Memcached benchmark
echo ""
echo "=== Memcached Benchmark ==="
bash bench/bench_memcached.sh $BENCH_FLAGS 2>&1 | tee "$RESULTS_DIR/memcached.log" || true

# Run RocksDB benchmark
echo ""
echo "=== RocksDB Benchmark ==="
bash bench/bench_rocksdb.sh $BENCH_FLAGS 2>&1 | tee "$RESULTS_DIR/rocksdb.log" || true

# Run algorithm comparison
echo ""
echo "=== Algorithm Comparison ==="
./bench/bench_algo_compare 2>&1 | tee "$RESULTS_DIR/algo_compare.log"

echo ""
echo "=== Benchmarks Complete ==="
ls -la "$RESULTS_DIR/"
REMOTE_SCRIPT

# Download results
log "Downloading results..."
mkdir -p "$RESULTS_DIR"
rsync -avz -e "ssh -o StrictHostKeyChecking=no -i $TMP_KEY_FILE" \
    ec2-user@"$PUBLIC_IP":~/smash/ec2_results/ "$RESULTS_DIR/"

log ""
log "=========================================="
log "Benchmarks complete!"
log "Results saved to: $RESULTS_DIR"
log "=========================================="
ls -la "$RESULTS_DIR/"

if [[ "$KEEP_INSTANCE" == "true" ]]; then
    log ""
    log "Instance kept running: $INSTANCE_ID ($PUBLIC_IP)"
    log "SSH: ssh -i $TMP_KEY_FILE ec2-user@$PUBLIC_IP"
    log "To terminate: aws ec2 terminate-instances --instance-ids $INSTANCE_ID"
    trap - EXIT  # Don't cleanup on exit
else
    log ""
    log "Terminating instance..."
fi
