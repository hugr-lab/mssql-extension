#!/usr/bin/env bash
#
# Spec 064 reconnaissance — the write path on a WIDE table of mixed types.
#
# The spec-057 numbers were all on narrow, uniform tables, which is the shape
# that flatters the columnar path: one family, one decision. A real load is wide
# and mixed, and the path is chosen PER CHUNK for the whole chunk — so one
# awkward column decides for the other nineteen. This measures that.
#
# The fixture is 20 columns, one per family the encoder has an arm for
# (integers, unsigned, bool, float/double, three decimal buckets, date, time,
# timestamp, timestamptz, uuid, three strings, blob, and the NULL carrier).
# Deliberately absent: pairs the encoder REFUSES (a signed TINYINT into
# `tinyint`, spec 057) — a refused column would poison every cell the same way
# and measure nothing.
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
set -euo pipefail
cd "$(dirname "$0")/../.."

set -a; . ./.env; set +a
DSN="Server=${MSSQL_TEST_HOST},${MSSQL_TEST_PORT};Database=TestDB;User Id=${MSSQL_TEST_USER};Password=${MSSQL_TEST_PASS}"
ROWS=${ROWS:-500000}
OUT=${OUT:-/tmp/bench_wide.log}
# A/B against another binary: BIN overrides the CLI; PRELUDE_SQL (e.g.
# "LOAD mssql;") is emitted first for binaries without the extension linked in.
BIN=${BIN:-./build/release/duckdb}
PRELUDE_SQL=${PRELUDE_SQL:-}

# The SQL file carries the DSN (password included): mktemp gives it 0600 and the
# trap removes it however the run ends — a fixed /tmp path outlived the run and
# was world-readable, which is how a bench script leaks credentials.
SQLFILE=$(mktemp "${TMPDIR:-/tmp}/bench_wide.XXXXXX.sql")
CELLLOG=$(mktemp "${TMPDIR:-/tmp}/bench_wide.XXXXXX.cell")
trap 'rm -f "$SQLFILE" "$CELLLOG"' EXIT

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
    (TIMESTAMP '2020-01-01 00:00:00' + INTERVAL (i % 10000) SECOND)::TIMESTAMPTZ AS c_tstz,
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
				echo ".timer on"; [ -n "$PRELUDE_SQL" ] && echo "$PRELUDE_SQL"
				echo "SET threads = ${threads};"
				echo "SET mssql_copy_parallel_writers = 0;"
				echo "ATTACH '${DSN}' AS db (TYPE mssql);"
				gen_source "$nulls"
				echo "SELECT CASE WHEN count(*) = ${ROWS} AND count(c_uuid) = ${ROWS} THEN 'FIXTURE OK' ELSE 'FIXTURE BROKEN' END AS f FROM wide_src;"
				echo "SELECT mssql_exec('db','IF OBJECT_ID(''dbo.WideBench'') IS NOT NULL DROP TABLE dbo.WideBench');"
				echo "SELECT '### CELL ${cell}' AS marker;"
				echo "CREATE TABLE db.dbo.WideBench AS SELECT $(projection "$ann") FROM wide_src;"
				echo "SELECT mssql_exec('db','DROP TABLE dbo.WideBench');"
			} > "$SQLFILE"
			echo "=== $cell ===" >> "$OUT"
			# A cell that errors, or whose fixture check did not print FIXTURE OK,
			# aborts the whole run BY NAME — an 8-cell matrix that produced zero
			# numbers must not exit 0 (the `[mssql]`-filter lesson).
			if ! MSSQL_COUNTERS=1 "$BIN" < "$SQLFILE" > "$CELLLOG" 2>&1; then
				cat "$CELLLOG" >> "$OUT"
				echo "CELL FAILED: $cell (see $OUT)" >&2
				exit 1
			fi
			cat "$CELLLOG" >> "$OUT"
			if ! grep -q 'FIXTURE OK' "$CELLLOG" || grep -q 'Error' "$CELLLOG"; then
				echo "CELL INVALID (fixture or error): $cell (see $OUT)" >&2
				exit 1
			fi
		done
	done
done
echo "raw log: $OUT"
