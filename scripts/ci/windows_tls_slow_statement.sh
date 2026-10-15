#!/usr/bin/env bash
# Issue #457 on Windows: a statement that runs for seconds over TLS, its
# response read in 1 s slices that each time out on the socket (WSAETIMEDOUT
# inside SSL_read, SSL_ERROR_SYSCALL to OpenSSL), must succeed with its count
# and leave the connection usable -- measured to hold (CI run 38688186396).
# The same scenario as test/sql/integration/tls_slow_statement.test, through
# the CLI: the Windows lane builds no sqllogictest runner.
#
# The CLI at the 2.0 pin does not fail on a runtime error (it prints nothing
# and exits 0), so the output is compared line by line with what the
# statements return.
#
# usage: windows_tls_slow_statement.sh <duckdb cli> <mssql.duckdb_extension>
set -euo pipefail

cli="$1"
extension="$2"
dsn="mssql://${MSSQL_TEST_USER}:${MSSQL_TEST_PASS}@${MSSQL_TEST_HOST}:${MSSQL_TEST_PORT}/master?encrypt=true&trustservercertificate=true"

work="$(mktemp -d)"
cat >"$work/script.sql" <<EOF
LOAD '${extension}';
ATTACH '${dsn}' AS t457 (TYPE mssql);
SELECT mssql_exec('t457', 'IF OBJECT_ID(''dbo.tls_slow_457'') IS NOT NULL DROP TABLE dbo.tls_slow_457; CREATE TABLE dbo.tls_slow_457 (id int PRIMARY KEY, v int NOT NULL); INSERT INTO dbo.tls_slow_457 VALUES (1, 0), (2, 0), (3, 0)') IS NOT NULL;
SELECT mssql_exec('t457', 'CREATE TRIGGER dbo.tls_slow_457_wait ON dbo.tls_slow_457 AFTER UPDATE AS WAITFOR DELAY ''00:00:03''') IS NOT NULL;
SELECT mssql_invalidate_cache('t457') IS NOT NULL;
UPDATE t457.dbo.tls_slow_457 SET v = v + 1 WHERE id <= 2;
SELECT string_agg(id || ':' || v, ';' ORDER BY id) FROM t457.dbo.tls_slow_457;
MERGE INTO t457.dbo.tls_slow_457 AS t USING (SELECT * FROM (VALUES (3, 5)) AS x(id, w)) AS s ON t.id = s.id WHEN MATCHED THEN UPDATE SET v = s.w;
BEGIN;
UPDATE t457.dbo.tls_slow_457 SET v = 9 WHERE id = 1;
SELECT v FROM t457.dbo.tls_slow_457 WHERE id = 1;
COMMIT;
SELECT x FROM mssql_scan('t457', 'WAITFOR DELAY ''00:00:03''; SELECT 42 AS x');
SELECT string_agg(id || ':' || v, ';' ORDER BY id) FROM t457.dbo.tls_slow_457;
SELECT mssql_exec('t457', 'DROP TABLE dbo.tls_slow_457') IS NOT NULL;
EOF

# The CLI prints no count for an UPDATE / MERGE: each one is checked by the
# rows read after it.
cat >"$work/expected.txt" <<'EOF'
true
true
true
1:1;2:1;3:0
9
42
1:9;2:1;3:5
true
EOF

start=$(date +%s)
set +e
"$cli" -unsigned -csv -noheader <"$work/script.sql" >"$work/actual_raw.txt" 2>"$work/stderr.txt"
status=$?
set -e
elapsed=$(($(date +%s) - start))
tr -d '\r' <"$work/actual_raw.txt" >"$work/actual.txt"

echo "CLI exit status: $status, elapsed: ${elapsed} s (four statements wait 3 s each)"
if ! diff -u "$work/expected.txt" "$work/actual.txt"; then
	echo "--- stderr:"
	cat "$work/stderr.txt"
	echo "FAIL: output differs from what each statement returns (issue #457)"
	exit 1
fi
if [ "$status" -ne 0 ]; then
	echo "--- stderr:"
	cat "$work/stderr.txt"
	echo "FAIL: CLI exited with $status"
	exit 1
fi
echo "PASS: every slow statement over TLS returned its result, and the connection served the next one"
