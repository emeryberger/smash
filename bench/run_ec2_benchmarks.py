#!/usr/bin/env python3
"""
run_ec2_benchmarks.py - Launch EC2 instance and run Smash benchmarks

This script:
  1. Finds a region with a default VPC and available capacity
  2. Launches an Amazon Linux 2023 instance (glibc 2.34+)
  3. Syncs the smash source code
  4. Builds everything including google/tcmalloc via Bazel
  5. Runs all benchmarks
  6. Downloads results to local machine
  7. Terminates the instance

Prerequisites:
  - boto3: pip install boto3
  - AWS credentials configured (aws configure or environment variables)
  - rsync and ssh available locally

Usage:
  python3 bench/run_ec2_benchmarks.py [--quick] [--keep-instance] [--region REGION]
"""

import argparse
import os
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    import boto3
    from botocore.exceptions import ClientError
except ImportError:
    print("ERROR: boto3 not installed. Run: pip install boto3")
    sys.exit(1)


# Instance types with ~32GB RAM, ordered by typical availability
PREFERRED_INSTANCES = [
    # General purpose 2xlarge (8 vCPU, 32GB) - best availability
    "m7i.2xlarge", "m7a.2xlarge", "m6i.2xlarge", "m6a.2xlarge",
    "m5.2xlarge", "m5a.2xlarge", "m5n.2xlarge",
    # Compute optimized 4xlarge (16 vCPU, 32GB)
    "c7i.4xlarge", "c7a.4xlarge", "c6i.4xlarge", "c6a.4xlarge",
    "c5.4xlarge", "c5a.4xlarge", "c5n.4xlarge",
    # Memory optimized xlarge (4 vCPU, 32GB)
    "r7i.xlarge", "r6i.xlarge", "r6a.xlarge", "r5.xlarge", "r5a.xlarge",
    # Burstable (8 vCPU, 32GB) - fallback, usually available
    "t3.2xlarge", "t3a.2xlarge",
]

REGIONS_TO_TRY = ["us-west-2", "us-east-1", "us-east-2", "eu-west-1", "ap-northeast-1"]

USER_DATA_SCRIPT = """#!/bin/bash
set -ex

# Install build dependencies
# Note: AL2023 uses libzstd-devel not zstd-devel
dnf install -y \
    git gcc gcc-c++ cmake ninja-build \
    autoconf automake libtool \
    java-17-amazon-corretto-devel \
    libevent-devel openssl-devel \
    zlib-devel bzip2-devel lz4-devel snappy-devel libzstd-devel \
    python3 python3-pip \
    bc htop || true

# Verify critical packages installed
which cmake gcc g++ git || { echo "CRITICAL: build tools missing"; exit 1; }

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
"""

REMOTE_BENCHMARK_SCRIPT = """#!/bin/bash
set -ex
BENCH_FLAGS="$1"

cd ~/smash
mkdir -p build && cd build

# Configure with benchmarks
cmake .. -DSMASH_BUILD_BENCH=ON -DSMASH_BUILD_BENCH_DEPS=ON -DCMAKE_BUILD_TYPE=Release

# Build everything (parallel)
make -j$(nproc)

# Build benchmark dependencies
make bench_deps || true

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
    2>&1 | tee "$RESULTS_DIR/allocator_compare.log" || true

# Run Redis benchmark
echo ""
echo "=== Redis Benchmark ==="
bash bench/bench_redis.sh $BENCH_FLAGS 2>&1 | tee "$RESULTS_DIR/redis.log" || true

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
./bench/bench_algo_compare 2>&1 | tee "$RESULTS_DIR/algo_compare.log" || true

echo ""
echo "=== Benchmarks Complete ==="
ls -la "$RESULTS_DIR/"
"""


def log(msg: str):
    """Print timestamped log message."""
    ts = datetime.now().strftime("%H:%M:%S")
    print(f"[{ts}] {msg}")


def error(msg: str):
    """Print error and exit."""
    log(f"ERROR: {msg}")
    sys.exit(1)


