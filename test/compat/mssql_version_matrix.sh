#!/usr/bin/env bash
#
# Local SQL Server version × charset compatibility matrix.
#
# NOT a CI job, deliberately: it pulls ~8 GB of images and, on Apple silicon,
# runs four of the five under amd64 emulation. Minutes per version, not seconds.
# Run it on a laptop before touching the login handshake or the string decode
# path, which is where the extension's version-dependent behaviour actually
# lives.
#
# WHAT IT ANSWERS
#   1. Does LOGIN7 succeed at all on this server version? The feature list in
#      LOGIN7 is version-sensitive: a server that dislikes what we advertise
#      rejects the login with 18456, which is indistinguishable from a wrong
#      password. That failure mode has already shipped once (issue #225,
#      UTF8_SUPPORT carrying a data byte, rejected by Azure SQL only).
#   2. Does the server grant UTF8SUPPORT, and does it even have UTF-8
#      collations? SQL Server 2019 introduced both; 2017 has neither, which is
#      the negative case the .test suite cannot reach because CI runs one
#      version.
#   3. Do all four UTF-8 width classes round-trip, for each of the storage
#      shapes the extension frames differently: UTF-8 collation (raw bytes),
#      NVARCHAR (UTF-16 batch decode), DBCS collation and single-byte code page
#      (server-side CAST)?
#   4. What does each width class actually cost on the wire, with the feature
#      on and off?
#
# USAGE
#   test/compat/mssql_version_matrix.sh [--versions 2019,2022] [--port 14333]
#                                       [--keep] [--json out.json]
#
#   --versions  comma-separated subset of: 2017 2019 2022 2025 edge  (default: all)
#   --port      host port to publish on. NOT 1433 by default — a developer
#               laptop usually already has something there.
#   --keep      leave the last container running for poking at by hand
#   --json      also write machine-readable results, for diffing across runs
#
# REQUIREMENTS
#   docker, and a release build of the extension (`make release`). The DuckDB
#   binary at build/release/duckdb is what gets exercised — this measures the
#   extension, not sqlcmd.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

PORT=14333
KEEP=0
JSON_OUT=""
VERSIONS="2017,2019,2022,2025,edge"
SA_PASS="Matrix#Passw0rd"
DUCKDB_BIN="build/release/duckdb"

while [ $# -gt 0 ]; do
	case "$1" in
	--versions) VERSIONS="$2"; shift 2 ;;
	--port)     PORT="$2";     shift 2 ;;
	--keep)     KEEP=1;        shift   ;;
	--json)     JSON_OUT="$2"; shift 2 ;;
	-h|--help)  sed -n '2,40p' "$0"; exit 0 ;;
	*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

image_for() {
	case "$1" in
	2017) echo "mcr.microsoft.com/mssql/server:2017-latest" ;;
	2019) echo "mcr.microsoft.com/mssql/server:2019-latest" ;;
	2022) echo "mcr.microsoft.com/mssql/server:2022-latest" ;;
	2025) echo "mcr.microsoft.com/mssql/server:2025-latest" ;;
	edge) echo "mcr.microsoft.com/azure-sql-edge:latest" ;;
	*) return 1 ;;
	esac
}

CONTAINER="mssql-matrix-$$"
RESULTS=()

cleanup() {
	if [ "$KEEP" = "1" ]; then
		echo "--keep: leaving $CONTAINER running on port $PORT"
		return
	fi
	docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

#-----------------------------------------------------------------------------
# sqlcmd is NOT in a fixed place, and for one image it is not there at all.
#
#   2017 / 2019  /opt/mssql-tools/bin/sqlcmd     (no -C flag; it does not
#                                                 validate the cert anyway)
#   2022 / 2025  /opt/mssql-tools18/bin/sqlcmd   (-C required, or it refuses
#                                                 the self-signed cert)
#   azure-sql-edge  ships no sqlcmd whatsoever
#
# For the last case we drive it from a sidecar sharing the container's network
# namespace, so `localhost` inside the sidecar is the server.
#-----------------------------------------------------------------------------
TOOLS_IMAGE="mcr.microsoft.com/mssql-tools:latest"
SQLCMD_MODE=""   # "in:<path>" or "sidecar"

detect_sqlcmd() {
	local p
	for p in /opt/mssql-tools18/bin/sqlcmd /opt/mssql-tools/bin/sqlcmd; do
		if docker exec "$CONTAINER" test -x "$p" 2>/dev/null; then
			SQLCMD_MODE="in:$p"
			return 0
		fi
	done
	docker image inspect "$TOOLS_IMAGE" >/dev/null 2>&1 || docker pull -q "$TOOLS_IMAGE" >/dev/null 2>&1
	SQLCMD_MODE="sidecar"
}

# sql <args...> — run sqlcmd against the server under test.
sql() {
	case "$SQLCMD_MODE" in
	in:*)
		local path="${SQLCMD_MODE#in:}"
		local extra=""
		[[ "$path" == *tools18* ]] && extra="-C"
		docker exec -i "$CONTAINER" "$path" -S localhost -U sa -P "$SA_PASS" $extra "$@"
		;;
	sidecar)
		docker run --rm -i --network "container:$CONTAINER" "$TOOLS_IMAGE" \
			/opt/mssql-tools/bin/sqlcmd -S localhost -U sa -P "$SA_PASS" "$@"
		;;
	esac
}

