#!/usr/bin/env bash
# test/bench/bench_058_t0.sh
#
# Spec 058 T0 — the measurement gate. Two jobs:
#
#   T0b  phase split (parse/read/process ns/row) from MSSQL_COUNTERS on three
#        fixture shapes: 1-column bigint, 4-column mixed, 15-column wide.
#   T0c/d marginal cost of ONE candidate pass/copy, by the established
#        2x trick: a variant binary does the work twice, the delta of the
#        interleaved A/B is exactly the marginal cost of one.
#
# Usage:
#   bench_058_t0.sh phases <binary>                  # T0b only
#   bench_058_t0.sh ab <base_binary> <variant_binary> <label>
#
# Interleaved pairs, order rotated, medians — wall clock is noise on this host
# (up to 80%); process CPU (user+sys) is the signal, per the project protocol.
set -euo pipefail
cd "$(dirname "$0")/../.."

set -a; . ./.env 2>/dev/null || true; set +a
: "${MSSQL_TEST_HOST:=localhost}"; : "${MSSQL_TEST_PORT:=1433}"
: "${MSSQL_TEST_USER:=sa}"; : "${MSSQL_TEST_PASS:=TestPassword1}"
DSN="Server=${MSSQL_TEST_HOST},${MSSQL_TEST_PORT};Database=TestDB;User Id=${MSSQL_TEST_USER};Password=${MSSQL_TEST_PASS}"

PAIRS=${PAIRS:-4}
ITERS=${ITERS:-8}
OUT=${OUT:-/tmp/bench_058_t0.log}

SQLFILE=$(mktemp "${TMPDIR:-/tmp}/b058.XXXXXX.sql")
ERRFILE=$(mktemp "${TMPDIR:-/tmp}/b058.XXXXXX.err")
trap 'rm -f "$SQLFILE" "$ERRFILE"' EXIT

median() { sort -n | awk '{a[NR]=$1} END {print (NR%2 ? a[(NR+1)/2] : (a[NR/2]+a[NR/2+1])/2)}'; }

attach_sql() { echo "ATTACH '${DSN}' AS db (TYPE mssql);"; }

