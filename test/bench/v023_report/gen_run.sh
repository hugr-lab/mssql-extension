#!/usr/bin/env bash
# Generated-source runs (the F0 workaround pair). Usage:
#   gen_run.sh <binary> <w|ctas> <rows|full> <tag>
# Appends "tag,gen-<kind>,<bin>,real,user,sys" to $RESULTS like run_variant.sh.
set -u
SCRATCH="$(cd "$(dirname "$0")" && pwd)"
RESULTS="${RESULTS:-$SCRATCH/results.csv}"
BIN="$1"; KIND="$2"; ROWS="$3"; TAG="$4"
PASS_SQL="${MSSQL_TEST_PASS//\'/\'\'}"
DSN="Server=${MSSQL_TEST_HOST},${MSSQL_TEST_PORT};Database=TestDB;User Id=${MSSQL_TEST_USER};Password=${PASS_SQL}"

GEN_SQL="$(cat "$SCRATCH/gen_source.sql")"
if [ "$ROWS" != "full" ]; then GEN_SQL="${GEN_SQL/range(38000000)/range($ROWS)}"; fi

# Probe, not path substring: a static build resolves mssql_version() without LOAD.
if "$BIN" -c "SELECT mssql_version();" >/dev/null 2>&1; then
  LOAD_LINE=""
else
  LOAD_LINE="LOAD mssql;"
fi

case "$KIND" in
  w)    STMT="COPY ($GEN_SQL) TO 'db.dbo.WB' (FORMAT bcp, CREATE_TABLE true, REPLACE true);";;
  ctas) STMT="CREATE OR REPLACE TABLE db.dbo.WCTAS AS $GEN_SQL;";;
  *) echo "usage: gen_run.sh <bin> w|ctas <rows|full> <tag>" >&2; exit 2;;
esac

LOG="$SCRATCH/logs/$(date +%H%M%S)_${TAG}_gen_$(basename "$BIN").log"
mkdir -p "$SCRATCH/logs"
GEN_SETUP="${GEN_SETUP:-}"
"$BIN" >"$LOG" 2>&1 <<EOF
.timer on
$LOAD_LINE
ATTACH '$DSN' AS db (TYPE mssql);
$GEN_SETUP
$STMT
EOF
rc=$?
rt_line=$(grep 'Run Time' "$LOG" | tail -1)
n_rt=$(grep -c 'Run Time' "$LOG")
if [ -n "$LOAD_LINE" ]; then min_rt=3; else min_rt=2; fi
if [ $rc -ne 0 ] || [ "$n_rt" -lt "$min_rt" ]; then
  echo "FAIL $TAG gen-$KIND $BIN rc=$rc rt_lines=$n_rt — see $LOG" >&2
  tail -5 "$LOG" >&2
  exit 1
fi
read -r real user sys <<<"$(echo "$rt_line" | sed -E 's/.*real ([0-9.]+) user ([0-9.]+) sys ([0-9.]+).*/\1 \2 \3/')"
echo "$TAG,gen-$KIND,$(basename "$(dirname "$BIN")")/$(basename "$BIN"),$real,$user,$sys" >>"$RESULTS"
echo "OK  $TAG gen-$KIND real=$real user=$user sys=$sys"
