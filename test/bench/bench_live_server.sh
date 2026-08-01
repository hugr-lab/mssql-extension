#!/usr/bin/env bash
# test/bench/bench_live_server.sh
#
# Live-server materialization benchmark: read (TDS -> DuckDB) and write
# (DuckDB -> BCP) against a real SQL Server, one step per type family.
#
# WHY THIS EXISTS
# ---------------
# The two benches we already have leave a gap in the middle:
#
#   make bench-materialize   pure client CPU, synthetic wire images, no server.
#                            Carries the optimization signal, but proves nothing
#                            about the shipped path.
#   bench_tpch_e2e.sh        wall clock of whole queries against the server.
#                            Realistic, but server I/O dominates: a 4x client
#                            win moves wall clock by a few percent and drowns.
#
# This script measures the shipped path against a live server AND separates the
# client's own cost from the server's, by recording **process CPU time** next to
# wall clock. Client CPU is exactly what specs 055-057 reduce; it does not move
# when the server is slow, and it is stable across runs in a way wall clock is
# not. A client-CPU win that wall clock hides today becomes a throughput win
# under concurrency (see the `conc` group).
#
# Three measurement devices make the numbers attributable:
#
#   1. per-family steps    Each step touches ONE column of ONE type, so
#                          ns/value lands on a single codec family and can be
#                          compared directly against `make bench-materialize`.
#   2. controls            `startup` (empty CLI invocation) is subtracted from
#                          every step. `ctl_*` steps run the SAME query shape
#                          against a LOCAL DuckDB table, giving the sink's own
#                          cost; step minus control ~= our TDS+codec cost.
#   3. explicit DDL        Server tables are pre-created with mssql_exec() at an
#                          exact T-SQL type, so NVARCHAR(16) and NVARCHAR(MAX)
#                          (PLP) are separate, deliberate cases rather than
#                          whatever CREATE_TABLE inferred.
#
# Threads default to 1: with the parallel scheduler on, user CPU is spread over
# workers and per-value attribution stops meaning anything.
#
# Manual target; NOT part of `make test` or any CI workflow.
#
# Env vars:
#   MSSQL_BENCH_DUCKDB_BIN  DuckDB CLI to measure (default: ./build/release/duckdb)
#   MSSQL_TEST_HOST         default: localhost
#   MSSQL_TEST_PORT         default: 1433
#   MSSQL_TEST_USER         default: sa
#   MSSQL_TEST_PASS         default: TestPassword1   (mssql-dev's old volume
#                           uses TPassw0rd! -- pass it explicitly there)
#   MSSQL_TEST_DB           default: TestDB
#   MSSQL_BENCH_ROWS        rows in the synthetic fixture (default: 500000)
#   MSSQL_BENCH_REPS        repetitions per step, median reported (default: 3)
#   MSSQL_BENCH_READ_ITERS  times a read step repeats its query in ONE process
#                           (default: 10) -- raises the CPU signal above clock
#                           resolution without growing the server-side table
#   MSSQL_BENCH_WRITE_ITERS same for write steps; each iteration APPENDS, so the
#                           table is reloaded to exactly ROWS rows (untimed)
#                           before the read step (default: 2)
#   MSSQL_BENCH_THREADS     DuckDB threads (default: 1, see above)
#   MSSQL_BENCH_GROUPS      subset of "write read wide conc" (default: all but conc)
#   MSSQL_BENCH_FAMILIES    subset of family keys (default: all)
#   MSSQL_BENCH_CONC_LIST   stream counts for the conc group (default: "1 2 4 8")
#   MSSQL_BENCH_PACKET_SIZES  TDS frame sizes to sweep on the wide steps; needs
#                           the mssql_tds_packet_size setting (probed; the
#                           dimension is skipped with a note when absent).
#                           default: empty (no sweep)
#   MSSQL_BENCH_LOAD_SQL    SQL prefix to load mssql (A/B against a released
#                           artifact: "LOAD '/abs/path/mssql.duckdb_extension';")
#   MSSQL_BENCH_UNSIGNED    1 to pass -unsigned (needed for the LOAD above)
#   MSSQL_BENCH_KEEP        1 to leave the server tables in place on exit
#   MSSQL_BENCH_OUTPUT      default: /tmp/bench_live_server_<epoch>.tsv
#
# Output columns:
#   step  wall_s  user_s  sys_s  cpu_s  rows  cols  cpu_ns_per_value  notes
#   (medians over MSSQL_BENCH_REPS; cpu_s = user+sys minus the startup control)

