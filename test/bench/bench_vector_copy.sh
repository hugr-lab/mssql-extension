#!/usr/bin/env bash
#
# Spec 093 T3 -- the bulk write of a vector(n) column, before and after its columnar scatter arm.
#
# Before the arm, a chunk holding a vector column went row-major for EVERY column of the chunk (the
# scatter had no arm for it), each value through sql_vector::EncodeToBcp. After it, the chunk stays
# columnar: one kernel call per column per block -- for float32 the header and a memcpy of the row's
# elements, for float16 a narrowing loop. This times a COPY of ROWS rows into two existing heaps:
#
#   bench_vec        (id int, v vector(DIMS))
#   bench_vec_mixed  (id int, s varchar(100), v vector(DIMS))
#
# with each binary, interleaved run by run (BEFORE then AFTER, then AFTER then BEFORE, ...), so a drift
# of the server or the machine lands on both. The source is generated once into a DuckDB file and read
# by every run, outside the timing. MSSQL_COUNTERS=1 (not MSSQL_DEBUG, which logs inside the phases it
# times) prints each COPY's write-path counters -- chunks_strided / chunks_cursor / chunks_row_major --
# into the log, which says which path the chunks took. Each run's row count is checked on the server:
# the DuckDB CLI at this pin can exit 0 with nothing printed on a runtime error.
#
# Needs SQL Server 2025 (the vector type). The connection: MSSQL_TESTDB_DSN, or MSSQL_TEST_HOST /
# _PORT / _USER / _PASS (TestDB) from the environment or ./.env. It goes into a 0600 temp file the CLI
# reads on stdin, and is never printed.
#
#   BEFORE_BIN=/path/to/duckdb-at-7246bcfa AFTER_BIN=./build/release/duckdb test/bench/bench_vector_copy.sh
#
# Environment: BEFORE_BIN / AFTER_BIN (DuckDB CLIs with the extension linked in -- a release build's
# build/release/duckdb), ROWS (1000000), DIMS (768), RUNS (3), WORK (a directory for the source file and
# the log; default a temp directory, kept), THREADS (DuckDB threads; default DuckDB's).
set -euo pipefail
cd "$(dirname "$0")/../.."

: "${BEFORE_BIN:?set BEFORE_BIN to the DuckDB CLI built at the commit before the arm (7246bcfa)}"
: "${AFTER_BIN:?set AFTER_BIN to the DuckDB CLI built with the arm}"
ROWS=${ROWS:-1000000}
DIMS=${DIMS:-768}
RUNS=${RUNS:-3}
WORK=${WORK:-$(mktemp -d "${TMPDIR:-/tmp}/bench_vector.XXXXXX")}
THREADS=${THREADS:-}
mkdir -p "$WORK"
LOG="$WORK/bench_vector_copy.log"
SRC="$WORK/bench_vector_src_${ROWS}_${DIMS}.duckdb"

if [ -z "${MSSQL_TESTDB_DSN:-}" ]; then
	if [ -f ./.env ]; then
		set -a
		. ./.env
		set +a
	fi
	: "${MSSQL_TEST_HOST:?set MSSQL_TESTDB_DSN, or MSSQL_TEST_HOST / _PORT / _USER / _PASS}"
	MSSQL_TESTDB_DSN="Server=${MSSQL_TEST_HOST},${MSSQL_TEST_PORT:-1433};Database=TestDB;User Id=${MSSQL_TEST_USER};Password=${MSSQL_TEST_PASS};TrustServerCertificate=yes"
fi

SQLFILE=$(mktemp "${TMPDIR:-/tmp}/bench_vector.XXXXXX.sql")
OUTFILE=$(mktemp "${TMPDIR:-/tmp}/bench_vector.XXXXXX.out")
trap 'rm -f "$SQLFILE" "$OUTFILE"' EXIT
: >"$LOG"

attach_sql() {
	printf "ATTACH '%s' AS bv (TYPE mssql);\n" "$MSSQL_TESTDB_DSN"
	[ -n "$THREADS" ] && printf "SET threads = %s;\n" "$THREADS"
	return 0
}

run_sql() {	# $1 = binary; the SQL in $SQLFILE; output in $OUTFILE
	MSSQL_COUNTERS=1 "$1" -unsigned -no-agent <"$SQLFILE" >"$OUTFILE" 2>&1 || true
	cat "$OUTFILE" >>"$LOG"
}

# --- the source, once: id, s, and a FLOAT[DIMS] of varied values (no NULL)
if [ ! -f "$SRC" ]; then
	echo "generating $ROWS rows of FLOAT[$DIMS] into $SRC ..."
	cat >"$SQLFILE" <<SQL
