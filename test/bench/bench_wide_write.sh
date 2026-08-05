#!/usr/bin/env bash
#
# Spec 064 reconnaissance — the write path on a WIDE table of mixed types.
#
# The spec-057 numbers were all on narrow, uniform tables, which is the shape
# that flatters the columnar path: one family, one decision. A real load is wide
# and mixed, and the path is chosen PER CHUNK for the whole chunk — so one
# awkward column decides for the other forty. This measures that.
#
# Three axes, because each moves a different lever:
#
#   threads   1 vs 4    — parallel writers (server-side ingest)
#   strings   plain vs MSSQL_NVARCHAR(n)  — sized vs MAX on the target
#   nulls     none vs some — the strided/cursor boundary is literally
#                           `all_valid && !has_variable`, so ONE NULL in ONE
#                           fixed-width column moves the whole chunk
#
# MSSQL_COUNTERS=1 with MSSQL_DEBUG unset: MSSQL_DEBUG logs from inside the
# phases it times. Wall clock comes from `.timer on`, and the phase totals it
# prints are SUMMED ACROSS THREADS — they rise with thread count while wall time
# falls, which is not a slowdown.
set -uo pipefail
cd /Users/vgribanov/projects/hugr-lab/mssql-extension

set -a; . ./.env; set +a
DSN="Server=${MSSQL_TEST_HOST},${MSSQL_TEST_PORT};Database=TestDB;User Id=${MSSQL_TEST_USER};Password=${MSSQL_TEST_PASS}"
ROWS=${ROWS:-500000}
OUT=${OUT:-/tmp/bench_wide.log}

# 26 columns across every family the encoder has an arm for, plus the two that
# do not (HUGEINT with generated metadata, and a signed TINYINT into `tinyint`).
# `nn` is the NULL carrier: switched on and off to move the strided/cursor line
# without changing anything else.
gen_source() {
	local nulls="$1"
	local nn_expr="0::INTEGER"
	[ "$nulls" = "nulls" ] && nn_expr="CASE WHEN i % 1000 = 0 THEN NULL ELSE (i % 97)::INTEGER END"
	cat <<SQL
CREATE OR REPLACE TABLE wide_src AS
SELECT
    i                                        AS c_bigint,
    (i % 100)::INTEGER                       AS c_int,
    (i % 1000)::SMALLINT                     AS c_smallint,
    (i % 200)::UTINYINT                      AS c_utinyint,
    (i % 2 = 0)                              AS c_bool,
    (i * 1.5)::FLOAT                         AS c_float,
    (i * 2.25)::DOUBLE                       AS c_double,
    (i % 10000)::DECIMAL(9,2)                AS c_dec9,
    (i % 100000)::DECIMAL(18,4)              AS c_dec18,
    (i % 100000)::DECIMAL(38,10)             AS c_dec38,
    DATE '2020-01-01' + ((i % 365)::INTEGER) AS c_date,
    TIME '12:34:56' + INTERVAL (i % 60) SECOND        AS c_time,
    TIMESTAMP '2020-01-01 00:00:00' + INTERVAL (i % 10000) SECOND AS c_ts,
    (TIMESTAMP '2020-01-01 00:00:00' + INTERVAL (i % 10000) SECOND) AT TIME ZONE 'UTC' AS c_tstz,
    gen_random_uuid()                        AS c_uuid,
    repeat('a', 10) || (i % 100)::VARCHAR    AS c_s1,
    repeat('b', 20) || (i % 100)::VARCHAR    AS c_s2,
    repeat('c', 40) || (i % 100)::VARCHAR    AS c_s3,
    encode(repeat('z', 16))::BLOB            AS c_blob,
    ${nn_expr}                               AS c_nn
FROM range(${ROWS}) t(i);
SQL
}

# The projection: `sized` annotates the three string columns so the target gets
# nvarchar(n) instead of nvarchar(max).
projection() {
	if [ "$1" = "sized" ]; then
		echo "c_bigint, c_int, c_smallint, c_utinyint, c_bool, c_float, c_double, c_dec9, c_dec18, c_dec38, c_date, c_time, c_ts, c_tstz, c_uuid, c_s1::MSSQL_NVARCHAR(20) AS c_s1, c_s2::MSSQL_NVARCHAR(30) AS c_s2, c_s3::MSSQL_NVARCHAR(50) AS c_s3, c_blob, c_nn"
	else
		echo "*"
	fi
}

: > "$OUT"
for nulls in nonulls nulls; do
	for ann in plain sized; do
		for threads in 1 4; do
			cell="threads=${threads} strings=${ann} ${nulls}"
			{
				echo ".timer on"
				echo "SET threads = ${threads};"
				echo "SET mssql_copy_parallel_writers = 0;"
				echo "ATTACH '${DSN}' AS db (TYPE mssql);"
				gen_source "$nulls"
				echo "SELECT CASE WHEN count(*) = ${ROWS} THEN 'FIXTURE OK' ELSE 'FIXTURE BROKEN' END AS f FROM wide_src;"
				echo "SELECT mssql_exec('db','IF OBJECT_ID(''dbo.WideBench'') IS NOT NULL DROP TABLE dbo.WideBench');"
				echo "SELECT '### CELL ${cell}' AS marker;"
				echo "CREATE TABLE db.dbo.WideBench AS SELECT $(projection "$ann") FROM wide_src;"
				echo "SELECT mssql_exec('db','DROP TABLE dbo.WideBench');"
			} > /tmp/bench_wide.sql
			echo "=== $cell ===" >> "$OUT"
			MSSQL_COUNTERS=1 ./build/release/duckdb < /tmp/bench_wide.sql >> "$OUT" 2>&1
		done
	done
done
echo "raw log: $OUT"
