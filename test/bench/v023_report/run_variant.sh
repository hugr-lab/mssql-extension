#!/usr/bin/env bash
# v0.2.3 final report — single-variant runner.
# Usage: run_variant.sh <binary> <variant> <rows|full> <tag>
#   binary : path to a duckdb CLI (stock needs LOAD; candidate is static)
#   variant: w-plain-N | w-maxlen | w-nv | w-vc-cp | w-vc-utf8 | w-cci | w-cci-vc
#            | ctas | r-grp | r-cols | r-parquet
#   rows   : row limit for smoke, or "full"
#   tag    : label written into the results CSV
# Env: MSSQL_TEST_HOST/PORT/USER/PASS set by caller (never printed).
# Results: appends "tag,variant,binary,real,user,sys" to $RESULTS (last Run Time
# line of the process = the measured statement; earlier lines are ATTACH/SETs).
set -u
SCRATCH="$(cd "$(dirname "$0")" && pwd)"
FIXTURE="${F38_FIXTURE:-$SCRATCH/f38_fixture.parquet}"
RESULTS="${RESULTS:-$SCRATCH/results.csv}"
BIN="$1"; VARIANT="$2"; ROWS="$3"; TAG="$4"

# Single quotes doubled so a quote in the password cannot truncate the ATTACH
# literal (which would echo the DSN into the error message).
PASS_SQL="${MSSQL_TEST_PASS//\'/\'\'}"
DSN="Server=${MSSQL_TEST_HOST},${MSSQL_TEST_PORT};Database=TestDB;User Id=${MSSQL_TEST_USER};Password=${PASS_SQL}"
SRC="read_parquet('$FIXTURE')"
if [ "$ROWS" != "full" ]; then SRC="(FROM read_parquet('$FIXTURE') LIMIT $ROWS)"; fi

# A baseline CLI needs `LOAD mssql` (community install); the static candidate
# has it registered at startup. Decided by PROBING, not by path substring: if
# mssql_version() resolves without LOAD, the binary is a static build.
if ! "$BIN" -c "SELECT 1" >/dev/null 2>&1; then echo "bad binary $BIN" >&2; exit 2; fi
if "$BIN" -c "SELECT mssql_version();" >/dev/null 2>&1; then
  LOAD_LINE=""
else
  LOAD_LINE="LOAD mssql;"
fi

# String-column REPLACE lists (F38Heap: 9 nvarchar cols incl n_half;
# s_cyr/s_cjk are NOT CP1252-representable, so the CP variant keeps them nvarchar).
NV_ALL="s_ann_nv::MSSQL_NVARCHAR(200) AS s_ann_nv, s_ann_vc::MSSQL_NVARCHAR(200) AS s_ann_vc, s_plain::MSSQL_NVARCHAR(200) AS s_plain, s_cyr::MSSQL_NVARCHAR(200) AS s_cyr, s_cjk::MSSQL_NVARCHAR(200) AS s_cjk, s_max::MSSQL_NVARCHAR(200) AS s_max, s_vcmax::MSSQL_NVARCHAR(200) AS s_vcmax, s_char::MSSQL_NVARCHAR(200) AS s_char, n_half::MSSQL_NVARCHAR(200) AS n_half"
VC_ALL="s_ann_nv::MSSQL_VARCHAR(200) AS s_ann_nv, s_ann_vc::MSSQL_VARCHAR(200) AS s_ann_vc, s_plain::MSSQL_VARCHAR(200) AS s_plain, s_cyr::MSSQL_VARCHAR(200) AS s_cyr, s_cjk::MSSQL_VARCHAR(200) AS s_cjk, s_max::MSSQL_VARCHAR(200) AS s_max, s_vcmax::MSSQL_VARCHAR(200) AS s_vcmax, s_char::MSSQL_VARCHAR(200) AS s_char, n_half::MSSQL_VARCHAR(200) AS n_half"
VC_CP="s_ann_nv::MSSQL_VARCHAR(200) AS s_ann_nv, s_ann_vc::MSSQL_VARCHAR(200) AS s_ann_vc, s_plain::MSSQL_VARCHAR(200) AS s_plain, s_cyr::MSSQL_NVARCHAR(200) AS s_cyr, s_cjk::MSSQL_NVARCHAR(200) AS s_cjk, s_max::MSSQL_VARCHAR(200) AS s_max, s_vcmax::MSSQL_VARCHAR(200) AS s_vcmax, s_char::MSSQL_VARCHAR(200) AS s_char, n_half::MSSQL_VARCHAR(200) AS n_half"

# A failed r-parquet run must not leave a multi-GB export behind.
trap 'rm -f "$SCRATCH/r_export_out.parquet"' EXIT

