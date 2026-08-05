#!/bin/bash
# integration_test.sh - Full SQL Server integration test for Linux
#
# Usage: integration_test.sh <duckdb_cli> <extension_path> [unittest_binary]
#   duckdb_cli:      Path to DuckDB CLI executable
#   extension_path:  Path to the .duckdb_extension file
#   unittest_binary: Path to the SQLLogicTest runner (build/release/test/unittest).
#                    Optional for backwards compatibility; when omitted only the
#                    smoke test runs and the .test suite is SKIPPED with a warning.
#
# Prerequisites:
#   - SQL Server container running and healthy
#   - The test database seeded via docker/init/init.sql (creates TestDB + master.dbo.test).
#     Without it the .test files fail rather than skip — they expect that seed data.
#   - Environment variables set: MSSQL_TEST_HOST, MSSQL_TEST_PORT, MSSQL_TEST_USER, MSSQL_TEST_PASS, MSSQL_TEST_DB
#
# Exit codes:
#   0: All tests pass
#   1: Test failure

set -euo pipefail

DUCKDB_CLI="${1:-}"
EXTENSION_PATH="${2:-}"
UNITTEST_BIN="${3:-}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SQL_DIR="$(dirname "$SCRIPT_DIR")/sql"
REPO_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"

# Default test environment (can be overridden by env vars)
export MSSQL_TEST_HOST="${MSSQL_TEST_HOST:-localhost}"
export MSSQL_TEST_PORT="${MSSQL_TEST_PORT:-1433}"
export MSSQL_TEST_USER="${MSSQL_TEST_USER:-sa}"
export MSSQL_TEST_PASS="${MSSQL_TEST_PASS:-TestPassword1}"
export MSSQL_TEST_DB="${MSSQL_TEST_DB:-master}"

# Connection strings the .test files gate on via `require-env`. These mirror the Makefile
# definitions verbatim — they used to exist ONLY in the Makefile, so CI never set them and every
# .test file silently skipped (issue #192). A `require-env` miss is a SKIP, not a failure, so
# getting these wrong makes the suite pass by doing nothing. Keep in sync with the Makefile.
export MSSQL_TEST_DSN="${MSSQL_TEST_DSN:-Server=${MSSQL_TEST_HOST},${MSSQL_TEST_PORT};Database=${MSSQL_TEST_DB};User Id=${MSSQL_TEST_USER};Password=${MSSQL_TEST_PASS}}"
export MSSQL_TEST_URI="${MSSQL_TEST_URI:-mssql://${MSSQL_TEST_USER}:${MSSQL_TEST_PASS}@${MSSQL_TEST_HOST}:${MSSQL_TEST_PORT}/${MSSQL_TEST_DB}}"
export MSSQL_TESTDB_DSN="${MSSQL_TESTDB_DSN:-Server=${MSSQL_TEST_HOST},${MSSQL_TEST_PORT};Database=TestDB;User Id=${MSSQL_TEST_USER};Password=${MSSQL_TEST_PASS}}"
export MSSQL_TESTDB_URI="${MSSQL_TESTDB_URI:-mssql://${MSSQL_TEST_USER}:${MSSQL_TEST_PASS}@${MSSQL_TEST_HOST}:${MSSQL_TEST_PORT}/TestDB}"
export MSSQL_TEST_SERVER="${MSSQL_TEST_SERVER:-$MSSQL_TEST_DSN}"
export MSSQL_TEST_CONNECTION_STRING="${MSSQL_TEST_CONNECTION_STRING:-$MSSQL_TEST_DSN}"

if [[ -z "$DUCKDB_CLI" ]] || [[ -z "$EXTENSION_PATH" ]]; then
    echo "ERROR: Both arguments required" >&2
    echo "Usage: $0 <duckdb_cli> <extension_path>" >&2
    exit 1
fi

if [[ ! -x "$DUCKDB_CLI" ]]; then
    echo "ERROR: DuckDB CLI not found or not executable: $DUCKDB_CLI" >&2
    exit 1
fi

if [[ ! -f "$EXTENSION_PATH" ]]; then
    echo "ERROR: Extension file not found: $EXTENSION_PATH" >&2
    exit 1
fi

if [[ ! -f "$SQL_DIR/smoke_test.sql" ]]; then
    echo "ERROR: Smoke test SQL not found: $SQL_DIR/smoke_test.sql" >&2
    exit 1
fi

echo "=== SQL Server Integration Test ==="
echo "DuckDB CLI: $DUCKDB_CLI"
echo "Extension: $EXTENSION_PATH"
echo ""
echo "Test Environment:"
echo "  MSSQL_TEST_HOST: $MSSQL_TEST_HOST"
echo "  MSSQL_TEST_PORT: $MSSQL_TEST_PORT"
echo "  MSSQL_TEST_USER: $MSSQL_TEST_USER"
echo "  MSSQL_TEST_DB: $MSSQL_TEST_DB"
echo ""

# Check if SQL Server is reachable (basic TCP check)
echo "Checking SQL Server connectivity..."
# Use nc (netcat) for portable TCP check across Linux and macOS
if command -v nc &>/dev/null; then
    if ! nc -z "$MSSQL_TEST_HOST" "$MSSQL_TEST_PORT" 2>/dev/null; then
        echo "ERROR: Cannot connect to SQL Server at $MSSQL_TEST_HOST:$MSSQL_TEST_PORT" >&2
        echo "Make sure SQL Server is running and the port is accessible." >&2
        exit 1
    fi