set -uo pipefail

DUCKDB_BIN="${MSSQL_BENCH_DUCKDB_BIN:-./build/release/duckdb}"
if [ ! -x "$DUCKDB_BIN" ]; then
	echo "ERROR: '$DUCKDB_BIN' is not executable. Run 'make release' or set MSSQL_BENCH_DUCKDB_BIN." >&2
	exit 2
fi

HOST="${MSSQL_TEST_HOST:-localhost}"
PORT="${MSSQL_TEST_PORT:-1433}"
SQLUSER="${MSSQL_TEST_USER:-sa}"
PASS="${MSSQL_TEST_PASS:-TestPassword1}"
DB="${MSSQL_TEST_DB:-TestDB}"
ROWS="${MSSQL_BENCH_ROWS:-500000}"
REPS="${MSSQL_BENCH_REPS:-3}"
READ_ITERS="${MSSQL_BENCH_READ_ITERS:-10}"
WRITE_ITERS="${MSSQL_BENCH_WRITE_ITERS:-2}"
THREADS="${MSSQL_BENCH_THREADS:-1}"
BENCH_GROUPS="${MSSQL_BENCH_GROUPS:-write read wide}"
FAMILY_FILTER="${MSSQL_BENCH_FAMILIES:-}"
CONC_LIST="${MSSQL_BENCH_CONC_LIST:-1 2 4 8}"
PACKET_SIZES="${MSSQL_BENCH_PACKET_SIZES:-}"
LOAD_SQL="${MSSQL_BENCH_LOAD_SQL:-}"
KEEP="${MSSQL_BENCH_KEEP:-0}"

UNSIGNED_FLAG=""
if [ "${MSSQL_BENCH_UNSIGNED:-0}" = "1" ]; then
	UNSIGNED_FLAG="-unsigned"
fi

OUTPUT_FILE="${MSSQL_BENCH_OUTPUT:-/tmp/bench_live_server_$(date +%s).tsv}"
DSN="Server=${HOST},${PORT};Database=${DB};User Id=${SQLUSER};Password=${PASS}"
SRC_DB="/tmp/bench_live_src_$$.duckdb"

case "$(uname -s 2>/dev/null)" in
	MINGW*|MSYS*|CYGWIN*) NULL_DEV="NUL" ;;
	*)                    NULL_DEV="/dev/null" ;;
esac

# `SET mssql_tds_packet_size` is emitted only when the build has it (probed
# below); PACKET_SQL stays empty otherwise so the same script runs on both.
PACKET_SQL=""

# ---------------------------------------------------------------------------
# Fixture definition: key | source column | T-SQL type | note
#
# One column per codec family, mirroring the groups in bench_materialize so the
# live ns/value can be read against the micro ns/value. str16max repeats the
# str16 payload at NVARCHAR(MAX) to isolate the PLP framing cost.
# ---------------------------------------------------------------------------
FAMILY_DEFS="
bigint|c_bigint|BIGINT|fixed 8B integer
int|c_int|INT|fixed 4B integer
double|c_double|FLOAT(53)|fixed 8B float
dec18|c_dec18|DECIMAL(18,4)|decimal, 8-byte mantissa
dec38|c_dec38|DECIMAL(38,10)|decimal, 16-byte mantissa (micro top target)
bool|c_bool|BIT|1-bit
date|c_date|DATE|3-byte date
ts|c_ts|DATETIME2(6)|8-byte datetime2
uuid|c_uuid|UNIQUEIDENTIFIER|16-byte GUID, byte-order swap
blob|c_blob|VARBINARY(16)|fixed binary
str4|c_str4|NVARCHAR(4)|len4 ASCII, low cardinality (dict candidate)
str16|c_str16|NVARCHAR(16)|len16 ASCII (micro reference case)
str16u|c_str16u|NVARCHAR(16)|non-ASCII, multi-byte UTF-8
str200|c_str200|NVARCHAR(200)|long strings
str16max|c_str16|NVARCHAR(MAX)|same payload, PLP framing
strnull|c_strnull|NVARCHAR(16)|50% NULL
vstr16|c_str16|VARCHAR(16) COLLATE Latin1_General_100_CI_AS_SC_UTF8|len16 ASCII into a UTF-8 varchar target
vstr16u|c_str16u|VARCHAR(32) COLLATE Latin1_General_100_CI_AS_SC_UTF8|non-ASCII into a UTF-8 varchar target
vstr200|c_str200|VARCHAR(200) COLLATE Latin1_General_100_CI_AS_SC_UTF8|long strings into a UTF-8 varchar target
vstrmax|c_str16|VARCHAR(MAX) COLLATE Latin1_General_100_CI_AS_SC_UTF8|same payload, PLP framing, UTF-8 target
"