# Scalar query -> bare value on stdout.
sql1() { sql -h -1 -W -Q "SET NOCOUNT ON; $1" 2>/dev/null | head -1 | tr -d '\r'; }

#-----------------------------------------------------------------------------
# Readiness. Poll the SERVICE, never the port: a published port can accept a
# TCP connection and still reset every byte (observed with a different runtime),
# and SQL Server accepts connections before recovery finishes.
#-----------------------------------------------------------------------------
wait_ready() {
	local deadline=$((SECONDS + 300))
	while [ $SECONDS -lt $deadline ]; do
		if [ "$(sql1 'SELECT 1')" = "1" ]; then return 0; fi
		sleep 5
	done
	return 1
}

#-----------------------------------------------------------------------------
# Seed. Branches on UTF-8 collation availability: on 2017 there is none, and
# asking for one is a hard error rather than something to skip past.
#-----------------------------------------------------------------------------
seed() {
	local have_utf8="$1" utf8_col=""
	[ "$have_utf8" = "yes" ] && utf8_col="v_u8 VARCHAR(400) COLLATE Latin1_General_100_CI_AS_SC_UTF8 NULL,"

	sql -Q "
SET NOCOUNT ON;
IF DB_ID('MatrixDB') IS NULL CREATE DATABASE MatrixDB;
" >/dev/null 2>&1

	sql -d MatrixDB -Q "
SET NOCOUNT ON;
IF OBJECT_ID('dbo.Charsets') IS NOT NULL DROP TABLE dbo.Charsets;
CREATE TABLE dbo.Charsets (
  id INT NOT NULL,
  label VARCHAR(16) NOT NULL,
  ${utf8_col}
  v_nvarchar NVARCHAR(400) NULL,
  v_jp   VARCHAR(400) COLLATE Japanese_CI_AS NULL,
  v_1252 VARCHAR(400) COLLATE Latin1_General_CI_AS NULL
);
-- Every literal is N'...'. A bare 'literal' is parsed in the DATABASE code page
-- before it is stored, so on a CP1252 database the CJK rows would already be
-- '?' by the time they reached the column, whatever the column's collation.
INSERT INTO dbo.Charsets (id, label, v_nvarchar, v_jp, v_1252) VALUES
 (1,'ascii',   REPLICATE(N'a',40),                         REPLICATE(N'a',40),  REPLICATE(N'a',40)),
 (2,'cyrillic',REPLICATE(NCHAR(0x0434),40),                NULL,                NULL),
 (3,'cjk',     REPLICATE(NCHAR(0x6F22),40),                REPLICATE(NCHAR(0x6F22),40), NULL),
 (4,'emoji',   REPLICATE(NCHAR(0xD83D)+NCHAR(0xDE00),40),  NULL,                NULL),
 (5,'latin1',  REPLICATE(NCHAR(0x00E9),40),                NULL,                REPLICATE(NCHAR(0x00E9),40));
" >/dev/null 2>&1

	if [ "$have_utf8" = "yes" ]; then
		sql -d MatrixDB -Q "SET NOCOUNT ON; UPDATE dbo.Charsets SET v_u8 = v_nvarchar;" >/dev/null 2>&1
	fi
}

#-----------------------------------------------------------------------------
# Exercise the EXTENSION (not sqlcmd) and report what it saw.
#-----------------------------------------------------------------------------
dsn() { echo "Server=localhost,$PORT;Database=MatrixDB;User Id=sa;Password=$SA_PASS"; }

# Does this build know about mssql_utf8_support? Lets the script stay useful on
# a main-branch build, before the feature lands.
has_utf8_setting() {
	"$DUCKDB_BIN" -c "SET mssql_utf8_support=true;" >/dev/null 2>&1
}

