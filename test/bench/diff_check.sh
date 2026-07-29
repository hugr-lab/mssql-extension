#!/usr/bin/env bash
# test/bench/diff_check.sh
#
# Spec 054 D5: differential harness. Runs an identical query list through
# TWO extension builds against the SAME SQL Server and compares the full
# ordered result sets byte-for-byte (COPY ... TO csv + cmp). Used from
# phase 0 onward as a merge gate for every conversion-touching PR in the
# materialization series.
#
# Coverage (fixtures from docker/init/init.sql, loaded by `make docker-up`):
#   - every type family            AllDataTypes (catalog scan + mssql_scan)
#   - NULL-heavy                   NullableTypes, NullableDatetimeScales
#   - PLP/MAX values               MaxTypes (incl. 8000-char values)
#   - empty-string vs NULL         MaxTypes rows 2/3 + explicit probe
#   - embedded NUL                 NCHAR(0) expression via mssql_scan
#   - non-UTF8 collations          CollationTest
#   - 0-row / 1-row results        AllDataTypes filters
#   - >2048-row results            LargeTable (10k-row slice + full checksum)
#   - BCP encode (write path)      write-back: COPY TO mssql + ordered readback
#
# Env vars:
#   MSSQL_DIFF_DUCKDB_A   duckdb CLI for side A (default ./build/release/duckdb)
#   MSSQL_DIFF_DUCKDB_B   duckdb CLI for side B (default: same as A)
#   MSSQL_DIFF_LOAD_SQL_A SQL prefix loading mssql on side A; empty relies on
#                         a built-in mssql. For a stock CLI use e.g.
#                           "INSTALL mssql FROM community; LOAD mssql;"
#                         or an unsigned local build:
#                           "LOAD '/abs/path/mssql.duckdb_extension';"
#   MSSQL_DIFF_LOAD_SQL_B same for side B
#   MSSQL_DIFF_UNSIGNED_A set to 1 to pass -unsigned to side A
#   MSSQL_DIFF_UNSIGNED_B set to 1 to pass -unsigned to side B
#   MSSQL_TEST_HOST/PORT/USER/PASS/DB   same defaults as bench_tpch_e2e.sh
#   MSSQL_DIFF_OUTPUT_DIR default: /tmp/mssql_diff_check_<pid>
#
# Exit codes: 0 all identical; 1 at least one query differs; 2 setup error;
# 4 a side failed to execute its statement list.
#
# Manual target; NOT run as part of `make test` or any CI workflow.

set -uo pipefail

DUCKDB_A="${MSSQL_DIFF_DUCKDB_A:-./build/release/duckdb}"
DUCKDB_B="${MSSQL_DIFF_DUCKDB_B:-$DUCKDB_A}"
LOAD_SQL_A="${MSSQL_DIFF_LOAD_SQL_A:-}"
LOAD_SQL_B="${MSSQL_DIFF_LOAD_SQL_B:-}"

for bin in "$DUCKDB_A" "$DUCKDB_B"; do
	if [ ! -x "$bin" ]; then
		echo "ERROR: duckdb CLI '$bin' is not executable." >&2
		exit 2
	fi
done

HOST="${MSSQL_TEST_HOST:-localhost}"
PORT="${MSSQL_TEST_PORT:-1433}"
USER="${MSSQL_TEST_USER:-sa}"
PASS="${MSSQL_TEST_PASS:-TestPassword1}"
DB="${MSSQL_TEST_DB:-TestDB}"
DSN="Server=${HOST},${PORT};Database=${DB};User Id=${USER};Password=${PASS}"

OUT_DIR="${MSSQL_DIFF_OUTPUT_DIR:-/tmp/mssql_diff_check_$$}"
mkdir -p "$OUT_DIR"

# Plain strings, not arrays (bash 3.2 + set -u). Single-token flags.
UNSIGNED_A=""
UNSIGNED_B=""
[ "${MSSQL_DIFF_UNSIGNED_A:-0}" = "1" ] && UNSIGNED_A="-unsigned"
[ "${MSSQL_DIFF_UNSIGNED_B:-0}" = "1" ] && UNSIGNED_B="-unsigned"

# The cmp loop below iterates this list; emit_side_script must write one
# ${OUT_DIR}/<name>.<side>.csv per entry.
QUERY_NAMES="alldatatypes_catalog alldatatypes_scan nullable_types datetime_scales maxtypes_plp empty_vs_null embedded_nul collation zero_rows one_row large_over_2048 large_checksum writeback_bcp"

