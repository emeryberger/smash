#!/bin/bash
# Validate the extent registry end-to-end: a buffer pool LARGER than the old
# 512 MiB (128K-slot) cap should now be tracked + compressed via one extent.
# Compares a 512 MiB pool (worked before, via hash) against 2 GiB and 4 GiB
# (impossible before — hash capped/hung). Success = nonzero compressed= and a
# real RSS reduction at multi-GB, plus a clean shutdown.
set -u
LIB=/home/emerydb/git/smash_extent/build/libsmash.so
BASE=/home/emerydb/mariadb_bench; DATADIR=$BASE/data; SOCK=$BASE/mysql.sock; LOG=$BASE/ext.log
mysql_q(){ mariadb --socket="$SOCK" -u root "$@"; }
rss(){ awk '/VmRSS/{print int($2/1024)}' /proc/$1/status 2>/dev/null; }
zc(){ strings "$LOG" 2>/dev/null | grep -oE 'compressed=[0-9]+' | tail -1; }

run(){ # $1=label $2=BP_MB $3=ROWS
  local lab="$1" bp="$2" rows="$3"
  pkill -9 mariadbd 2>/dev/null; sleep 2; rm -rf "$DATADIR"; mkdir -p "$DATADIR"
  mariadb-install-db --no-defaults --datadir="$DATADIR" --auth-root-authentication-method=normal >/dev/null 2>&1
  LD_PRELOAD=$LIB SMASH_LARGE_ONLY=1 SMASH_TRACK_EXTERNAL=1 SMASH_COLD_TIMEOUT_SEC=3 SMASH_STATS=1 SMASH_DEBUG=1 \
    /usr/sbin/mariadbd --no-defaults --datadir="$DATADIR" --socket="$SOCK" \
    --skip-networking --skip-grant-tables --innodb-buffer-pool-size=${bp}M \
    --innodb-flush-log-at-trx-commit=0 --innodb-doublewrite=0 \
    --innodb-buffer-pool-load-at-startup=0 --innodb-buffer-pool-dump-at-shutdown=0 >"$LOG" 2>&1 &
  local mpid=$! t0=$SECONDS ok=0
  for i in $(seq 1 60); do mysql_q -e "SELECT 1" >/dev/null 2>&1 && { ok=1; break; }; sleep 1; done
  if [ $ok = 0 ]; then echo "$lab (bp=${bp}M): START HUNG/FAIL"; pkill -9 mariadbd; return; fi
  echo "$lab (bp=${bp}M): started in $((SECONDS-t0))s"
  mysql_q -e "CREATE DATABASE IF NOT EXISTS b;"
  mysql_q b -e "DROP TABLE IF EXISTS t; DROP TABLE IF EXISTS nums;
    CREATE TABLE t(id INT PRIMARY KEY,k INT,pad CHAR(200),payload VARCHAR(700)) ENGINE=InnoDB;
    CREATE TABLE nums(n INT PRIMARY KEY AUTO_INCREMENT);
    INSERT INTO nums VALUES (),(),(),(),(),(),(),(),(),(),(),(),(),(),(),();"
  local c=16
  while [ "$c" -lt "$rows" ]; do mysql_q b -e "INSERT INTO nums(n) SELECT n+$c FROM nums;" 2>/dev/null; c=$((c*2)); done
  mysql_q b -e "INSERT INTO t SELECT n,n MOD 1000,CONCAT('u',n MOD 100),REPEAT(CONCAT('tok',n MOD 50,' '),30) FROM nums WHERE n<=$rows;"
  mysql_q b -e "DROP TABLE nums;"
  mysql_q -e "SET GLOBAL innodb_max_dirty_pages_pct=0;" 2>/dev/null
  mysql_q b -e "FLUSH TABLES;" 2>/dev/null
  sleep 3
  local nrows fill_rss min r red
  nrows=$(mysql_q b -sN -e "SELECT COUNT(*) FROM t" 2>/dev/null)
  fill_rss=$(rss "$mpid"); min=$fill_rss
  for i in $(seq 1 45); do sleep 1; r=$(rss "$mpid"); [ -n "$r" ] && [ "$r" -lt "$min" ] && min=$r; done
  red=0; [ "${fill_rss:-0}" -gt 0 ] && red=$(( (fill_rss-min)*100/fill_rss ))
  echo "$lab (bp=${bp}M): RESULT rows=$nrows fill=${fill_rss}MB min=${min}MB reduction=${red}% smash_$(zc)"
  local t0s=$SECONDS
  mysql_q -e "SHUTDOWN" 2>/dev/null
  local hung=1
  for i in $(seq 1 15); do kill -0 $mpid 2>/dev/null || { hung=0; break; }; sleep 1; done
  [ $hung = 1 ] && { echo "$lab: SHUTDOWN HUNG"; pkill -9 mariadbd; } || echo "$lab: clean shutdown in $((SECONDS-t0s))s"
  sleep 1
}

run baseline_512 512 1500000
run large_2048   2048 4000000
run large_4096   4096 6000000
echo DONE
