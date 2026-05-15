#!/usr/bin/env bash
# test/bench/bench_codec_e2e.sh
#
# End-to-end before/after benchmark for spec 044 (UTF-16 Codec Consolidation).
# Executes a six-step workflow against the Docker SQL Server image used by
# `make integration-test`, timing each step. Outputs per-step wall-clock
# in a fixed `step<TAB>seconds<TAB>row_count<TAB>notes` format.
#
# Manual target (FR-056); NOT run as part of `make test` or any CI workflow.
#
# Env vars:
#   MSSQL_BENCH_DUCKDB_BIN  path to a built DuckDB CLI with the mssql
#                           extension loadable (REQUIRED)
#   MSSQL_TEST_HOST         default: localhost
#   MSSQL_TEST_PORT         default: 1433
#   MSSQL_TEST_USER         default: sa
#   MSSQL_TEST_PASS         default: TestPassword1
#   MSSQL_TEST_DB           default: TestDB
#   MSSQL_BENCH_ROW_COUNT   default: 100000000 (100M)
#                           Override smaller for developer iteration; the
#                           INSERT-VALUES step is capped at min(100k, override).
#   MSSQL_BENCH_OUTPUT      default: /tmp/bench_codec_e2e_<epoch>.txt
#
# Output:
#   step_name<TAB>seconds<TAB>row_count<TAB>notes
#   (one line per step + a trailing summary block with host metadata)

set -uo pipefail

DUCKDB_BIN="${MSSQL_BENCH_DUCKDB_BIN:-}"
if [ -z "$DUCKDB_BIN" ]; then
	echo "ERROR: MSSQL_BENCH_DUCKDB_BIN must be set to the path of a built DuckDB CLI with the mssql extension." >&2
	echo "Example: MSSQL_BENCH_DUCKDB_BIN=./build/release/duckdb bash test/bench/bench_codec_e2e.sh" >&2
	exit 2
fi
if [ ! -x "$DUCKDB_BIN" ]; then
	echo "ERROR: $DUCKDB_BIN is not executable." >&2
	exit 2
fi

HOST="${MSSQL_TEST_HOST:-localhost}"
PORT="${MSSQL_TEST_PORT:-1433}"
USER="${MSSQL_TEST_USER:-sa}"
PASS="${MSSQL_TEST_PASS:-TestPassword1}"
DB="${MSSQL_TEST_DB:-TestDB}"

ROW_COUNT_DEFAULT=100000000
ROW_COUNT="${MSSQL_BENCH_ROW_COUNT:-$ROW_COUNT_DEFAULT}"
# INSERT-VALUES is bounded because it does not exercise the per-cell UTF-16
# codec (the whole SQL_BATCH text is encoded once per batch); pushing it to
# 100M would burn hours of SQL Server insert work with no additional codec
# signal. See spec User Story 5 step 3 rationale and FR-051.
INSERT_ROW_CAP=100000
INSERT_ROWS=$(( ROW_COUNT < INSERT_ROW_CAP ? ROW_COUNT : INSERT_ROW_CAP ))

OUTPUT_DEFAULT="/tmp/bench_codec_e2e_$(date +%s).txt"
OUTPUT_FILE="${MSSQL_BENCH_OUTPUT:-$OUTPUT_DEFAULT}"

DSN="Server=${HOST},${PORT};Database=${DB};User Id=${USER};Password=${PASS}"

# Resolve how to load the mssql extension. If MSSQL_BENCH_EXTENSION_PATH is
# set (absolute path to a `mssql.duckdb_extension` file), LOAD it directly.
# Otherwise default to the standard `INSTALL mssql; LOAD mssql` flow.
if [ -n "${MSSQL_BENCH_EXTENSION_PATH:-}" ]; then
	if [ ! -r "$MSSQL_BENCH_EXTENSION_PATH" ]; then
		echo "ERROR: MSSQL_BENCH_EXTENSION_PATH=$MSSQL_BENCH_EXTENSION_PATH is not readable." >&2
		exit 2
	fi
	MSSQL_LOAD="LOAD '${MSSQL_BENCH_EXTENSION_PATH}'"
else
	MSSQL_LOAD="INSTALL mssql; LOAD mssql"
fi

echo "[bench_codec_e2e] DuckDB CLI: $DUCKDB_BIN"
echo "[bench_codec_e2e] DSN: Server=${HOST},${PORT};Database=${DB};User Id=${USER};Password=***"
echo "[bench_codec_e2e] Source row count: ${ROW_COUNT}"
echo "[bench_codec_e2e] INSERT-VALUES row count: ${INSERT_ROWS} (capped at ${INSERT_ROW_CAP})"
echo "[bench_codec_e2e] Output file: ${OUTPUT_FILE}"
echo ""