else
    # Fallback to bash /dev/tcp (Linux only)
    if ! timeout 5 bash -c "echo > /dev/tcp/$MSSQL_TEST_HOST/$MSSQL_TEST_PORT" 2>/dev/null; then
        echo "ERROR: Cannot connect to SQL Server at $MSSQL_TEST_HOST:$MSSQL_TEST_PORT" >&2
        echo "Make sure SQL Server is running and the port is accessible." >&2
        exit 1
    fi
fi
echo "SQL Server is reachable."
echo ""

# Create a temporary SQL file that loads the extension first
TEMP_SQL=$(mktemp)
trap "rm -f $TEMP_SQL" EXIT

cat > "$TEMP_SQL" << EOF
-- Load extension first
LOAD '$EXTENSION_PATH';

EOF

# Append the smoke test SQL
cat "$SQL_DIR/smoke_test.sql" >> "$TEMP_SQL"

echo "Running integration tests..."
echo ""

if "$DUCKDB_CLI" --unsigned < "$TEMP_SQL"; then
    echo ""
    echo "=== Smoke test PASSED ==="
else
    echo ""
    echo "=== Smoke test FAILED ==="
    exit 1
fi

# ---------------------------------------------------------------------------
# SQLLogicTest suite (test/sql/**/*.test)
#
# The smoke test above only proves the extension loads and can talk to the
# server. The real coverage is the .test files, which need the `unittest`
# runner — the DuckDB CLI cannot execute them.
# ---------------------------------------------------------------------------
echo ""
echo "=== SQLLogicTest suite ==="

if [[ -z "$UNITTEST_BIN" ]]; then
    echo "WARNING: no unittest binary passed - SKIPPING the .test suite." >&2
    echo "         Pass it as the 3rd argument to run test/sql/**/*.test." >&2
    echo "=== Integration Test PASSED (smoke only) ==="
    exit 0
fi

if [[ ! -x "$UNITTEST_BIN" ]]; then
    echo "ERROR: unittest binary not found or not executable: $UNITTEST_BIN" >&2
    exit 1
fi

echo "Runner: $UNITTEST_BIN"
echo "  MSSQL_TEST_DSN:   Server=${MSSQL_TEST_HOST},${MSSQL_TEST_PORT};Database=${MSSQL_TEST_DB};User Id=${MSSQL_TEST_USER};Password=***"
echo "  MSSQL_TESTDB_DSN: Server=${MSSQL_TEST_HOST},${MSSQL_TEST_PORT};Database=TestDB;User Id=${MSSQL_TEST_USER};Password=***"
echo ""

# Fail if the seed is missing rather than letting every test skip or fail one by one.
if ! "$DUCKDB_CLI" --unsigned -c "
LOAD '$EXTENSION_PATH';
ATTACH '${MSSQL_TESTDB_DSN}' AS seedcheck (TYPE mssql);
SELECT count(*) FROM seedcheck.dbo.TestSimplePK;
" > /dev/null 2>&1; then
    echo "ERROR: TestDB is missing or unseeded - apply docker/init/init.sql first." >&2
    echo "       The .test files expect that seed data; without it they fail, not skip." >&2
    exit 1
fi

cd "$REPO_ROOT"

# The runner reports `require-env` misses as skips, never failures, so a missing variable would
# quietly reduce the suite to a no-op. The skip list is printed at the end of its output; the
# expected skips are the optional environments (Azure, Kerberos, TLS, Windows SSPI).
# A filter that matches nothing exits 0. That is not a hypothetical here: this
# job has been reporting PASSED while running ZERO tests (issue #212 — "test/sql/*
# selects zero files"), verified on the last scheduled runs of main as well as on
# this one. The exit code cannot tell the difference, so the COUNT is checked.
#
# The Makefile's integration-test target grew the same floor (spec 063 D7), and
# that was not enough: CI does not call it, it calls this script. A gate on one of
# two entry points is a gate on neither.
MIN_TEST_CASES=${MSSQL_MIN_TEST_CASES:-150}
set -o pipefail
"$UNITTEST_BIN" "test/sql/*" 2>&1 | tee /tmp/mssql_integration_ci.out
status=$?

cases=$(grep -oE '[0-9]+ test cases?' /tmp/mssql_integration_ci.out | tail -1 | grep -oE '^[0-9]+' || true)
if [ -z "$cases" ]; then
    cases=0
fi

if [ "$status" -ne 0 ]; then
    echo ""
    echo "=== Integration Test FAILED ($cases cases ran) ==="
    exit 1
fi

if [ "$cases" -lt "$MIN_TEST_CASES" ]; then
    echo ""
    echo "=== Integration Test ran $cases cases, expected at least $MIN_TEST_CASES ===" >&2
    echo "    The runner exited 0 having selected (almost) nothing — see issue #212." >&2
    echo "    A green job here means the suite RAN, not merely that it did not fail." >&2
    exit 1
fi

echo ""
echo "=== Integration Test PASSED ($cases cases) ==="
exit 0
