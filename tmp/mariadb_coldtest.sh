#!/bin/bash
# Does InnoDB let its buffer-pool frames go cold when idle+quiesced?
# 512 MB pool fits the 128K external-slot budget (4 KB pages) so
# SMASH_TRACK_EXTERNAL registers it cleanly (post-PR#53). We fill, force all
# pages CLEAN (flush), quiesce background flushing, idle, and watch smash's
# compressed= count + RSS. If pages stay warm, reduction ~0 and compressed
# stays flat; if they go cold, compressed rises and RSS drops.
set -u
LIB=/home/emerydb/git/smash_master/build/libsmash.so
BASE=/home/emerydb/mariadb_bench; DATADIR=$BASE/data; SOCK=$BASE/mysql.sock; LOG=$BASE/cold.log
ROWS=1500000; BP_MB=512
mysql_q(){ mariadb --socket="$SOCK" -u root "$@"; }
rss(){ awk '/VmRSS/{print int($2/1024)}' /proc/$1/status 2>/dev/null; }
zc(){ strings "$LOG" 2>/dev/null | grep -oE 'compressed=[0-9]+' | tail -1; }

run(){ # $1=label; rest=extra mariadbd quiesce args
  local lab="$1"; shift
  pkill -9 mariadbd 2>/dev/null; sleep 2; rm -rf "$DATADIR"; mkdir -p "$DATADIR"
  mariadb-install-db --no-defaults --datadir="$DATADIR" --auth-root-authentication-method=normal >/dev/null 2>&1
  LD_PRELOAD=$LIB SMASH_LARGE_ONLY=1 SMASH_TRACK_EXTERNAL=1 SMASH_COLD_TIMEOUT_SEC=3 SMASH_STATS=1 SMASH_DEBUG=1 \
    /usr/sbin/mariadbd --no-defaults --datadir="$DATADIR" --socket="$SOCK" \
    --skip-networking --skip-grant-tables --innodb-buffer-pool-size=${BP_MB}M \
    --innodb-flush-log-at-trx-commit=0 --innodb-doublewrite=0 \
    --innodb-buffer-pool-load-at-startup=0 --innodb-buffer-pool-dump-at-shutdown=0 \
    "$@" >"$LOG" 2>&1 &
  local mpid=$!
  for i in $(seq 1 40); do mysql_q -e "SELECT 1" >/dev/null 2>&1 && break; sleep 1; done
  kill -0 $mpid 2>/dev/null || { echo "$lab: START_FAIL"; tail -3 "$LOG"; return; }
  mysql_q -e "CREATE DATABASE IF NOT EXISTS b;"
  mysql_q b -e "DROP TABLE IF EXISTS t; DROP TABLE IF EXISTS nums;
    CREATE TABLE t(id INT PRIMARY KEY,k INT,pad CHAR(200),payload VARCHAR(700)) ENGINE=InnoDB;
    CREATE TABLE nums(n INT PRIMARY KEY AUTO_INCREMENT);
    INSERT INTO nums VALUES (),(),(),(),(),(),(),(),(),(),(),(),(),(),(),();"
  local c=16
  while [ "$c" -lt "$ROWS" ]; do mysql_q b -e "INSERT INTO nums(n) SELECT n+$c FROM nums;" 2>/dev/null; c=$((c*2)); done
  mysql_q b -e "INSERT INTO t SELECT n,n MOD 1000,CONCAT('u',n MOD 100),REPEAT(CONCAT('tok',n MOD 50,' '),30) FROM nums WHERE n<=$ROWS;"
  mysql_q b -e "DROP TABLE nums;"
  # Quiesce: make all pages clean, then idle.
  mysql_q -e "SET GLOBAL innodb_max_dirty_pages_pct=0;" 2>/dev/null
  mysql_q b -e "FLUSH TABLES;" 2>/dev/null
  sleep 3
  local rows fill_rss min r red
  rows=$(mysql_q b -sN -e "SELECT COUNT(*) FROM t" 2>/dev/null)
  fill_rss=$(rss "$mpid"); min=$fill_rss
  echo "$lab: post-fill+flush RSS=${fill_rss}MB rows=$rows compressed_now=$(zc)"
  for i in $(seq 1 45); do sleep 1; r=$(rss "$mpid"); [ -n "$r" ] && [ "$r" -lt "$min" ] && min=$r; done
  red=0; [ "${fill_rss:-0}" -gt 0 ] && red=$(( (fill_rss-min)*100/fill_rss ))
  echo "$lab: RESULT fill=${fill_rss}MB min=${min}MB reduction=${red}% smash_$(zc)"
  kill -9 "$mpid" 2>/dev/null; sleep 1
}

echo "### default (adaptive flushing on)"
run default
echo "### quiesced (no adaptive flush / no neighbor flush / low io cap / no bp dump / 0 dirty)"
run quiesced --skip-innodb-adaptive-flushing --innodb-flush-neighbors=0 \
    --innodb-io-capacity=100 --innodb-lru-scan-depth=128 --innodb-buffer-pool-dump-pct=0 \
    --innodb-max-dirty-pages-pct=0 --innodb-max-dirty-pages-pct-lwm=0
echo DONE