def find_region_with_default_vpc(preferred_region: str = None) -> tuple[str, boto3.Session]:
    """Find a region that has a default VPC with subnets."""
    regions = [preferred_region] if preferred_region else []
    regions.extend([r for r in REGIONS_TO_TRY if r != preferred_region])

    for region in regions:
        try:
            session = boto3.Session(region_name=region)
            ec2 = session.client("ec2")

            # Check for default subnets
            resp = ec2.describe_subnets(
                Filters=[{"Name": "default-for-az", "Values": ["true"]}]
            )
            if resp["Subnets"]:
                log(f"Found default VPC in {region} with {len(resp['Subnets'])} subnets")
                return region, session
        except ClientError as e:
            log(f"  {region}: {e.response['Error']['Message']}")
        except Exception as e:
            log(f"  {region}: {e}")

    error("No region found with default VPC. Create one with: aws ec2 create-default-vpc")


def get_available_instance_types(ec2, region: str) -> set[str]:
    """Get set of instance types available in this region."""
    types = set()
    paginator = ec2.get_paginator("describe_instance_type_offerings")
    for page in paginator.paginate(LocationType="region"):
        for offering in page["InstanceTypeOfferings"]:
            types.add(offering["InstanceType"])
    return types


def get_latest_al2023_ami(ec2) -> str:
    """Get the latest Amazon Linux 2023 AMI ID."""
    resp = ec2.describe_images(
        Owners=["amazon"],
        Filters=[
            {"Name": "name", "Values": ["al2023-ami-2023*-x86_64"]},
            {"Name": "state", "Values": ["available"]},
        ],
    )
    if not resp["Images"]:
        error("Could not find Amazon Linux 2023 AMI")

    # Sort by creation date, newest first
    images = sorted(resp["Images"], key=lambda x: x["CreationDate"], reverse=True)
    return images[0]["ImageId"]


def ensure_key_pair(ec2, key_name: str) -> Path:
    """Ensure key pair exists and return path to .pem file."""
    key_path = Path.home() / ".ssh" / f"{key_name}.pem"

    try:
        ec2.describe_key_pairs(KeyNames=[key_name])
        if key_path.exists():
            log(f"Using existing key pair: {key_name}")
            return key_path
        # Key exists in AWS but not locally - recreate
        log(f"Key pair '{key_name}' exists but local .pem missing - recreating...")
        ec2.delete_key_pair(KeyName=key_name)
    except ClientError as e:
        if "InvalidKeyPair.NotFound" not in str(e):
            raise

    # Create new key pair
    log(f"Creating key pair: {key_name}")
    key_path.parent.mkdir(parents=True, exist_ok=True)
    resp = ec2.create_key_pair(KeyName=key_name)
    key_path.write_text(resp["KeyMaterial"])
    key_path.chmod(0o600)
    log(f"Key saved to {key_path}")
    return key_path


def ensure_security_group(ec2) -> str:
    """Ensure security group exists and return its ID."""
    sg_name = "smash-bench-sg"

    try:
        resp = ec2.describe_security_groups(GroupNames=[sg_name])
        sg_id = resp["SecurityGroups"][0]["GroupId"]
        log(f"Using existing security group: {sg_id}")
        return sg_id
    except ClientError as e:
        if "InvalidGroup.NotFound" not in str(e):
            raise

    # Create security group
    log(f"Creating security group: {sg_name}")
    resp = ec2.create_security_group(
        GroupName=sg_name,
        Description="Smash benchmark access"
    )
    sg_id = resp["GroupId"]

    ec2.authorize_security_group_ingress(
        GroupId=sg_id,
        IpProtocol="tcp",
        FromPort=22,
        ToPort=22,
        CidrIp="0.0.0.0/0"
    )
    return sg_id


def get_availability_zones(ec2) -> list[str]:
    """Get list of available AZs."""
    resp = ec2.describe_availability_zones(
        Filters=[{"Name": "state", "Values": ["available"]}]
    )
    return [az["ZoneName"] for az in resp["AvailabilityZones"]]


def get_subnet_for_az(ec2, az: str) -> str | None:
    """Get default subnet ID for an AZ."""
    resp = ec2.describe_subnets(
        Filters=[
            {"Name": "availability-zone", "Values": [az]},
            {"Name": "default-for-az", "Values": ["true"]},
        ]
    )
    if resp["Subnets"]:
        return resp["Subnets"][0]["SubnetId"]
    return None