# Pre-flight: connect via DuckDB CLI, verify the attach + SELECT 1 round trip.
echo "[bench_codec_e2e] Pre-flight: ATTACH + SELECT 1..."
preflight_out=$("$DUCKDB_BIN" -unsigned -c "${MSSQL_LOAD}; ATTACH '${DSN}' AS db (TYPE mssql); SELECT 1 AS ok;" 2>&1)
if ! echo "$preflight_out" | grep -q "ok"; then
	echo "ERROR: pre-flight failed. Last output:" >&2
	echo "$preflight_out" >&2
	exit 3
fi
echo "[bench_codec_e2e] Pre-flight OK."
echo ""

# Idempotent cleanup before each run (FR-053). DROP-if-exists on the three
# target tables.
cleanup_sql() {
	cat <<-EOF
		${MSSQL_LOAD};
		ATTACH '${DSN}' AS db (TYPE mssql);
		DROP TABLE IF EXISTS db.dbo.bench_target_insert;
		DROP TABLE IF EXISTS db.dbo.bench_target_copy;
		DROP TABLE IF EXISTS db.dbo.bench_target_ctas;
		DETACH db;
	EOF
}

echo "[bench_codec_e2e] Cleanup: dropping any existing target tables..."
"$DUCKDB_BIN" -unsigned -c "$(cleanup_sql)" >/dev/null 2>&1 || true

# Time a single command. Args: <step_name> <sql>. Prints to OUTPUT_FILE and
# stdout the per-step timing line.
time_step() {
	local step="$1"
	local sql="$2"
	local rows="$3"
	local notes="$4"
	local t0
	local t1
	local seconds
	t0=$(date +%s.%N)
	if ! "$DUCKDB_BIN" -unsigned -c "$sql" >/dev/null 2>&1; then
		echo "ERROR: step '$step' failed. Aborting." >&2
		"$DUCKDB_BIN" -unsigned -c "$(cleanup_sql)" >/dev/null 2>&1 || true
		exit 4
	fi
	t1=$(date +%s.%N)
	seconds=$(awk "BEGIN {printf \"%.3f\", $t1 - $t0}")
	printf '%s\t%s\t%s\t%s\n' "$step" "$seconds" "$rows" "$notes" | tee -a "$OUTPUT_FILE"
}

# Reset the output file (header only).
{
	echo "# bench_codec_e2e output"
	echo "# date: $(date -Iseconds 2>/dev/null || date)"
	echo "# duckdb_bin: $DUCKDB_BIN"
	echo "# host: ${HOST}:${PORT}/${DB}"
	echo "# row_count: ${ROW_COUNT}"
	echo "# insert_rows: ${INSERT_ROWS}"
	echo "step	seconds	row_count	notes"
} > "$OUTPUT_FILE"

echo "[bench_codec_e2e] Running six-step workflow..."
echo ""

# Step 1: DDL (CREATE TABLE via SQL_BATCH UTF-16 encode hot path).
time_step "ddl_create_tables" "
	${MSSQL_LOAD};
	ATTACH '${DSN}' AS db (TYPE mssql);
	CREATE TABLE db.dbo.bench_target_insert (
		id INT, name VARCHAR, payload VARCHAR, created_at TIMESTAMP
	);
	CREATE TABLE db.dbo.bench_target_copy (
		id INT, name VARCHAR, payload VARCHAR, created_at TIMESTAMP
	);
	DETACH db;
" "-" "create two target tables"

# Step 2: Generate the source table in a DuckDB temp DB. Persist to a
# DuckDB database file so subsequent CLI invocations can reuse it; this
# keeps the bench's CLI-per-step structure (each step is a fresh
# DuckDB process) while sharing the source data across steps.
SRC_DB="/tmp/bench_codec_e2e_src_$$.duckdb"
trap 'rm -f "$SRC_DB"' EXIT
time_step "generate_source" "
	ATTACH '${SRC_DB}' AS src;
	CREATE TABLE src.bench_source AS
	SELECT
		i::INT AS id,
		CASE i % 4
			WHEN 0 THEN 'ascii row ' || i
			WHEN 1 THEN 'Кириллица ' || i
			WHEN 2 THEN '中文行 ' || i
			ELSE '🔒 emoji ' || i
		END AS name,
		repeat('mixed-Unicode payload Αβγ ', 4) AS payload,
		timestamp '2026-01-01' + INTERVAL (i) SECOND AS created_at
	FROM range(0, ${ROW_COUNT}) t(i);
	DETACH src;
" "${ROW_COUNT}" "duckdb temp table"