# extension_login -> "ok" | "<error text>"
extension_login() {
	local out
	out=$("$DUCKDB_BIN" -c "ATTACH '$(dsn)' AS m (TYPE mssql); SELECT 1;" 2>&1)
	if echo "$out" | grep -qiE "error"; then
		echo "$out" | grep -iE "error" | head -1 | cut -c1-90
	else
		echo "ok"
	fi
}

# roundtrip <column> -> "pass" | "FAIL:<detail>" | "n/a"
# Compares what the extension returns against the same value fetched as
# NVARCHAR, which is the shape whose decode path is already well covered.
roundtrip() {
	local col="$1" pre="$2" out
	# -csv -noheader: the default box renderer draws with U+2502, not ASCII '|',
	# so anything grepping for a pipe silently matches nothing and every result
	# reads as "n/a" — which looks like a skip rather than a broken parser.
	out=$("$DUCKDB_BIN" -csv -noheader -c "
$pre
ATTACH '$(dsn)' AS m (TYPE mssql);
SELECT count(*) FROM m.dbo.Charsets
WHERE $col IS NOT NULL AND $col IS DISTINCT FROM v_nvarchar;" 2>/dev/null | tr -d '\r' | grep -E '^[0-9]+$' | head -1)
	if [ -z "$out" ]; then echo "n/a"; elif [ "$out" = "0" ]; then echo "pass"; else echo "FAIL:${out}_mismatch"; fi
}

# wire_bytes_per_row <label> <column> <feature> -> integer or "?"
# Per ROW, not total: the D4 stream counters are known to report a single chunk
# rather than the whole scan, so only a per-row figure is comparable.
wire_bytes_per_row() {
	local label="$1" col="$2" feat="$3" pre=""
	[ -n "$feat" ] && pre="SET mssql_utf8_support=$feat;"
	MSSQL_DEBUG=2 "$DUCKDB_BIN" -c "
SET threads=1; $pre
ATTACH '$(dsn)' AS m (TYPE mssql);
SELECT sum(length($col)) FROM m.dbo.Charsets WHERE label='$label';" 2>&1 \
	| grep "MSSQL COUNTERS] stream close" \
	| python3 -c "
import sys,re
rows=w=0
for L in sys.stdin:
    m=re.search(r'rows=(\d+).*wire_in=(\d+)B',L)
    if m: rows+=int(m.group(1)); w+=int(m.group(2))
print(round(w/rows,1) if rows else '?')
" 2>/dev/null || echo "?"
}

#-----------------------------------------------------------------------------
run_version() {
	local ver="$1" image
	image="$(image_for "$ver")" || { echo "unknown version: $ver" >&2; return 1; }

	echo
	echo "==============================================================="
	echo "  $ver  —  $image"
	echo "==============================================================="

	docker rm -f "$CONTAINER" >/dev/null 2>&1
	local platform_flag=""
	# Everything but Edge is amd64-only; be explicit so the emulation is a
	# deliberate choice rather than a surprise.
	[ "$ver" != "edge" ] && platform_flag="--platform linux/amd64"

	# shellcheck disable=SC2086
	if ! docker run -d --name "$CONTAINER" $platform_flag -p "$PORT:1433" -m 4G \
		-e ACCEPT_EULA=Y -e MSSQL_SA_PASSWORD="$SA_PASS" -e "MSSQL_PID=Developer" \
		"$image" >/dev/null 2>&1; then
		RESULTS+=("$ver|start|FAILED|-|-|-|-|-|-")
		echo "  container failed to start"
		return
	fi

	detect_sqlcmd
	echo "  sqlcmd: $SQLCMD_MODE"
	if ! wait_ready; then
		echo "  server never became ready; last log lines:"
		docker logs "$CONTAINER" 2>&1 | tail -5 | sed 's/^/    /'
		RESULTS+=("$ver|notready|FAILED|-|-|-|-|-|-")
		return
	fi

	local product edition collation utf8_colls
	product=$(sql1 "SELECT CONVERT(varchar(32), SERVERPROPERTY('ProductVersion'))")
	edition=$(sql1 "SELECT CONVERT(varchar(48), SERVERPROPERTY('Edition'))")
	collation=$(sql1 "SELECT CONVERT(varchar(64), SERVERPROPERTY('Collation'))")
	utf8_colls=$(sql1 "SELECT COUNT(*) FROM sys.fn_helpcollations() WHERE name LIKE '%[_]UTF8'")
	[ -z "$utf8_colls" ] && utf8_colls=0
	echo "  product=$product  edition=$edition"
	echo "  server collation=$collation   UTF-8 collations=$utf8_colls"

	local have_utf8="no"; [ "$utf8_colls" -gt 0 ] 2>/dev/null && have_utf8="yes"
	seed "$have_utf8"

	local login; login=$(extension_login)
	echo "  extension login: $login"
	if [ "$login" != "ok" ]; then
		RESULTS+=("$ver|$product|LOGIN_FAIL|$utf8_colls|-|-|-|-|-")
		return
	fi

	local pre_on="" pre_off=""
	if has_utf8_setting; then pre_on="SET mssql_utf8_support=true;"; pre_off="SET mssql_utf8_support=false;"; fi

	local rt_nv rt_jp rt_1252 rt_u8="n/a"
	rt_nv=$(roundtrip v_nvarchar "$pre_on")
	rt_jp=$(roundtrip v_jp   "$pre_on")
	rt_1252=$(roundtrip v_1252 "$pre_on")
	[ "$have_utf8" = "yes" ] && rt_u8=$(roundtrip v_u8 "$pre_on")
	echo "  round-trip: nvarchar=$rt_nv  dbcs(jp)=$rt_jp  cp1252=$rt_1252  utf8col=$rt_u8"

	# Wire cost per row, feature on vs off, on the UTF-8-collated column. This
	# is where the feature's benefit is claimed, and where it inverts by script.
	local wire=""
	if [ "$have_utf8" = "yes" ] && has_utf8_setting; then
		local s on off
		for s in ascii cyrillic cjk emoji; do
			on=$(wire_bytes_per_row "$s" v_u8 true)
			off=$(wire_bytes_per_row "$s" v_u8 false)
			wire+="$s:$on/$off "
			printf "  wire B/row  %-9s on=%-7s off=%-7s\n" "$s" "$on" "$off"
		done
	else
		wire="n/a"
		echo "  wire B/row: skipped ($( [ "$have_utf8" = yes ] && echo 'build has no mssql_utf8_support' || echo 'server has no UTF-8 collation' ))"
	fi

	RESULTS+=("$ver|$product|OK|$utf8_colls|$rt_nv|$rt_jp|$rt_1252|$rt_u8|$wire")
}