def launch_instance(ec2, ami_id: str, key_name: str, sg_id: str,
                   available_types: set[str], azs: list[str]) -> tuple[str, str]:
    """Try to launch an instance, cycling through types and AZs."""

    # Filter to available types
    types_to_try = [t for t in PREFERRED_INSTANCES if t in available_types]
    if not types_to_try:
        error(f"None of the preferred instance types are available in this region")

    log(f"Will try {len(types_to_try)} instance types across {len(azs)} AZs")

    for az in azs:
        subnet_id = get_subnet_for_az(ec2, az)
        if not subnet_id:
            continue

        for instance_type in types_to_try:
            log(f"Trying {instance_type} in {az}...")
            try:
                resp = ec2.run_instances(
                    ImageId=ami_id,
                    InstanceType=instance_type,
                    KeyName=key_name,
                    SecurityGroupIds=[sg_id],
                    SubnetId=subnet_id,
                    UserData=USER_DATA_SCRIPT,
                    MinCount=1,
                    MaxCount=1,
                    BlockDeviceMappings=[{
                        "DeviceName": "/dev/xvda",
                        "Ebs": {"VolumeSize": 100, "VolumeType": "gp3"}
                    }],
                    TagSpecifications=[{
                        "ResourceType": "instance",
                        "Tags": [{"Key": "Name", "Value": "smash-benchmark"}]
                    }]
                )
                instance_id = resp["Instances"][0]["InstanceId"]
                log(f"Launched {instance_type} in {az}: {instance_id}")
                return instance_id, instance_type

            except ClientError as e:
                err_code = e.response["Error"]["Code"]
                err_msg = e.response["Error"]["Message"]
                # Always show actual error for debugging
                log(f"  {err_code}: {err_msg[:100]}")

    error("Could not launch any instance. Try a different region or wait for capacity.")


def wait_for_instance_running(ec2, instance_id: str) -> str:
    """Wait for instance to be running and return public IP."""
    log("Waiting for instance to start...")
    waiter = ec2.get_waiter("instance_running")
    waiter.wait(InstanceIds=[instance_id])

    resp = ec2.describe_instances(InstanceIds=[instance_id])
    public_ip = resp["Reservations"][0]["Instances"][0].get("PublicIpAddress")
    if not public_ip:
        error("Instance has no public IP. Check VPC/subnet configuration.")

    log(f"Public IP: {public_ip}")
    return public_ip


def wait_for_ssh(host: str, key_path: Path, max_attempts: int = 30) -> bool:
    """Wait for SSH to become available."""
    log("Waiting for SSH...")
    for attempt in range(max_attempts):
        try:
            result = subprocess.run(
                ["ssh", "-o", "StrictHostKeyChecking=no", "-o", "ConnectTimeout=5",
                 "-o", "BatchMode=yes", "-i", str(key_path), f"ec2-user@{host}", "echo ok"],
                capture_output=True, text=True, timeout=15
            )
            if result.returncode == 0:
                log("SSH connected")
                return True
            if attempt % 3 == 0:
                # Show error every 30 seconds
                err = result.stderr.strip().split('\n')[-1] if result.stderr else "unknown"
                log(f"  SSH attempt {attempt}: {err[:60]}")
        except subprocess.TimeoutExpired:
            if attempt % 3 == 0:
                log(f"  SSH attempt {attempt}: timeout")
        time.sleep(10)
    return False


def wait_for_setup(host: str, key_path: Path, max_attempts: int = 60) -> bool:
    """Wait for user-data setup to complete."""
    log("Waiting for instance setup (installing dependencies)...")
    for attempt in range(max_attempts):
        try:
            result = subprocess.run(
                ["ssh", "-o", "StrictHostKeyChecking=no", "-o", "BatchMode=yes",
                 "-i", str(key_path), f"ec2-user@{host}",
                 "test -f /tmp/setup-complete && echo done || tail -1 /var/log/cloud-init-output.log 2>/dev/null || echo waiting"],
                capture_output=True, text=True, timeout=15
            )
            output = result.stdout.strip()
            if output == "done":
                log("Instance setup complete")
                return True
            if attempt % 6 == 0:  # Log every minute
                log(f"  Setup in progress ({attempt * 10}s): {output[:60]}")
        except subprocess.TimeoutExpired:
            if attempt % 6 == 0:
                log(f"  Setup in progress ({attempt * 10}s): SSH timeout")
        time.sleep(10)
    return False