ATTACH '$SRC' AS src;
CREATE TABLE src.t AS
SELECT i::INTEGER AS id, 'row ' || i::VARCHAR AS s,
       list_transform(range($DIMS), lambda x: ((i * 31 + x * 7) % 1000)::FLOAT / 997.0)::FLOAT[$DIMS] AS v
FROM range($ROWS) r(i);
SELECT 'generated=' || count(*) FROM src.t;
SQL
	run_sql "$AFTER_BIN"
	grep -q "generated=$ROWS" "$OUTFILE" || { echo "generation failed, see $LOG" >&2; exit 1; }
fi

# --- the targets
{
	attach_sql
	cat <<SQL
SELECT mssql_exec('bv', 'DROP TABLE IF EXISTS dbo.bench_vec; DROP TABLE IF EXISTS dbo.bench_vec_mixed; CREATE TABLE dbo.bench_vec (id int NOT NULL, v vector($DIMS) NULL); CREATE TABLE dbo.bench_vec_mixed (id int NOT NULL, s varchar(100) NULL, v vector($DIMS) NULL)');
SQL
} >"$SQLFILE"
run_sql "$AFTER_BIN"

# --- one timed COPY: $1 binary, $2 table, $3 the source projection
copy_once() {
	{
		attach_sql
		printf "ATTACH '%s' AS src (READ_ONLY);\n" "$SRC"
		printf "SELECT mssql_exec('bv', 'TRUNCATE TABLE dbo.%s');\n" "$2"
		printf ".timer on\n"
		printf "COPY (SELECT %s FROM src.t) TO 'mssql://bv/dbo/%s' (FORMAT 'bcp', CREATE_TABLE false);\n" "$3" "$2"
		printf ".timer off\n"
		printf "SELECT 'loaded=' || loaded FROM mssql_scan('bv', 'SELECT COUNT_BIG(*) AS loaded FROM dbo.%s');\n" "$2"
	} >"$SQLFILE"
	echo "=== $(basename "$(dirname "$(dirname "$1")")")/$(basename "$1") $2" >>"$LOG"
	run_sql "$1"
	local real loaded
	real=$(grep -oE 'Run Time \(s\): real [0-9.]+' "$OUTFILE" | tail -1 | awk '{print $5}')
	loaded=$(grep -oE "loaded=[0-9]+" "$OUTFILE" | tail -1 | cut -d= -f2)
	if [ -z "$real" ] || [ "$loaded" != "$ROWS" ]; then
		echo "FAIL"
	else
		echo "$real"
	fi
}

declare -a TIMES
printf "%-6s %-17s %-8s %s\n" run table binary "wall (s)"
for run in $(seq 1 "$RUNS"); do
	if [ $((run % 2)) -eq 1 ]; then order="BEFORE AFTER"; else order="AFTER BEFORE"; fi
	for which in $order; do
		bin=$([ "$which" = BEFORE ] && echo "$BEFORE_BIN" || echo "$AFTER_BIN")
		t1=$(copy_once "$bin" bench_vec "id, v")
		t2=$(copy_once "$bin" bench_vec_mixed "id, s, v")
		printf "%-6s %-17s %-8s %s\n" "$run" bench_vec "$which" "$t1"
		printf "%-6s %-17s %-8s %s\n" "$run" bench_vec_mixed "$which" "$t2"
		TIMES+=("$which bench_vec $t1" "$which bench_vec_mixed $t2")
	done
done

echo
echo "median wall (s), $RUNS run(s), $ROWS rows of FLOAT[$DIMS]:"
for which in BEFORE AFTER; do
	for table in bench_vec bench_vec_mixed; do
		median=$(printf "%s\n" "${TIMES[@]}" | awk -v w="$which" -v t="$table" '$1 == w && $2 == t && $3 != "FAIL" {print $3}' |
			sort -n | awk '{a[NR] = $1} END {if (NR == 0) print "n/a"; else if (NR % 2) print a[(NR + 1) / 2]; else print (a[NR / 2] + a[NR / 2 + 1]) / 2}')
		printf "  %-7s %-17s %s\n" "$which" "$table" "$median"
	done
done
echo
echo "write-path counters per COPY (chunks_strided / chunks_cursor / chunks_row_major) are in $LOG"

# The targets go; the source file stays in $WORK for another run.
{
	attach_sql
	echo "SELECT mssql_exec('bv', 'DROP TABLE IF EXISTS dbo.bench_vec; DROP TABLE IF EXISTS dbo.bench_vec_mixed');"
} >"$SQLFILE"
run_sql "$AFTER_BIN"