# Emit the full statement list for one side into a .sql file. $1 = side (a|b).
# Every result lands in a csv named <query>.<side>.csv; the write-back case
# additionally creates dbo.diff_check_wb_<side> on the server (dropped at the
# end of this script).
emit_side_script() {
	local side="$1"
	local load_sql="$2"
	cat <<-EOF
		${load_sql}
		ATTACH '${DSN}' AS db (TYPE mssql);

		COPY (SELECT * FROM db.dbo.AllDataTypes ORDER BY id)
			TO '${OUT_DIR}/alldatatypes_catalog.${side}.csv' (FORMAT csv, HEADER);

		COPY (SELECT * FROM mssql_scan('db', 'SELECT * FROM dbo.AllDataTypes ORDER BY id'))
			TO '${OUT_DIR}/alldatatypes_scan.${side}.csv' (FORMAT csv, HEADER);

		COPY (SELECT * FROM db.dbo.NullableTypes ORDER BY id)
			TO '${OUT_DIR}/nullable_types.${side}.csv' (FORMAT csv, HEADER);

		COPY (SELECT * FROM db.dbo.NullableDatetimeScales ORDER BY id)
			TO '${OUT_DIR}/datetime_scales.${side}.csv' (FORMAT csv, HEADER);

		COPY (SELECT * FROM db.dbo.MaxTypes ORDER BY id)
			TO '${OUT_DIR}/maxtypes_plp.${side}.csv' (FORMAT csv, HEADER);

		COPY (SELECT id,
					 col_nvarchar_max IS NULL AS nv_is_null,
					 coalesce(length(col_nvarchar_max), -1) AS nv_len,
					 col_varchar_max IS NULL AS v_is_null,
					 coalesce(length(col_varchar_max), -1) AS v_len,
					 col_varbinary_max IS NULL AS vb_is_null,
					 coalesce(octet_length(col_varbinary_max), -1) AS vb_len
			  FROM db.dbo.MaxTypes ORDER BY id)
			TO '${OUT_DIR}/empty_vs_null.${side}.csv' (FORMAT csv, HEADER);

		COPY (SELECT * FROM mssql_scan('db',
				'SELECT id, CONCAT(N''pre'', NCHAR(0), N''post'') AS s, NCHAR(0) AS only_nul FROM dbo.AllDataTypes ORDER BY id'))
			TO '${OUT_DIR}/embedded_nul.${side}.csv' (FORMAT csv, HEADER);

		COPY (SELECT * FROM db.dbo.CollationTest ORDER BY id)
			TO '${OUT_DIR}/collation.${side}.csv' (FORMAT csv, HEADER);

		COPY (SELECT * FROM db.dbo.AllDataTypes WHERE id < 0 ORDER BY id)
			TO '${OUT_DIR}/zero_rows.${side}.csv' (FORMAT csv, HEADER);

		COPY (SELECT * FROM db.dbo.AllDataTypes WHERE id = 1 ORDER BY id)
			TO '${OUT_DIR}/one_row.${side}.csv' (FORMAT csv, HEADER);

		COPY (SELECT * FROM db.dbo.LargeTable WHERE id <= 10000 ORDER BY id)
			TO '${OUT_DIR}/large_over_2048.${side}.csv' (FORMAT csv, HEADER);

		COPY (SELECT count(*) AS cnt,
					 sum(id) AS sum_id,
					 sum(category) AS sum_cat,
					 sum(value) AS sum_value,
					 min(created_date) AS min_date,
					 max(created_date) AS max_date,
					 sum(length(name)) AS name_len,
					 sum(length(coalesce(description, ''))) AS desc_len,
					 sum(CASE WHEN is_active THEN 1 ELSE 0 END) AS active_cnt
			  FROM db.dbo.LargeTable)
			TO '${OUT_DIR}/large_checksum.${side}.csv' (FORMAT csv, HEADER);

		COPY (SELECT range::BIGINT AS id,
					 'тест_' || range AS s_unique,
					 CASE WHEN range % 7 = 0 THEN NULL
						  WHEN range % 5 = 0 THEN ''
						  ELSE 'val_' || (range % 10) END AS s_nullable,
					 (range * 1.5)::DECIMAL(18,6) AS d18,
					 (range % 2 = 0) AS flag,
					 (range * 0.001)::DOUBLE AS dbl,
					 TIMESTAMP '2024-01-01 00:00:00' + INTERVAL (range % 86400) SECOND AS ts,
					 DATE '2024-01-01' + INTERVAL (range % 365) DAY AS dt
			  FROM range(5000))
			TO 'mssql://db/dbo/diff_check_wb_${side}' (FORMAT 'bcp', CREATE_TABLE true, OVERWRITE true);

		COPY (SELECT * FROM mssql_scan('db',
				'SELECT id, s_unique, s_nullable, d18, flag, dbl, ts, dt FROM dbo.diff_check_wb_${side} ORDER BY id'))
			TO '${OUT_DIR}/writeback_bcp.${side}.csv' (FORMAT csv, HEADER);
	EOF
}