def sync_source(host: str, key_path: Path, repo_root: Path):
    """Sync source code to instance."""
    log("Syncing source code...")
    cmd = [
        "rsync", "-avz",
        "--exclude=build", "--exclude=.git", "--exclude=*.o", "--exclude=*.a",
        "-e", f"ssh -o StrictHostKeyChecking=no -i {key_path}",
        f"{repo_root}/", f"ec2-user@{host}:~/smash/"
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        log(f"rsync stderr: {result.stderr}")
        error("Failed to sync source code")


def run_benchmarks(host: str, key_path: Path, quick: bool):
    """Run benchmarks on the instance."""
    log("Running benchmarks (this will take a while)...")
    flags = "--quick" if quick else ""

    cmd = [
        "ssh", "-o", "StrictHostKeyChecking=no",
        "-i", str(key_path), f"ec2-user@{host}",
        f"bash -s {flags}"
    ]

    # Stream output
    proc = subprocess.Popen(
        cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True
    )
    proc.stdin.write(REMOTE_BENCHMARK_SCRIPT)
    proc.stdin.close()

    for line in proc.stdout:
        print(line, end="")

    proc.wait()
    if proc.returncode != 0:
        log(f"Warning: Benchmark script exited with code {proc.returncode}")


def download_results(host: str, key_path: Path, results_dir: Path):
    """Download results from instance."""
    log(f"Downloading results to {results_dir}...")
    results_dir.mkdir(parents=True, exist_ok=True)

    cmd = [
        "rsync", "-avz",
        "-e", f"ssh -o StrictHostKeyChecking=no -i {key_path}",
        f"ec2-user@{host}:~/smash/ec2_results/", f"{results_dir}/"
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        log(f"Warning: rsync returned {result.returncode}: {result.stderr}")


def terminate_instance(ec2, instance_id: str):
    """Terminate the instance."""
    log(f"Terminating instance {instance_id}...")
    ec2.terminate_instances(InstanceIds=[instance_id])


def main():
    parser = argparse.ArgumentParser(description="Run Smash benchmarks on EC2")
    parser.add_argument("--region", help="AWS region (auto-detected if not specified)")
    parser.add_argument("--quick", action="store_true", help="Run quick benchmarks")
    parser.add_argument("--keep-instance", action="store_true", help="Don't terminate after benchmarks")
    parser.add_argument("--key-name", default=f"smash-bench-{os.environ.get('USER', 'default')}")
    args = parser.parse_args()

    script_dir = Path(__file__).parent
    repo_root = script_dir.parent
    results_dir = repo_root / "paper_results" / f"ec2_run_{datetime.now().strftime('%Y%m%d_%H%M%S')}"

    log("Smash EC2 Benchmark Runner")
    log("=" * 40)

    # Find region with default VPC
    region, session = find_region_with_default_vpc(args.region)
    ec2 = session.client("ec2")

    # Get account info
    sts = session.client("sts")
    account = sts.get_caller_identity()["Account"]
    log(f"Account: {account}, Region: {region}")

    instance_id = None
    try:
        # Setup
        key_path = ensure_key_pair(ec2, args.key_name)
        sg_id = ensure_security_group(ec2)
        ami_id = get_latest_al2023_ami(ec2)
        log(f"Using AMI: {ami_id}")

        available_types = get_available_instance_types(ec2, region)
        azs = get_availability_zones(ec2)
        log(f"Available AZs: {', '.join(azs)}")

        # Launch
        instance_id, instance_type = launch_instance(ec2, ami_id, args.key_name, sg_id, available_types, azs)
        public_ip = wait_for_instance_running(ec2, instance_id)

        if not wait_for_ssh(public_ip, key_path):
            error("SSH not available after timeout")

        if not wait_for_setup(public_ip, key_path):
            error("Instance setup timed out")

        log("Instance ready!")

        # Run benchmarks
        sync_source(public_ip, key_path, repo_root)
        run_benchmarks(public_ip, key_path, args.quick)
        download_results(public_ip, key_path, results_dir)

        log("")
        log("=" * 40)
        log("Benchmarks complete!")
        log(f"Results saved to: {results_dir}")
        log("=" * 40)

        if args.keep_instance:
            log("")
            log(f"Instance kept running: {instance_id} ({public_ip})")
            log(f"SSH: ssh -i {key_path} ec2-user@{public_ip}")
            log(f"To terminate: aws ec2 terminate-instances --instance-ids {instance_id} --region {region}")
            instance_id = None  # Don't terminate in finally

    finally:
        if instance_id and not args.keep_instance:
            terminate_instance(ec2, instance_id)


if __name__ == "__main__":
    main()