# The four vstr* families exist for one comparison: every char column goes onto
# the BCP wire declared NVARCHAR and transcoded to UTF-16, so a UTF-8 varchar
# target is filled by transcoding twice — once here, once on the server. The
# nvarchar families beside them are the control: whatever a UTF-8 write path
# does, they must not move.

WIDE_COLS="c_bigint, c_int, c_double, c_dec18, c_dec38, c_bool, c_date, c_ts, c_uuid, c_blob, c_str4, c_str16, c_str16u, c_str200, c_strnull"
WIDE_DDL="c_bigint BIGINT, c_int INT, c_double FLOAT(53), c_dec18 DECIMAL(18,4), c_dec38 DECIMAL(38,10), c_bool BIT, c_date DATE, c_ts DATETIME2(6), c_uuid UNIQUEIDENTIFIER, c_blob VARBINARY(16), c_str4 NVARCHAR(4), c_str16 NVARCHAR(16), c_str16u NVARCHAR(16), c_str200 NVARCHAR(200), c_strnull NVARCHAR(16)"
WIDE_NCOLS=15

# ---------------------------------------------------------------------------
# Plumbing
# ---------------------------------------------------------------------------

run_sql() {
	"$DUCKDB_BIN" $UNSIGNED_FLAG -c "${LOAD_SQL} SET threads=${THREADS}; ${PACKET_SQL} $1"
}

# Untimed side effects (DDL, cleanup). Failures are the caller's problem.
run_quiet() {
	run_sql "$1" >/dev/null 2>&1
}

attach_sql() {
	printf "ATTACH '%s' AS db (TYPE mssql);" "$DSN"
}

# Repeat a statement N times inside ONE invocation.
#
# The client is fast enough that a single pass over a few hundred thousand
# values lands under the resolution of the CPU clock: 500k values at ~20 ns is
# 10 ms, which is also roughly the process-startup cost we subtract. Repeating
# the statement in-process multiplies the signal without multiplying the data
# on the server, and amortizes startup across the whole step.
repeat_sql() {
	local n="$1" sql="$2" i
	for i in $(seq 1 "$n"); do
		printf '%s\n' "$sql"
	done
}

# Median of a newline-separated list of numbers on stdin.
median() {
	sort -n | awk '
		{ v[NR] = $1 }
		END {
			if (NR == 0) { print "0.000"; exit }
			m = int((NR + 1) / 2)
			if (NR % 2) { printf "%.3f\n", v[m] } else { printf "%.3f\n", (v[m] + v[m+1]) / 2 }
		}'
}

# One timed invocation. Echoes "wall user sys", or "FAIL" on a non-zero exit.
# The `time` keyword writes to the *enclosing* shell's stderr, so the command's
# own streams are silenced inside the braces and only timings survive.
TIMEFORMAT='%3R %3U %3S'
run_timed() {
	local out
	out=$( { time { run_sql "$1" >/dev/null 2>/dev/null || echo "STEPFAIL"; } ; } 2>&1 )
	case "$out" in
		*STEPFAIL*) echo "FAIL" ; return 0 ;;
	esac
	# Strip any stray lines; keep the last one (the TIMEFORMAT output).
	echo "$out" | tail -n 1
}

STARTUP_CPU="0"

