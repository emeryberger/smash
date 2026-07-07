#!/bin/bash
# Dump InnoDB's large anon VMA structure + per-VMA RSS after fill, for both
# master (hash) and extent builds. Reveals whether the pool is one 512MB mmap
# or N chunks, and which VMAs hold resident memory.
set -u
LIB=$1  # path to libsmash.so
D=/home/emerydb/mariadb_bench/data; S=/home/emerydb/mariadb_bench/mysql.sock; L=/home/emerydb/mariadb_bench/pm.log
q(){ mariadb --socket=$S -u root "$@"; }
pkill -9 mariadbd 2>/dev/null; sleep 2; rm -rf "$D"; mkdir -p "$D"
mariadb-install-db --no-defaults --datadir="$D" --auth-root-authentication-method=normal >/dev/null 2>&1
LD_PRELOAD=$LIB SMASH_LARGE_ONLY=1 SMASH_TRACK_EXTERNAL=1 SMASH_COLD_TIMEOUT_SEC=3 SMASH_STATS=1 \
  /usr/sbin/mariadbd --no-defaults --datadir="$D" --socket="$S" --skip-networking --skip-grant-tables \
  --innodb-buffer-pool-size=512M --innodb-flush-log-at-trx-commit=0 --innodb-doublewrite=0 \
  --innodb-buffer-pool-load-at-startup=0 --innodb-buffer-pool-dump-at-shutdown=0 >"$L" 2>&1 &
MPID=$!
for i in $(seq 1 60); do q -e "SELECT 1" >/dev/null 2>&1 && break; sleep 1; done
q -e "CREATE DATABASE IF NOT EXISTS b" 2>/dev/null
q b -e "CREATE TABLE t(id INT PRIMARY KEY,k INT,pad CHAR(200),payload VARCHAR(700)); CREATE TABLE nums(n INT PRIMARY KEY AUTO_INCREMENT); INSERT INTO nums VALUES (),(),(),(),(),(),(),(),(),(),(),(),(),(),(),();" 2>/dev/null
c=16; while [ "$c" -lt 1500000 ]; do q b -e "INSERT INTO nums(n) SELECT n+$c FROM nums" 2>/dev/null; c=$((c*2)); done
q b -e "INSERT INTO t SELECT n,n MOD 1000,CONCAT(0x75,n MOD 100),REPEAT(CONCAT(0x746f6b,n MOD 50,0x20),30) FROM nums WHERE n<=1500000" 2>/dev/null
q b -e "DROP TABLE nums" 2>/dev/null
q -e "SET GLOBAL innodb_max_dirty_pages_pct=0" 2>/dev/null
q b -e "FLUSH TABLES" 2>/dev/null
sleep 10  # let compressor run
echo "=== total RSS: $(awk '/VmRSS/{print int($2/1024)}' /proc/$MPID/status)MB ==="
echo "=== anon VMAs >= 32MB with RSS (from smaps) ==="
awk '
  /^[0-9a-f]+-[0-9a-f]+ / {
    split($1,a,"-"); sz=(strtonum("0x"a[2])-strtonum("0x"a[1]))/1048576;
    vma=$1; perm=$2; big=(sz>=32); rssline=0
  }
  big && /^Rss:/ { printf "  %8.0fMB size, RSS=%6d kB  %s %s\n", sz, $2, perm, vma }
' /proc/$MPID/smaps 2>/dev/null | sort -t= -k2 -rn | head -20
pkill -9 mariadbd 2>/dev/null
