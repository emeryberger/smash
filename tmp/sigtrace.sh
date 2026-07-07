#!/bin/bash
set -u
LIB=/home/emerydb/git/smash_extent/build/libsmash.so
D=/home/emerydb/mariadb_bench/data; S=/home/emerydb/mariadb_bench/mysql.sock; L=/home/emerydb/mariadb_bench/st.log
q(){ mariadb --socket=$S -u root "$@"; }
pkill -9 mariadbd 2>/dev/null; sleep 2; rm -rf "$D"; mkdir -p "$D"
mariadb-install-db --no-defaults --datadir="$D" --auth-root-authentication-method=normal >/dev/null 2>&1
LD_PRELOAD=$LIB SMASH_LARGE_ONLY=1 SMASH_TRACK_EXTERNAL=1 SMASH_COLD_TIMEOUT_SEC=3 SMASH_SIGTRACE=1 SMASH_DUMP_CRASH_BT=1 \
  /usr/sbin/mariadbd --no-defaults --datadir="$D" --socket="$S" --skip-networking --skip-grant-tables \
  --innodb-buffer-pool-size=512M --innodb-flush-log-at-trx-commit=0 --innodb-doublewrite=0 \
  --innodb-buffer-pool-load-at-startup=0 --innodb-buffer-pool-dump-at-shutdown=0 >"$L" 2>&1 &
for i in $(seq 1 60); do q -e "SELECT 1" >/dev/null 2>&1 && break; sleep 1; done
q -e "CREATE DATABASE IF NOT EXISTS b" 2>/dev/null
q b -e "CREATE TABLE t(id INT PRIMARY KEY,k INT,pad CHAR(200),payload VARCHAR(700)); CREATE TABLE nums(n INT PRIMARY KEY AUTO_INCREMENT); INSERT INTO nums VALUES (),(),(),(),(),(),(),(),(),(),(),(),(),(),(),();" 2>/dev/null
c=16; while [ "$c" -lt 800000 ]; do q b -e "INSERT INTO nums(n) SELECT n+$c FROM nums" 2>/dev/null; c=$((c*2)); done
q b -e "INSERT INTO t SELECT n,n MOD 1000,CONCAT(0x75,n MOD 100),REPEAT(CONCAT(0x746f6b,n MOD 50,0x20),30) FROM nums WHERE n<=800000" 2>/dev/null
sleep 6; pkill -9 mariadbd 2>/dev/null; sleep 1
echo "=== smash sig/crash trace ==="; grep -aiE "smash sig|smash crash|si_addr|si_code|chain_kind" "$L" | head
