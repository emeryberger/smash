#!/bin/bash
# Reproduce the extent-path segfault under gdb, small pool.
set -u
LIB=/home/emerydb/git/smash_extent/build/libsmash.so
BASE=/home/emerydb/mariadb_bench; DATADIR=$BASE/data; SOCK=$BASE/mysql.sock; LOG=$BASE/crash.log
mysql_q(){ mariadb --socket="$SOCK" -u root "$@"; }
pkill -9 mariadbd 2>/dev/null; sleep 2; rm -rf "$DATADIR"; mkdir -p "$DATADIR"
mariadb-install-db --no-defaults --datadir="$DATADIR" --auth-root-authentication-method=normal >/dev/null 2>&1
# Run mariadbd under gdb, batch, catch SIGSEGV, print backtrace of crashing thread.
LD_PRELOAD=$LIB SMASH_LARGE_ONLY=1 SMASH_TRACK_EXTERNAL=1 SMASH_COLD_TIMEOUT_SEC=3 \
gdb -batch \
  -ex "set pagination off" \
  -ex "handle SIGSEGV stop print" \
  -ex "run --no-defaults --datadir=$DATADIR --socket=$SOCK --skip-networking --skip-grant-tables --innodb-buffer-pool-size=512M --innodb-flush-log-at-trx-commit=0 --innodb-doublewrite=0 --innodb-buffer-pool-load-at-startup=0 --innodb-buffer-pool-dump-at-shutdown=0" \
  -ex "bt 20" \
  -ex "info registers rip" \
  /usr/sbin/mariadbd > /tmp/gdb_crash.txt 2>&1 &
GDBPID=$!
# wait for server up, then drive the fill that crashes
for i in $(seq 1 60); do mysql_q -e "SELECT 1" >/dev/null 2>&1 && break; sleep 1; done
mysql_q -e "CREATE DATABASE IF NOT EXISTS b;" 2>/dev/null
mysql_q b -e "DROP TABLE IF EXISTS t; DROP TABLE IF EXISTS nums;
  CREATE TABLE t(id INT PRIMARY KEY,k INT,pad CHAR(200),payload VARCHAR(700)) ENGINE=InnoDB;
  CREATE TABLE nums(n INT PRIMARY KEY AUTO_INCREMENT);
  INSERT INTO nums VALUES (),(),(),(),(),(),(),(),(),(),(),(),(),(),(),();" 2>/dev/null
c=16; while [ "$c" -lt 1500000 ]; do mysql_q b -e "INSERT INTO nums(n) SELECT n+$c FROM nums;" 2>/dev/null; c=$((c*2)); done
mysql_q b -e "INSERT INTO t SELECT n,n MOD 1000,CONCAT('u',n MOD 100),REPEAT(CONCAT('tok',n MOD 50,' '),30) FROM nums WHERE n<=1500000;" 2>/dev/null
sleep 3
mysql_q -e "SHUTDOWN" 2>/dev/null
sleep 5
kill -9 $GDBPID 2>/dev/null; pkill -9 mariadbd 2>/dev/null
echo "=== gdb crash backtrace ==="
grep -iE "Thread|#[0-9]+|SIGSEGV|smash|VmRegion|Extent|trackExternal|pageIndex|pageAddress|findExtent|compressPage|handleFault|rip" /tmp/gdb_crash.txt | head -40