# time_step <step> <pre_sql|-> <sql> <rows> <cols> <notes>
# pre_sql runs untimed before EVERY repetition (DROP/CREATE of the target).
time_step() {
	local step="$1" pre_sql="$2" sql="$3" rows="$4" cols="$5" notes="$6"
	local walls="" users="" syss="" i out failed=0

	for i in $(seq 1 "$REPS"); do
		if [ "$pre_sql" != "-" ]; then
			run_quiet "$pre_sql"
		fi
		out=$(run_timed "$sql")
		if [ "$out" = "FAIL" ]; then
			failed=1
			break
		fi
		walls="${walls}$(echo "$out" | awk '{print $1}')
"
		users="${users}$(echo "$out" | awk '{print $2}')
"
		syss="${syss}$(echo "$out" | awk '{print $3}')
"
	done

	if [ "$failed" = "1" ]; then
		printf '%s\t-\t-\t-\t-\t%s\t%s\tFAILED: %s\n' "$step" "$rows" "$cols" "$notes" | tee -a "$OUTPUT_FILE"
		return 0
	fi

	local w u s cpu nsv
	w=$(printf '%s' "$walls" | grep -v '^$' | median)
	u=$(printf '%s' "$users" | grep -v '^$' | median)
	s=$(printf '%s' "$syss" | grep -v '^$' | median)
	cpu=$(awk -v u="$u" -v s="$s" -v b="$STARTUP_CPU" 'BEGIN {v = u + s - b; if (v < 0) v = 0; printf "%.3f", v}')
	if [ "$rows" = "-" ] || [ "$cols" = "-" ]; then
		nsv="-"
	else
		nsv=$(awk -v c="$cpu" -v r="$rows" -v k="$cols" 'BEGIN {n = r * k; if (n <= 0) {print "-"} else {printf "%.1f", c * 1e9 / n}}')
	fi
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$step" "$w" "$u" "$s" "$cpu" "$rows" "$cols" "$nsv" "$notes" | tee -a "$OUTPUT_FILE"
}

family_selected() {
	[ -z "$FAMILY_FILTER" ] && return 0
	case " $FAMILY_FILTER " in
		*" $1 "*) return 0 ;;
		*)        return 1 ;;
	esac
}

group_selected() {
	case " $BENCH_GROUPS " in
		*" $1 "*) return 0 ;;
		*)        return 1 ;;
	esac
}

cleanup_server() {
	local drops="" key rest
	echo "$FAMILY_DEFS" | grep -v '^$' | while IFS='|' read -r key rest; do
		echo "DROP TABLE IF EXISTS db.dbo.bench_live_${key};"
	done > /tmp/bench_live_drops_$$.sql
	drops=$(cat /tmp/bench_live_drops_$$.sql)
	rm -f /tmp/bench_live_drops_$$.sql
	run_quiet "$(attach_sql) ${drops} DROP TABLE IF EXISTS db.dbo.bench_live_wide; DETACH db;"
}

on_exit() {
	rm -f "$SRC_DB"
	if [ "$KEEP" != "1" ]; then
		cleanup_server
	fi
}
trap on_exit EXIT

# ---------------------------------------------------------------------------
# Pre-flight
# ---------------------------------------------------------------------------

echo "[bench_live] DuckDB CLI:  $DUCKDB_BIN"
echo "[bench_live] DSN:         Server=${HOST},${PORT};Database=${DB};User Id=${SQLUSER};Password=***"
echo "[bench_live] rows=${ROWS} reps=${REPS} read_iters=${READ_ITERS} write_iters=${WRITE_ITERS} threads=${THREADS}"
echo "[bench_live] groups:      ${BENCH_GROUPS}"
echo "[bench_live] output:      ${OUTPUT_FILE}"
echo ""

echo "[bench_live] Pre-flight: ATTACH + SELECT 1..."
preflight=$(run_sql "$(attach_sql) SELECT 1 AS ok;" 2>&1)
if ! echo "$preflight" | grep -q "ok"; then
	echo "ERROR: pre-flight failed. Check the DSN / that the server is up. Output:" >&2
	echo "$preflight" >&2
	exit 3
fi

# TDS frame size is a client-requested LOGIN7 field; the sweep needs a build
# that exposes it as a setting.
if [ -n "$PACKET_SIZES" ]; then
	if run_quiet "SET mssql_tds_packet_size=4096; SELECT 1;"; then
		echo "[bench_live] mssql_tds_packet_size supported; sweeping: ${PACKET_SIZES}"
	else
		echo "[bench_live] NOTE: this build has no mssql_tds_packet_size setting;"
		echo "[bench_live]       frame-size sweep skipped (TDS frames stay at the 4096 default)."
		PACKET_SIZES=""
	fi