SETUP=""; STMT=""
case "$VARIANT" in
  w-plain-*)
    N="${VARIANT##*-}"
    SETUP="SET mssql_copy_parallel_writers = $N;"
    STMT="COPY (FROM $SRC) TO 'db.dbo.WB' (FORMAT bcp, CREATE_TABLE true, REPLACE true);";;
  w-plain0)
    # Baseline form: no settings 0.2.2 lacks. Not referenced by campaign.sh —
    # it exists for the F0 bisect (baseline at LIMIT sizes, where 0.2.2 still
    # survives the parquet source; see report finding F0).
    STMT="COPY (FROM $SRC) TO 'db.dbo.WB' (FORMAT bcp, CREATE_TABLE true, REPLACE true);";;
  w-maxlen)
    SETUP="SET mssql_copy_parallel_writers = 4; SET mssql_default_string_length = 200;"
    STMT="COPY (FROM $SRC) TO 'db.dbo.WB' (FORMAT bcp, CREATE_TABLE true, REPLACE true);";;
  w-nv)
    SETUP="SET mssql_copy_parallel_writers = 4;"
    STMT="COPY (SELECT * REPLACE ($NV_ALL) FROM $SRC) TO 'db.dbo.WB' (FORMAT bcp, CREATE_TABLE true, REPLACE true);";;
  w-vc-cp)
    SETUP="SET mssql_copy_parallel_writers = 4; SET mssql_utf8_collation = '';"
    STMT="COPY (SELECT * REPLACE ($VC_CP) FROM $SRC) TO 'db.dbo.WB' (FORMAT bcp, CREATE_TABLE true, REPLACE true);";;
  w-vc-utf8)
    SETUP="SET mssql_copy_parallel_writers = 4;"
    STMT="COPY (SELECT * REPLACE ($VC_ALL) FROM $SRC) TO 'db.dbo.WB' (FORMAT bcp, CREATE_TABLE true, REPLACE true);";;
  w-cci)
    SETUP="SET mssql_copy_parallel_writers = 4;"
    STMT="COPY (FROM $SRC) TO 'db.dbo.WB' (FORMAT bcp, CREATE_TABLE true, REPLACE true, table_kind 'columnstore');";;
  w-cci-vc)
    SETUP="SET mssql_copy_parallel_writers = 4;"
    STMT="COPY (SELECT * REPLACE ($VC_ALL) FROM $SRC) TO 'db.dbo.WB' (FORMAT bcp, CREATE_TABLE true, REPLACE true, table_kind 'columnstore');";;
  ctas)
    STMT="CREATE OR REPLACE TABLE db.dbo.WCTAS AS FROM $SRC;";;
  r-grp)
    STMT="SELECT i8, count(*), sum(i64), avg(f8), min(s_plain), max(d18) FROM db.dbo.F38Heap GROUP BY i8 ORDER BY i8 LIMIT 3;";;
  r-cols)
    STMT="SELECT min(i32), min(i64), min(f8), min(d18), min(s_plain), min(s_cyr), min(ts7), min(gu) FROM db.dbo.F38Heap;";;
  r-parquet)
    STMT="COPY (SELECT * FROM db.dbo.F38Heap) TO '$SCRATCH/r_export_out.parquet';";;
  *) echo "unknown variant $VARIANT" >&2; exit 2;;
esac

LOG="$SCRATCH/logs/$(date +%H%M%S)_${TAG}_$(basename "$BIN").log"
mkdir -p "$SCRATCH/logs"
"$BIN" >"$LOG" 2>&1 <<EOF
.timer on
$LOAD_LINE
ATTACH '$DSN' AS db (TYPE mssql);
$SETUP
$STMT
EOF
rc=$?
rt_line=$(grep 'Run Time' "$LOG" | tail -1)
if [ $rc -ne 0 ] || [ -z "$rt_line" ]; then
  echo "FAIL $TAG $VARIANT $BIN rc=$rc — see $LOG" >&2
  # Scrub the DSN before echoing: an ATTACH parse error quotes the statement,
  # password included.
  tail -5 "$LOG" | sed -E 's/Password=[^;'"'"']*/Password=***/g' >&2
  exit 1
fi
read -r real user sys <<<"$(echo "$rt_line" | sed -E 's/.*real ([0-9.]+) user ([0-9.]+) sys ([0-9.]+).*/\1 \2 \3/')"
echo "$TAG,$VARIANT,$(basename "$(dirname "$BIN")")/$(basename "$BIN"),$real,$user,$sys" >>"$RESULTS"
echo "OK  $TAG $VARIANT real=$real user=$user sys=$sys"
