#!/bin/bash
# MariaDB smash benchmark — framing #3:
#   A) smash on UNCOMPRESSED tables (SMASH_LARGE_ONLY, transparent RSS reduction)
#   B) MariaDB-native COMPRESSED tables, NO smash (app-level page compression)
#   C) baseline: uncompressed, no smash
# Metric: mariadbd RSS after fill + cool window (working set held resident).
set -u
LIB=/home/emerydb/git/smash_cohort/build_cohort/libsmash.so
BASE=/home/emerydb/mariadb_bench
DATADIR=$BASE/data
SOCK=$BASE/mysql.sock
LOG=$BASE/mariadbd.log
ROWS=${ROWS:-2000000}
BP_MB=${BP_MB:-4096}
COOL=${COOL:-25}
MPID=""

mysql_q(){ mariadb --socket="$SOCK" -u root "$@"; }
rss_of(){ awk '/VmRSS/{print int($2/1024)}' /proc/$1/status 2>/dev/null; }

start_server(){ # $1=preload(0/1)
  local preload=$1
  pkill -9 mariadbd 2>/dev/null; sleep 2
  rm -rf "$DATADIR"; mkdir -p "$DATADIR"
  mariadb-install-db --no-defaults --datadir="$DATADIR" --auth-root-authentication-method=normal >/dev/null 2>&1
  if [ "$preload" = "1" ]; then
    LD_PRELOAD=$LIB SMASH_COLD_TIMEOUT_SEC=4 SMASH_LARGE_ONLY=1 \
      /usr/sbin/mariadbd --no-defaults --datadir="$DATADIR" --socket="$SOCK" \
      --skip-networking --skip-grant-tables --innodb-buffer-pool-size=${BP_MB}M \
      --innodb-flush-log-at-trx-commit=0 --innodb-doublewrite=0 \
      --innodb-buffer-pool-load-at-startup=0 --innodb-buffer-pool-dump-at-shutdown=0 \
      >"$LOG" 2>&1 &
  else
    /usr/sbin/mariadbd --no-defaults --datadir="$DATADIR" --socket="$SOCK" \
      --skip-networking --skip-grant-tables --innodb-buffer-pool-size=${BP_MB}M \
      --innodb-flush-log-at-trx-commit=0 --innodb-doublewrite=0 \
      --innodb-buffer-pool-load-at-startup=0 --innodb-buffer-pool-dump-at-shutdown=0 \
      >"$LOG" 2>&1 &
  fi
  MPID=$!
  for i in $(seq 1 60); do mysql_q -e "SELECT 1" >/dev/null 2>&1 && return 0; sleep 1; done
  echo "START FAIL"; tail -6 "$LOG"; return 1
}
stop_server(){ mysql_q -e "SHUTDOWN" >/dev/null 2>&1; wait $MPID 2>/dev/null; }

fill(){ # $1=row_format clause; fast doubling loader (set-based, no recursion)
  local fmt="$1"
  mysql_q -e "CREATE DATABASE IF NOT EXISTS b;"
  mysql_q b -e "DROP TABLE IF EXISTS t; DROP TABLE IF EXISTS nums;
    CREATE TABLE t(id INT PRIMARY KEY, k INT, pad CHAR(200), payload VARCHAR(700)) $fmt;
    CREATE TABLE nums(n INT PRIMARY KEY AUTO_INCREMENT);
    INSERT INTO nums VALUES (),(),(),(),(),(),(),(),(),(),(),(),(),(),(),();"
  local cnt=16
  while [ "$cnt" -lt "$ROWS" ]; do
    mysql_q b -e "INSERT INTO nums(n) SELECT n+$cnt FROM nums;" 2>/dev/null
    cnt=$((cnt*2))
  done
  # Compressible payload: repeated low-entropy tokens (mimics real records).
  mysql_q b -e "INSERT INTO t
    SELECT n, n MOD 1000, CONCAT('user_record_', n MOD 100),
      REPEAT(CONCAT('tok', n MOD 50, ' '), 30)
    FROM nums WHERE n <= $ROWS;"
  mysql_q b -e "DROP TABLE nums;"
}

run_config(){ # $1=label $2=preload $3=row_format_clause
  local lab="$1" pl="$2" fmt="$3"
  start_server "$pl" || { echo "RESULT $lab START_FAIL"; return 1; }
  fill "$fmt" >/dev/null 2>&1
  local n dl fill_rss min r red smash_maps=0
  n=$(mysql_q b -sN -e "SELECT COUNT(*) FROM t" 2>/dev/null)
  dl=$(mysql_q b -sN -e "SELECT ROUND(SUM(data_length+index_length)/1048576) FROM information_schema.tables WHERE table_schema='b'" 2>/dev/null)
  [ "$pl" = "1" ] && smash_maps=$(grep -c libsmash /proc/$MPID/maps 2>/dev/null)
  fill_rss=$(rss_of $MPID); min=$fill_rss
  for i in $(seq 1 ${COOL}); do sleep 1; r=$(rss_of $MPID); [ -n "$r" ] && [ "$r" -lt "$min" ] && min=$r; done
  red=0; [ "${fill_rss:-0}" -gt 0 ] && red=$(( (fill_rss-min)*100/fill_rss ))
  echo "RESULT $lab rows=$n on_disk_mb=$dl fill_rss_mb=$fill_rss min_rss_mb=$min reduction_pct=$red smash_maps=$smash_maps"
  stop_server
}

MODE="${1:-all}"
echo "### MariaDB smash bench: rows=$ROWS bp=${BP_MB}MB cool=${COOL}s mode=$MODE lib=$LIB"
case "$MODE" in
  baseline) run_config "C_baseline_uncompressed_nosmash" 0 "ENGINE=InnoDB" ;;
  smash)    run_config "A_uncompressed_SMASH"             1 "ENGINE=InnoDB" ;;
  native)   run_config "B_native_COMPRESSED_nosmash"      0 "ENGINE=InnoDB ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8" ;;
  all)
    run_config "C_baseline_uncompressed_nosmash" 0 "ENGINE=InnoDB"
    run_config "A_uncompressed_SMASH"            1 "ENGINE=InnoDB"
    run_config "B_native_COMPRESSED_nosmash"     0 "ENGINE=InnoDB ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8"
    ;;
esac
pkill -9 mariadbd 2>/dev/null
echo "### DONE"
