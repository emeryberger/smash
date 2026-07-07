#!/bin/bash
# Watch the InnoDB pool VMA's RSS (from smaps) during cool, alongside total
# VmRSS. Distinguishes "madvise not reclaiming the pool" from "reclaimed but
# reappears elsewhere / refaults".
set -u
LIB=/home/emerydb/git/smash_extent/build/libsmash.so
D=/home/emerydb/mariadb_bench/data; S=/home/emerydb/mariadb_bench/mysql.sock; L=/home/emerydb/mariadb_bench/sm.log
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
# Identify the 512MB pool VMA start (page-aligned hex, no dashes)
POOL=$(awk '/rw-p/{split($1,a,"-"); sz=(strtonum("0x"a[2])-strtonum("0x"a[1])); if(sz>=500*1048576 && sz<=520*1048576){print a[1]; exit}}' /proc/$MPID/maps)
echo "pool VMA start: $POOL"
for i in $(seq 1 12); do
  sleep 3
  total=$(awk '/VmRSS/{print int($2/1024)}' /proc/$MPID/status)
  # pool VMA RSS from smaps
  poolrss=$(awk -v p="$POOL" '$1 ~ ("^"p"-"){f=1} f&&/^Rss:/{print int($2/1024);f=0}' /proc/$MPID/smaps 2>/dev/null)
  echo "  t=$((i*3))s total_RSS=${total}MB pool_VMA_RSS=${poolrss}MB"
done
pkill -9 mariadbd 2>/dev/null