fi
echo "[bench_live] Pre-flight OK."
echo ""

# ---------------------------------------------------------------------------
# Fixture
# ---------------------------------------------------------------------------

echo "[bench_live] Building local source fixture (${ROWS} rows)..."
rm -f "$SRC_DB"
FIXTURE_SQL=$(cat <<-EOF
	ATTACH '${SRC_DB}' AS src;
	CREATE OR REPLACE TABLE src.syn AS
	SELECT
		i                                                              AS c_bigint,
		(i % 1000000)::INTEGER                                         AS c_int,
		(i * 1.5)::DOUBLE                                              AS c_double,
		((i % 100000) / 100.0)::DECIMAL(18,4)                          AS c_dec18,
		CAST(CAST(i % 100000 AS DECIMAL(38,10)) / 3 AS DECIMAL(38,10)) AS c_dec38,
		(i % 2 = 0)                                                    AS c_bool,
		DATE '2000-01-01' + (i % 5000)::INTEGER                        AS c_date,
		TIMESTAMP '2000-01-01' + to_seconds((i % 100000)::BIGINT)      AS c_ts,
		(substr(md5(i::VARCHAR), 1, 8) || '-' || substr(md5(i::VARCHAR), 9, 4) || '-' ||
		 substr(md5(i::VARCHAR), 13, 4) || '-' || substr(md5(i::VARCHAR), 17, 4) || '-' ||
		 substr(md5(i::VARCHAR), 21, 12))::UUID                        AS c_uuid,
		encode(lpad((i % 1000)::VARCHAR, 16, '0'))                     AS c_blob,
		lpad((i % 1000)::VARCHAR, 4, '0')                              AS c_str4,
		lpad(i::VARCHAR, 16, 'x')                                      AS c_str16,
		'Ünïcödé' || lpad((i % 1000)::VARCHAR, 4, '0')                 AS c_str16u,
		repeat('abcdefghij', 19) || lpad((i % 1000)::VARCHAR, 10, '0') AS c_str200,
		CASE WHEN i % 2 = 0 THEN NULL ELSE lpad(i::VARCHAR, 16, 'x') END AS c_strnull
	FROM range(0, ${ROWS}) t(i);
EOF
)
if ! run_quiet "$FIXTURE_SQL"; then
	echo "ERROR: fixture creation failed. Re-run showing the error:" >&2
	run_sql "$FIXTURE_SQL" >&2
	exit 4
fi
echo "[bench_live] Fixture ready: ${SRC_DB}"
echo ""

{
	echo "# bench_live_server output"
	echo "# date: $(date -Iseconds 2>/dev/null || date)"
	echo "# duckdb_bin: $DUCKDB_BIN"
	echo "# host: ${HOST}:${PORT}/${DB}"
	echo "# rows: ${ROWS}  reps: ${REPS}  read_iters: ${READ_ITERS}  write_iters: ${WRITE_ITERS}  threads: ${THREADS}"
	echo "# groups: ${BENCH_GROUPS}"
	echo "# packet_sizes: ${PACKET_SIZES:-<default 4096, setting absent or unset>}"
	printf 'step\twall_s\tuser_s\tsys_s\tcpu_s\trows\tcols\tcpu_ns_per_value\tnotes\n'
} > "$OUTPUT_FILE"

# ---------------------------------------------------------------------------
# Control: the fixed cost of one CLI invocation (process start, extension init,
# ATTACH). Subtracted from every step's CPU below.
# ---------------------------------------------------------------------------

echo "[bench_live] === controls ==="
time_step "startup_bare" "-" "SELECT 1;" "-" "-" "empty invocation (NOT subtracted; reference)"
time_step "startup" "-" "$(attach_sql) SELECT 1;" "-" "-" "invocation + ATTACH (subtracted from every step)"
STARTUP_CPU=$(awk -F'\t' '$1 == "startup" {print $3 + $4}' "$OUTPUT_FILE" | tail -n 1)
STARTUP_CPU="${STARTUP_CPU:-0}"
echo "[bench_live] startup CPU baseline: ${STARTUP_CPU}s (subtracted from all steps below)"