echo "[diff_check] side A: $DUCKDB_A ${UNSIGNED_A} load='${LOAD_SQL_A}'"
echo "[diff_check] side B: $DUCKDB_B ${UNSIGNED_B} load='${LOAD_SQL_B}'"
echo "[diff_check] DSN: Server=${HOST},${PORT};Database=${DB};User Id=${USER};Password=***"
echo "[diff_check] output dir: $OUT_DIR"
echo ""

# Pre-flight: fixtures must exist (docker/init/init.sql via `make docker-up`).
preflight_out=$("$DUCKDB_A" $UNSIGNED_A -c "${LOAD_SQL_A} ATTACH '${DSN}' AS db (TYPE mssql); SELECT count(*) AS ok FROM db.dbo.AllDataTypes;" 2>&1)
if ! echo "$preflight_out" | grep -q "ok"; then
	echo "ERROR: pre-flight failed — dbo.AllDataTypes not reachable. Run 'make docker-up'" >&2
	echo "       (fixtures come from docker/init/init.sql) and check the DSN. Last output:" >&2
	echo "$preflight_out" >&2
	exit 2
fi
echo "[diff_check] Pre-flight OK."

run_side() {
	local side="$1"
	local bin="$2"
	local unsigned_flag="$3"
	local load_sql="$4"
	local script="${OUT_DIR}/side_${side}.sql"
	emit_side_script "$side" "$load_sql" > "$script"
	echo "[diff_check] running side ${side}..."
	if ! "$bin" $unsigned_flag < "$script" > "${OUT_DIR}/side_${side}.log" 2>&1; then
		echo "ERROR: side ${side} failed. Log:" >&2
		cat "${OUT_DIR}/side_${side}.log" >&2
		exit 4
	fi
}

run_side a "$DUCKDB_A" "$UNSIGNED_A" "$LOAD_SQL_A"
run_side b "$DUCKDB_B" "$UNSIGNED_B" "$LOAD_SQL_B"

echo ""
failures=0
for name in $QUERY_NAMES; do
	fa="${OUT_DIR}/${name}.a.csv"
	fb="${OUT_DIR}/${name}.b.csv"
	if [ ! -f "$fa" ] || [ ! -f "$fb" ]; then
		printf '%-24s MISSING (a:%s b:%s)\n' "$name" "$([ -f "$fa" ] && echo ok || echo absent)" "$([ -f "$fb" ] && echo ok || echo absent)"
		failures=$((failures + 1))
		continue
	fi
	if cmp -s "$fa" "$fb"; then
		printf '%-24s PASS (%s bytes)\n' "$name" "$(wc -c < "$fa" | tr -d ' ')"
	else
		printf '%-24s DIFF — see %s vs %s\n' "$name" "$fa" "$fb"
		failures=$((failures + 1))
	fi
done

# Cleanup the server-side write-back tables (idempotent).
"$DUCKDB_A" $UNSIGNED_A -c "${LOAD_SQL_A} ATTACH '${DSN}' AS db (TYPE mssql); DROP TABLE IF EXISTS db.dbo.diff_check_wb_a; DROP TABLE IF EXISTS db.dbo.diff_check_wb_b;" >/dev/null 2>&1 || true

echo ""
if [ "$failures" -gt 0 ]; then
	echo "[diff_check] FAILED: ${failures} quer(y/ies) differ or are missing. Artifacts kept in $OUT_DIR"
	exit 1
fi
echo "[diff_check] OK: all queries byte-identical across the two builds."
exit 0