#-----------------------------------------------------------------------------
if ! docker info >/dev/null 2>&1; then
	echo "docker daemon is not running" >&2; exit 1
fi
if [ ! -x "$DUCKDB_BIN" ]; then
	echo "no $DUCKDB_BIN — run 'make release' first" >&2; exit 1
fi
if lsof -nP -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1; then
	echo "host port $PORT is already in use; pass --port" >&2; exit 1
fi

IFS=',' read -r -a WANTED <<< "$VERSIONS"
for v in "${WANTED[@]}"; do run_version "$v"; done

echo
echo "==============================================================="
echo "  SUMMARY"
echo "==============================================================="
printf "%-6s %-14s %-10s %-6s %-9s %-9s %-9s %-9s\n" ver product status u8col nvarchar dbcs cp1252 utf8col
for r in "${RESULTS[@]}"; do
	IFS='|' read -r ver prod st u8 nv jp l1 u8c _wire <<< "$r"
	printf "%-6s %-14s %-10s %-6s %-9s %-9s %-9s %-9s\n" "$ver" "$prod" "$st" "$u8" "$nv" "$jp" "$l1" "$u8c"
done
echo
for r in "${RESULTS[@]}"; do
	IFS='|' read -r ver _p _s _u _n _j _l _uc wire <<< "$r"
	[ "$wire" != "n/a" ] && [ -n "$wire" ] && echo "  $ver wire B/row (on/off): $wire"
done

if [ -n "$JSON_OUT" ]; then
	{
		echo "["
		first=1
		for r in "${RESULTS[@]}"; do
			IFS='|' read -r ver prod st u8 nv jp l1 u8c wire <<< "$r"
			[ $first -eq 0 ] && echo ","
			first=0
			printf '  {"version":"%s","product":"%s","status":"%s","utf8_collations":"%s",' "$ver" "$prod" "$st" "$u8"
			printf '"roundtrip":{"nvarchar":"%s","dbcs_jp":"%s","cp1252":"%s","utf8_collation":"%s"},' "$nv" "$jp" "$l1" "$u8c"
			printf '"wire_bytes_per_row":"%s"}' "$wire"
		done
		echo
		echo "]"
	} > "$JSON_OUT"
	echo "wrote $JSON_OUT"
fi

for r in "${RESULTS[@]}"; do
	case "$r" in *"|FAILED|"*|*"|LOGIN_FAIL|"*|*FAIL:*) exit 1 ;; esac
done
exit 0