# Fixtures: NOT NULL columns so every row is a ROW token (the branch the 2x
# patches instrument), bounded types only (no PLP — MAX columns are out of
# scope for D1 by design).
setup_fixtures() {
	{
		attach_sql
		cat <<'SQL'
SELECT mssql_exec('db', 'IF OBJECT_ID(''dbo.b58_1col'') IS NULL
CREATE TABLE dbo.b58_1col (c_bigint bigint NOT NULL)');
SELECT mssql_exec('db', 'IF OBJECT_ID(''dbo.b58_4col'') IS NULL
CREATE TABLE dbo.b58_4col (c_bigint bigint NOT NULL, c_int int NOT NULL, c_dec decimal(18,4) NOT NULL, c_str nvarchar(16) NOT NULL)');
SELECT mssql_exec('db', 'IF OBJECT_ID(''dbo.b58_15col'') IS NULL
CREATE TABLE dbo.b58_15col (
  c1 bigint NOT NULL, c2 int NOT NULL, c3 smallint NOT NULL, c4 tinyint NOT NULL,
  c5 bit NOT NULL, c6 real NOT NULL, c7 float NOT NULL, c8 decimal(9,2) NOT NULL,
  c9 decimal(18,4) NOT NULL, c10 decimal(38,10) NOT NULL, c11 date NOT NULL,
  c12 datetime2(3) NOT NULL, c13 time(3) NOT NULL, c14 uniqueidentifier NOT NULL,
  c15 nvarchar(16) NOT NULL)');
SQL
		echo "SELECT CASE WHEN (SELECT count(*) FROM db.dbo.b58_1col) = 2000000 THEN 'F1 OK' ELSE 'F1 LOAD' END;"
		echo "SELECT CASE WHEN (SELECT count(*) FROM db.dbo.b58_4col) = 1000000 THEN 'F2 OK' ELSE 'F1 LOAD' END;"
		echo "SELECT CASE WHEN (SELECT count(*) FROM db.dbo.b58_15col) = 500000 THEN 'F3 OK' ELSE 'F1 LOAD' END;"
	} > "$SQLFILE"
	local probe
	probe=$("$1" < "$SQLFILE" 2>&1) || { echo "fixture probe failed:"; echo "$probe" | grep -i error; exit 1; }
	if echo "$probe" | grep -q 'F1 LOAD'; then
		echo "[b058] loading fixtures (one-time)..."
		{
			attach_sql
			cat <<'SQL'
SELECT mssql_exec('db', 'TRUNCATE TABLE dbo.b58_1col');
SELECT mssql_exec('db', 'TRUNCATE TABLE dbo.b58_4col');
SELECT mssql_exec('db', 'TRUNCATE TABLE dbo.b58_15col');
COPY (SELECT i AS c_bigint FROM range(2000000) t(i)) TO 'db.dbo.b58_1col' (FORMAT bcp, CREATE_TABLE false);
COPY (SELECT i AS c_bigint, (i % 1000000)::INTEGER AS c_int,
             ((i % 99999) / 10.0)::DECIMAL(18,4) AS c_dec,
             'v' || (i % 10000) AS c_str
      FROM range(1000000) t(i)) TO 'db.dbo.b58_4col' (FORMAT bcp, CREATE_TABLE false);
COPY (SELECT i AS c1, (i % 100000)::INTEGER AS c2, (i % 30000)::SMALLINT AS c3,
             (i % 200)::UTINYINT AS c4, (i % 2 = 0) AS c5, (i * 1.5)::FLOAT AS c6,
             (i * 2.25)::DOUBLE AS c7, ((i % 9999) / 100.0)::DECIMAL(9,2) AS c8,
             ((i % 99999) / 10.0)::DECIMAL(18,4) AS c9,
             ((i % 999999) * 1.0)::DECIMAL(38,10) AS c10,
             (DATE '2020-01-01' + ((i % 3000)::INTEGER)) AS c11,
             (TIMESTAMP '2024-01-15 12:34:56.789' + INTERVAL (i % 90000) SECOND) AS c12,
             (TIME '01:02:03.456' + INTERVAL (i % 1000) SECOND)::TIME AS c13,
             uuid() AS c14, 'w' || (i % 10000) AS c15
      FROM range(500000) t(i)) TO 'db.dbo.b58_15col' (FORMAT bcp, CREATE_TABLE false);
SQL
		} > "$SQLFILE"
		"$1" < "$SQLFILE" > /dev/null 2>"$ERRFILE" || { echo "fixture load failed:"; grep -i error "$ERRFILE"; exit 1; }
	fi
}

# One process: ATTACH + ITERS drains of one fixture. Echoes "user sys" (seconds).
TIMEFORMAT='%3U %3S'
run_read() { # <binary> <table> <select> <counters:0|1>
	local bin="$1" tbl="$2" sel="$3" want_counters="$4"
	{
		attach_sql
		for _ in $(seq 1 "$ITERS"); do echo "${sel};"; done
	} > "$SQLFILE"
	# Exit status is tested EXPLICITLY: an earlier form echoed STEPFAIL inside
	# the timed block, but bash prints the TIMEFORMAT line AFTER it, so
	# `tail -n1` always captured the timing and a crashed variant binary flowed
	# near-zero CPU into the medians as a phantom improvement (review finding).
	local out rc
	if [ "$want_counters" = "1" ]; then
		# `|| rc=$?` NOT `; rc=$?`: under `set -e` (line 18) a bare assignment
		# from a failing command substitution aborts the script before the next
		# statement runs, so the explicit check below was itself unreachable —
		# the operator got a silent abort instead of the FAIL path's diagnosis.
		rc=0; out=$( { time { MSSQL_COUNTERS=1 "$bin" < "$SQLFILE" >/dev/null 2>"$ERRFILE"; } ; } 2>&1 ) || rc=$?
	else
		rc=0; out=$( { time { "$bin" < "$SQLFILE" >/dev/null 2>"$ERRFILE"; } ; } 2>&1 ) || rc=$?
	fi
	if [ "$rc" -ne 0 ]; then echo FAIL; else echo "$out" | tail -n1; fi
}

startup_cpu() { # <binary> — median of 3 (a single sample biased every
	# subtracted number AND every win identically; review finding). The first
	# run also absorbs macOS first-exec overhead before the samples we keep.
	echo "$(attach_sql) SELECT 1;" > "$SQLFILE"
	"$1" < "$SQLFILE" >/dev/null 2>&1 || true
	local t vals=""
	for _ in 1 2 3; do
		t=$( { time { "$1" < "$SQLFILE" >/dev/null 2>&1 || true; } ; } 2>&1 | tail -n1 )
		vals="$vals$(echo "$t" | awk '{printf "%.3f\n", $1 + $2}')"$'\n'
	done
	printf '%s' "$vals" | grep -v '^$' | median
}

FIX_TBL=(b58_1col b58_4col b58_15col)
FIX_SEL=("SELECT min(c_bigint) FROM db.dbo.b58_1col"
         "SELECT min(c_bigint), min(c_int), min(c_dec), min(c_str) FROM db.dbo.b58_4col"
         "SELECT min(c1),min(c2),min(c3),min(c4),min(c5),min(c6),min(c7),min(c8),min(c9),min(c10),min(c11),min(c12),min(c13),min(c14),min(c15) FROM db.dbo.b58_15col")
FIX_ROWS=(2000000 1000000 500000)
FIX_COLS=(1 4 15)

mode="${1:-phases}"
case "$mode" in
phases)
	bin="${2:-./build/release/duckdb}"
	setup_fixtures "$bin"
	echo "[b058] T0b phase split, binary=$bin, iters=$ITERS" | tee "$OUT"
	for f in 0 1 2; do
		# Capture rather than discard: `> /dev/null` threw the FAIL marker away,
		# so a crashed binary produced an empty counters section that reads as
		# "emitted no counters" rather than "died". The `ab` branch has always
		# checked this; this one did not, and adding `|| true` to the grep below
		# made the silence quieter still.
		t=$(run_read "$bin" "${FIX_TBL[$f]}" "${FIX_SEL[$f]}" 1)
		echo "--- ${FIX_TBL[$f]} (${FIX_ROWS[$f]} rows x ${FIX_COLS[$f]} cols) ---" | tee -a "$OUT"
		if [ "$t" = "FAIL" ]; then
			echo "RUN FAILED (phases, ${FIX_TBL[$f]})" | tee -a "$OUT"
			grep -i error "$ERRFILE" | head -3 || true
			exit 1
		fi
		# `|| true` stays, but now only covers the genuinely-empty grep: a binary
		# that ran fine and emitted no counter line.
		grep 'ns/row' "$ERRFILE" | tee -a "$OUT" || true
	done
	;;
ab)
	base="$2"; variant="$3"; label="$4"
	setup_fixtures "$base"
	su_base=$(startup_cpu "$base"); su_var=$(startup_cpu "$variant")
	echo "[b058] A/B $label: base=$base variant=$variant pairs=$PAIRS iters=$ITERS startup(base=$su_base var=$su_var)" | tee -a "$OUT"
	printf 'fixture\tbase_cpu_s\tvar_cpu_s\tdelta_s\tdelta_ns_per_value\tpair_wins\n' | tee -a "$OUT"
	for f in 0 1 2; do
		base_cpus=""; var_cpus=""; wins=0
		for p in $(seq 1 "$PAIRS"); do
			if [ $((p % 2)) -eq 1 ]; then order="base var"; else order="var base"; fi
			for who in $order; do
				if [ "$who" = base ]; then t=$(run_read "$base" "${FIX_TBL[$f]}" "${FIX_SEL[$f]}" 0); else t=$(run_read "$variant" "${FIX_TBL[$f]}" "${FIX_SEL[$f]}" 0); fi
				[ "$t" = "FAIL" ] && { echo "RUN FAILED ($who, ${FIX_TBL[$f]})"; grep -i error "$ERRFILE" | head -3 || true; exit 1; }
				cpu=$(echo "$t" | awk -v s="$([ "$who" = base ] && echo "$su_base" || echo "$su_var")" '{printf "%.3f", $1 + $2 - s}')
				if [ "$who" = base ]; then base_cpus="$base_cpus$cpu"$'\n'; b_last="$cpu"; else var_cpus="$var_cpus$cpu"$'\n'; v_last="$cpu"; fi
			done
			awk -v b="$b_last" -v v="$v_last" 'BEGIN {exit !(v > b)}' && wins=$((wins+1))
		done
		bm=$(printf '%s' "$base_cpus" | grep -v '^$' | median)
		vm=$(printf '%s' "$var_cpus" | grep -v '^$' | median)
		nv=$(awk -v b="$bm" -v v="$vm" -v r="${FIX_ROWS[$f]}" -v c="${FIX_COLS[$f]}" -v i="$ITERS" \
			'BEGIN {printf "%.2f", (v - b) * 1e9 / (r * c * i)}')
		d=$(awk -v b="$bm" -v v="$vm" 'BEGIN {printf "%.3f", v - b}')
		printf '%s\t%s\t%s\t%s\t%s\t%d/%d\n' "${FIX_TBL[$f]}" "$bm" "$vm" "$d" "$nv" "$wins" "$PAIRS" | tee -a "$OUT"
	done
	;;
*)
	echo "usage: $0 phases [binary] | ab <base> <variant> <label>"; exit 1;;
esac
echo "[b058] log: $OUT"
