#!/usr/bin/env bash
# test/bench/bench_tpch_e2e.sh
#
# Spec 054 D3: end-to-end TPC-H benchmark for the materialization series.
# Times BCP write (DuckDB→SQL Server) and scan read (SQL Server→DuckDB) steps
# over dbgen data at several scale factors. Same TSV protocol + host-metadata
# footer as bench_codec_e2e.sh (spec 044).
#
# e2e numbers are NO-REGRESSION EVIDENCE, not the optimization signal —
# SQL-Server-in-Docker I/O dominates at scale; microbenchmarks
# (make bench-materialize) carry the signal. See spec 054 §2.
#
# Requires a DuckDB CLI with BOTH mssql and tpch built in: `make bench-build`
# (tpch is bench-only and never ships in extension_config.cmake).
#
# Manual target; NOT run as part of `make test` or any CI workflow.
#
# Env vars:
#   MSSQL_BENCH_DUCKDB_BIN  path to the bench-build DuckDB CLI (REQUIRED)
#   MSSQL_TEST_HOST         default: localhost
#   MSSQL_TEST_PORT         default: 1433
#   MSSQL_TEST_USER         default: sa
#   MSSQL_TEST_PASS         default: TestPassword1
#   MSSQL_TEST_DB           default: TestDB
#   MSSQL_BENCH_SF_LIST     default: "0.01 0.1 1"  (spec: SF 10 optional,
#                           append it when the Docker volume allows)
#   MSSQL_BENCH_OUTPUT      default: /tmp/bench_tpch_e2e_<epoch>.txt
#
# Output:
#   step_name<TAB>seconds<TAB>row_count<TAB>notes
#   (steps are suffixed _sf<sf>; dbgen is timed but excluded from comparison)

set -uo pipefail

DUCKDB_BIN="${MSSQL_BENCH_DUCKDB_BIN:-}"
if [ -z "$DUCKDB_BIN" ]; then
	echo "ERROR: MSSQL_BENCH_DUCKDB_BIN must be set (a 'make bench-build' CLI with mssql+tpch)." >&2
	echo "Example: MSSQL_BENCH_DUCKDB_BIN=./build/release/duckdb bash test/bench/bench_tpch_e2e.sh" >&2
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
SF_LIST="${MSSQL_BENCH_SF_LIST:-0.01 0.1 1}"

OUTPUT_DEFAULT="/tmp/bench_tpch_e2e_$(date +%s).txt"
OUTPUT_FILE="${MSSQL_BENCH_OUTPUT:-$OUTPUT_DEFAULT}"

DSN="Server=${HOST},${PORT};Database=${DB};User Id=${USER};Password=${PASS}"

case "$(uname -s 2>/dev/null)" in
	MINGW*|MSYS*|CYGWIN*) NULL_DEV="NUL" ;;
	*)                    NULL_DEV="/dev/null" ;;
esac

echo "[bench_tpch_e2e] DuckDB CLI: $DUCKDB_BIN"
echo "[bench_tpch_e2e] DSN: Server=${HOST},${PORT};Database=${DB};User Id=${USER};Password=***"
echo "[bench_tpch_e2e] Scale factors: ${SF_LIST}"
echo "[bench_tpch_e2e] Output file: ${OUTPUT_FILE}"
echo ""

# Pre-flight: the CLI must have tpch (dbgen) AND mssql (ATTACH) built in.
echo "[bench_tpch_e2e] Pre-flight: dbgen(sf=0) + ATTACH + SELECT 1..."
preflight_out=$("$DUCKDB_BIN" -c "CALL dbgen(sf=0); ATTACH '${DSN}' AS db (TYPE mssql); SELECT 1 AS ok;" 2>&1)
if ! echo "$preflight_out" | grep -q "ok"; then
	echo "ERROR: pre-flight failed. Build with 'make bench-build' and check the DSN. Last output:" >&2
	echo "$preflight_out" >&2
	exit 3
fi
echo "[bench_tpch_e2e] Pre-flight OK."
echo ""

# All server-side tables carry the bench_tpch_ prefix; cleanup is idempotent.
cleanup_sql() {
	cat <<-EOF
		ATTACH '${DSN}' AS db (TYPE mssql);
		DROP TABLE IF EXISTS db.dbo.bench_tpch_lineitem;
		DROP TABLE IF EXISTS db.dbo.bench_tpch_orders;
		DROP TABLE IF EXISTS db.dbo.bench_tpch_part;
		DROP TABLE IF EXISTS db.dbo.bench_tpch_customer;
		DROP TABLE IF EXISTS db.dbo.bench_tpch_sum_agg;
		DETACH db;
	EOF
}