# ---------------------------------------------------------------------------
# Per-family steps
# ---------------------------------------------------------------------------

echo ""
echo "[bench_live] === per-family: write (BCP encode + send) / read (decode) / control (local sink) ==="

echo "$FAMILY_DEFS" | grep -v '^$' | while IFS='|' read -r KEY SRC_COL TSQL_TYPE NOTE; do
	family_selected "$KEY" || continue
	TBL="bench_live_${KEY}"

	# Server-side DDL at an EXACT type, so the codec path under test is the one
	# we named -- not whatever CREATE_TABLE would have inferred. Untimed.
	PRE_DDL="$(attach_sql)
		SELECT mssql_exec('db', 'DROP TABLE IF EXISTS dbo.${TBL}');
		SELECT mssql_exec('db', 'CREATE TABLE dbo.${TBL} (c ${TSQL_TYPE} NULL)');"

	LOAD_ONCE="ATTACH '${SRC_DB}' AS src; $(attach_sql)
		COPY (SELECT ${SRC_COL} AS c FROM src.syn) TO 'mssql://db/dbo/${TBL}' (FORMAT 'bcp');"

	if group_selected write; then
		time_step "write_${KEY}" "$PRE_DDL" "
			ATTACH '${SRC_DB}' AS src;
			$(attach_sql)
			$(repeat_sql "$WRITE_ITERS" "COPY (SELECT ${SRC_COL} AS c FROM src.syn)
				TO 'mssql://db/dbo/${TBL}' (FORMAT 'bcp');")
		" "$((ROWS * WRITE_ITERS))" "1" "BCP write, ${TSQL_TYPE} -- ${NOTE}"
	fi

	if group_selected read; then
		# Reset to exactly ROWS rows: the write step above appended WRITE_ITERS
		# times, and read_* row counts must match what the step reports. Untimed.
		run_quiet "$PRE_DDL"
		run_quiet "$LOAD_ONCE"

		# min() forces the column into the projection and through the codec,
		# at one comparison per value -- the cheapest sink that cannot be
		# optimized away into a server-side count.
		time_step "read_${KEY}" "-" "
			$(attach_sql)
			$(repeat_sql "$READ_ITERS" "SELECT min(c) FROM db.dbo.${TBL};")
		" "$((ROWS * READ_ITERS))" "1" "TDS read + decode, ${TSQL_TYPE} -- ${NOTE}"

		# Same sink, same value count, local storage: the part of read_* that
		# is NOT ours. read_<k> minus ctl_<k> ~= TDS + codec cost.
		time_step "ctl_${KEY}" "-" "
			ATTACH '${SRC_DB}' AS src;
			$(repeat_sql "$READ_ITERS" "SELECT min(${SRC_COL}) FROM src.syn;")
		" "$((ROWS * READ_ITERS))" "1" "control: local scan + same sink"
	fi
done

# ---------------------------------------------------------------------------
# Wide table: realistic mixed-type row, plus the TDS frame-size sweep
# ---------------------------------------------------------------------------

WIDE_MIN_SELECT="SELECT min(c_bigint), min(c_int), min(c_double), min(c_dec18), min(c_dec38),
			   min(c_bool), min(c_date), min(c_ts), min(c_uuid), min(c_blob),
			   min(c_str4), min(c_str16), min(c_str16u), min(c_str200), min(c_strnull)"

# A wide pass already covers WIDE_NCOLS values per row, so it needs fewer
# repetitions than a single-column step to reach the same value budget.
WIDE_ITERS=$((READ_ITERS / WIDE_NCOLS))
[ "$WIDE_ITERS" -lt 1 ] && WIDE_ITERS=1

run_wide_steps() {
	local tag="$1" note_suffix="$2"

	local pre_ddl="$(attach_sql)
		SELECT mssql_exec('db', 'DROP TABLE IF EXISTS dbo.bench_live_wide');
		SELECT mssql_exec('db', 'CREATE TABLE dbo.bench_live_wide (${WIDE_DDL})');"

	time_step "write_wide${tag}" "$pre_ddl" "
		ATTACH '${SRC_DB}' AS src;
		$(attach_sql)
		COPY (SELECT ${WIDE_COLS} FROM src.syn)
			TO 'mssql://db/dbo/bench_live_wide' (FORMAT 'bcp');
	" "$ROWS" "$WIDE_NCOLS" "BCP write, 15 mixed columns${note_suffix}"

	time_step "read_wide_min${tag}" "-" "
		$(attach_sql)
		$(repeat_sql "$WIDE_ITERS" "${WIDE_MIN_SELECT} FROM db.dbo.bench_live_wide;")
	" "$((ROWS * WIDE_ITERS))" "$WIDE_NCOLS" "TDS read + decode, 15 mixed columns${note_suffix}"

	time_step "read_wide_drain${tag}" "-" "
		$(attach_sql)
		COPY (SELECT * FROM db.dbo.bench_live_wide) TO '${NULL_DEV}' (FORMAT csv);
	" "$ROWS" "$WIDE_NCOLS" "read + CSV sink (realistic drain)${note_suffix}"

	time_step "ctl_wide_min${tag}" "-" "
		ATTACH '${SRC_DB}' AS src;
		$(repeat_sql "$WIDE_ITERS" "${WIDE_MIN_SELECT} FROM src.syn;")
	" "$((ROWS * WIDE_ITERS))" "$WIDE_NCOLS" "control: local scan + same sink${note_suffix}"
}

if group_selected wide; then
	echo ""
	echo "[bench_live] === wide (15 mixed columns) ==="
	if [ -z "$PACKET_SIZES" ]; then
		run_wide_steps "" ""
	else
		for PS in $PACKET_SIZES; do
			echo "[bench_live] --- TDS frame size ${PS} ---"
			PACKET_SQL="SET mssql_tds_packet_size=${PS};"
			run_wide_steps "_ps${PS}" " (TDS frame ${PS}B)"
		done
		PACKET_SQL=""
	fi
fi

# ---------------------------------------------------------------------------
# Concurrency: turns a client-CPU win into a visible throughput win. If the
# client is the bottleneck, aggregate rows/s stops scaling with stream count.
# ---------------------------------------------------------------------------

if group_selected conc; then
	echo ""
	echo "[bench_live] === concurrency (aggregate throughput vs stream count) ==="
	for N in $CONC_LIST; do
		# N independent CLI processes drain the wide table simultaneously; the
		# step's wall clock is the slowest of them, rows = N * ROWS.
		SQL="$(attach_sql) SELECT min(c_str16), min(c_bigint), min(c_dec38) FROM db.dbo.bench_live_wide;"
		STEP="conc_read_x${N}"
		WALLS=""
		for i in $(seq 1 "$REPS"); do
			T0=$(date +%s.%N)
			for j in $(seq 1 "$N"); do
				run_sql "$SQL" >/dev/null 2>&1 &
			done
			wait
			T1=$(date +%s.%N)
			WALLS="${WALLS}$(awk "BEGIN {printf \"%.3f\", $T1 - $T0}")
"
		done
		W=$(printf '%s' "$WALLS" | grep -v '^$' | median)
		THR=$(awk -v w="$W" -v r="$ROWS" -v n="$N" 'BEGIN {if (w <= 0) {print "-"} else {printf "%.0f", r * n * 3 / w}}')
		printf '%s\t%s\t-\t-\t-\t%s\t3\t-\t%s aggregate values/s across %s streams\n' \
			"$STEP" "$W" "$((ROWS * N))" "$THR" "$N" | tee -a "$OUTPUT_FILE"
	done
fi

# ---------------------------------------------------------------------------

{
	echo ""
	echo "# end of bench_live_server"
	echo "# uname: $(uname -srm 2>/dev/null || uname -a)"
	if [ -r /proc/cpuinfo ]; then
		echo "# cpu_model: $(grep -m1 'model name' /proc/cpuinfo | sed 's/.*:[[:space:]]*//')"
	else
		echo "# cpu_model: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
	fi
	echo "# cores: $(getconf _NPROCESSORS_ONLN 2>/dev/null || echo unknown)"
	echo "# sql_server_image: $(docker inspect mssql-dev --format '{{.Image}}' 2>/dev/null || echo unknown)"
} >> "$OUTPUT_FILE"

echo ""
echo "[bench_live] DONE. Output written to: ${OUTPUT_FILE}"
echo ""
column -t -s "$(printf '\t')" "$OUTPUT_FILE" 2>/dev/null || cat "$OUTPUT_FILE"