# Step 3: INSERT via batched VALUES (bounded at ${INSERT_ROWS}; see header
# comment). Exercises tds_protocol.cpp SQL_BATCH UTF-16 encoding.
time_step "insert_values" "
	${MSSQL_LOAD};
	ATTACH '${SRC_DB}' AS src;
	ATTACH '${DSN}' AS db (TYPE mssql);
	INSERT INTO db.dbo.bench_target_insert
		SELECT * FROM src.bench_source LIMIT ${INSERT_ROWS};
	DETACH db;
	DETACH src;
" "${INSERT_ROWS}" "LIMIT ${INSERT_ROWS} (codec amortized per batch)"

# Step 4: CTAS+BCP (DDL + bulk via BCP). Full ${ROW_COUNT}. Exercises
# bcp_row_encoder.cpp's per-cell UTF-16 encode hot path.
time_step "ctas_bcp" "
	${MSSQL_LOAD};
	ATTACH '${SRC_DB}' AS src;
	ATTACH '${DSN}' AS db (TYPE mssql);
	CREATE TABLE db.dbo.bench_target_ctas AS
		SELECT * FROM src.bench_source;
	DETACH db;
	DETACH src;
" "${ROW_COUNT}" "default mssql_ctas_use_bcp=true"

# Step 5: COPY (BCP) into the prepared copy target. Full ${ROW_COUNT}.
# Re-exercises per-cell BCP encode + BCP-writer header encoding.
time_step "copy_bcp" "
	${MSSQL_LOAD};
	ATTACH '${SRC_DB}' AS src;
	ATTACH '${DSN}' AS db (TYPE mssql);
	COPY (SELECT * FROM src.bench_source)
		TO 'mssql://db/dbo/bench_target_copy' (FORMAT 'bcp');
	DETACH db;
	DETACH src;
" "${ROW_COUNT}" "COPY TO MSSQL via BCP"

# Step 6a: SELECT COUNT(*) (smoke check; no NVARCHAR bytes over the wire).
time_step "select_count" "
	${MSSQL_LOAD};
	ATTACH '${DSN}' AS db (TYPE mssql);
	SELECT COUNT(*) FROM db.dbo.bench_target_ctas;
	DETACH db;
" "1" "smoke check (no NVARCHAR scan)"

# Step 6b: SELECT * materialized to /dev/null (or NUL on Windows). This is
# the primary read-side signal — full NVARCHAR scan decode via
# type_converter.cpp's per-cell Utf16LEDecode.
case "$(uname -s 2>/dev/null)" in
	MINGW*|MSYS*|CYGWIN*) NULL_DEV="NUL" ;;
	*)                    NULL_DEV="/dev/null" ;;
esac
time_step "select_full" "
	${MSSQL_LOAD};
	ATTACH '${DSN}' AS db (TYPE mssql);
	COPY (SELECT * FROM db.dbo.bench_target_ctas)
		TO '${NULL_DEV}' (FORMAT csv);
	DETACH db;
" "${ROW_COUNT}" "primary read-side signal (NVARCHAR scan decode)"

# Cleanup (FR-053 idempotency on exit).
echo ""
echo "[bench_codec_e2e] Cleanup: dropping target tables..."
"$DUCKDB_BIN" -unsigned -c "$(cleanup_sql)" >/dev/null 2>&1 || true

# Trailing summary block.
{
	echo ""
	echo "# end of bench_codec_e2e"
	echo "# uname: $(uname -srm 2>/dev/null || uname -a)"
	if [ -r /proc/cpuinfo ]; then
		echo "# cpu_model: $(grep -m1 'model name' /proc/cpuinfo | sed 's/.*:[[:space:]]*//')"
	else
		echo "# cpu_model: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
	fi
	echo "# cores: $(getconf _NPROCESSORS_ONLN 2>/dev/null || echo unknown)"
	if [ -r /proc/meminfo ]; then
		echo "# ram_gb: $(awk '/MemTotal/ {printf \"%.1f\", $2 / 1024 / 1024}' /proc/meminfo)"
	else
		echo "# ram_gb: $(sysctl -n hw.memsize 2>/dev/null | awk '{printf \"%.1f\", $1 / 1024 / 1024 / 1024}' || echo unknown)"
	fi
	echo "# docker: $(docker --version 2>/dev/null || echo not-available)"
	echo "# sql_server_image: $(docker inspect mssql-dev --format '{{.Image}}' 2>/dev/null || echo unknown)"
} >> "$OUTPUT_FILE"

echo ""
echo "[bench_codec_e2e] DONE. Output written to: $OUTPUT_FILE"
cat "$OUTPUT_FILE"