# Time a single CLI invocation. Args: <step> <sql> <rows> <notes>.
time_step() {
	local step="$1"
	local sql="$2"
	local rows="$3"
	local notes="$4"
	local t0
	local t1
	local seconds
	t0=$(date +%s.%N)
	if ! "$DUCKDB_BIN" -c "$sql" >/dev/null 2>&1; then
		echo "ERROR: step '$step' failed. Aborting." >&2
		"$DUCKDB_BIN" -c "$(cleanup_sql)" >/dev/null 2>&1 || true
		exit 4
	fi
	t1=$(date +%s.%N)
	seconds=$(awk "BEGIN {printf \"%.3f\", $t1 - $t0}")
	printf '%s\t%s\t%s\t%s\n' "$step" "$seconds" "$rows" "$notes" | tee -a "$OUTPUT_FILE"
}

{
	echo "# bench_tpch_e2e output"
	echo "# date: $(date -Iseconds 2>/dev/null || date)"
	echo "# duckdb_bin: $DUCKDB_BIN"
	echo "# host: ${HOST}:${PORT}/${DB}"
	echo "# sf_list: ${SF_LIST}"
	echo "step	seconds	row_count	notes"
} > "$OUTPUT_FILE"

for SF in $SF_LIST; do
	SFX="sf$(echo "$SF" | tr '.' '_')"
	SRC_DB="/tmp/bench_tpch_src_${SFX}_$$.duckdb"
	trap 'rm -f /tmp/bench_tpch_src_*_$$.duckdb' EXIT

	echo ""
	echo "[bench_tpch_e2e] === SF ${SF} ==="

	echo "[bench_tpch_e2e] Cleanup: dropping bench_tpch_* server tables..."
	"$DUCKDB_BIN" -c "$(cleanup_sql)" >/dev/null 2>&1 || true
	rm -f "$SRC_DB"

	# dbgen — data generation into a persistent DuckDB file so each step is a
	# fresh CLI process sharing the same source (bench_codec_e2e pattern).
	# Excluded from before/after comparison.
	time_step "dbgen_${SFX}" "
		ATTACH '${SRC_DB}' AS src;
		USE src;
		CALL dbgen(sf=${SF});
	" "-" "data generation (excluded from comparison)"

	LINEITEM_ROWS=$("$DUCKDB_BIN" -noheader -list -c "ATTACH '${SRC_DB}' AS src; SELECT count(*) FROM src.lineitem;" 2>/dev/null | tail -1)
	ORDERS_ROWS=$("$DUCKDB_BIN" -noheader -list -c "ATTACH '${SRC_DB}' AS src; SELECT count(*) FROM src.orders;" 2>/dev/null | tail -1)
	PART_ROWS=$("$DUCKDB_BIN" -noheader -list -c "ATTACH '${SRC_DB}' AS src; SELECT count(*) FROM src.part;" 2>/dev/null | tail -1)
	CUSTOMER_ROWS=$("$DUCKDB_BIN" -noheader -list -c "ATTACH '${SRC_DB}' AS src; SELECT count(*) FROM src.customer;" 2>/dev/null | tail -1)
	echo "[bench_tpch_e2e] rows: lineitem=${LINEITEM_ROWS} orders=${ORDERS_ROWS} part=${PART_ROWS} customer=${CUSTOMER_ROWS}"

	# --- Write path (DuckDB → BCP) ---

	time_step "copy_lineitem_${SFX}" "
		ATTACH '${SRC_DB}' AS src;
		ATTACH '${DSN}' AS db (TYPE mssql);
		COPY (SELECT * FROM src.lineitem)
			TO 'mssql://db/dbo/bench_tpch_lineitem' (FORMAT 'bcp', CREATE_TABLE true);
	" "${LINEITEM_ROWS}" "numeric/date-heavy + low-cardinality strings"

	time_step "copy_orders_${SFX}" "
		ATTACH '${SRC_DB}' AS src;
		ATTACH '${DSN}' AS db (TYPE mssql);
		COPY (SELECT * FROM src.orders)
			TO 'mssql://db/dbo/bench_tpch_orders' (FORMAT 'bcp', CREATE_TABLE true);
	" "${ORDERS_ROWS}" "mixed numeric/string"

	time_step "copy_part_${SFX}" "
		ATTACH '${SRC_DB}' AS src;
		ATTACH '${DSN}' AS db (TYPE mssql);
		COPY (SELECT * FROM src.part)
			TO 'mssql://db/dbo/bench_tpch_part' (FORMAT 'bcp', CREATE_TABLE true);
	" "${PART_ROWS}" "string-heavy"

	time_step "copy_customer_${SFX}" "
		ATTACH '${SRC_DB}' AS src;
		ATTACH '${DSN}' AS db (TYPE mssql);
		COPY (SELECT * FROM src.customer)
			TO 'mssql://db/dbo/bench_tpch_customer' (FORMAT 'bcp', CREATE_TABLE true);
	" "${CUSTOMER_ROWS}" "string-heavy"

	# SUM(BIGINT) → HUGEINT → DECIMAL(38,0) on the wire: the #177 regression
	# case (works natively since spec 054 T4; no ::DECIMAL cast workaround).
	time_step "copy_sum_agg_${SFX}" "
		ATTACH '${SRC_DB}' AS src;
		ATTACH '${DSN}' AS db (TYPE mssql);
		COPY (SELECT l_returnflag, l_linestatus, SUM(l_orderkey) AS key_sum, COUNT(*) AS cnt
			  FROM src.lineitem GROUP BY l_returnflag, l_linestatus)
			TO 'mssql://db/dbo/bench_tpch_sum_agg' (FORMAT 'bcp', CREATE_TABLE true);
	" "-" "aggregate-then-COPY (HUGEINT -> DECIMAL(38,0), #177)"

	# --- Read path (TDS → DuckDB) ---

	time_step "scan_full_lineitem_${SFX}" "
		ATTACH '${DSN}' AS db (TYPE mssql);
		COPY (SELECT * FROM db.dbo.bench_tpch_lineitem)
			TO '${NULL_DEV}' (FORMAT csv);
	" "${LINEITEM_ROWS}" "drain scan, all columns"

	time_step "scan_strings_${SFX}" "
		ATTACH '${DSN}' AS db (TYPE mssql);
		COPY (SELECT p_name, p_type, p_comment FROM db.dbo.bench_tpch_part)
			TO '${NULL_DEV}' (FORMAT csv);
		COPY (SELECT c_name, c_address, c_comment FROM db.dbo.bench_tpch_customer)
			TO '${NULL_DEV}' (FORMAT csv);
	" "-" "part+customer string columns"

	time_step "scan_lowcard_${SFX}" "
		ATTACH '${DSN}' AS db (TYPE mssql);
		COPY (SELECT l_shipmode, l_returnflag FROM db.dbo.bench_tpch_lineitem)
			TO '${NULL_DEV}' (FORMAT csv);
	" "${LINEITEM_ROWS}" "low-cardinality projection (dictionary case, phases 2+)"

	time_step "scan_limit_${SFX}" "
		ATTACH '${DSN}' AS db (TYPE mssql);
		SELECT * FROM db.dbo.bench_tpch_lineitem LIMIT 10;
	" "10" "cold bind + early abandon (per-chunk overhead)"

	time_step "q1_local_${SFX}" "
		ATTACH '${DSN}' AS db (TYPE mssql);
		SELECT l_returnflag, l_linestatus,
			   SUM(l_quantity) AS sum_qty,
			   SUM(l_extendedprice) AS sum_base_price,
			   SUM(l_extendedprice * (1 - l_discount)) AS sum_disc_price,
			   SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge,
			   AVG(l_quantity) AS avg_qty,
			   AVG(l_extendedprice) AS avg_price,
			   AVG(l_discount) AS avg_disc,
			   COUNT(*) AS count_order
		FROM db.dbo.bench_tpch_lineitem
		WHERE l_shipdate <= DATE '1998-09-02'
		GROUP BY l_returnflag, l_linestatus
		ORDER BY l_returnflag, l_linestatus;
	" "${LINEITEM_ROWS}" "TPC-H Q1 over attached table (scan + aggregate)"

	rm -f "$SRC_DB"
done

echo ""
echo "[bench_tpch_e2e] Cleanup: dropping bench_tpch_* server tables..."
"$DUCKDB_BIN" -c "$(cleanup_sql)" >/dev/null 2>&1 || true

{
	echo ""
	echo "# end of bench_tpch_e2e"
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
echo "[bench_tpch_e2e] DONE. Output written to: $OUTPUT_FILE"
cat "$OUTPUT_FILE"
