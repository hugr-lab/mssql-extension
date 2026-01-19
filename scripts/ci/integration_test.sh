#!/bin/bash
# integration_test.sh - Full SQL Server integration test for Linux
#
# Usage: integration_test.sh <duckdb_cli> <extension_path>
#   duckdb_cli: Path to DuckDB CLI executable
#   extension_path: Path to the .duckdb_extension file
#
# Prerequisites:
#   - SQL Server container running and healthy
#   - Environment variables set: MSSQL_TEST_HOST, MSSQL_TEST_PORT, MSSQL_TEST_USER, MSSQL_TEST_PASS, MSSQL_TEST_DB
#
# Exit codes:
#   0: All tests pass
#   1: Test failure

set -euo pipefail

DUCKDB_CLI="${1:-}"
EXTENSION_PATH="${2:-}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SQL_DIR="$(dirname "$SCRIPT_DIR")/sql"

# Default test environment (can be overridden by env vars)
export MSSQL_TEST_HOST="${MSSQL_TEST_HOST:-localhost}"
export MSSQL_TEST_PORT="${MSSQL_TEST_PORT:-1433}"
export MSSQL_TEST_USER="${MSSQL_TEST_USER:-sa}"
export MSSQL_TEST_PASS="${MSSQL_TEST_PASS:-TestPassword1}"
export MSSQL_TEST_DB="${MSSQL_TEST_DB:-master}"

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
    echo "=== Integration Test PASSED ==="
    exit 0
else
    echo ""
    echo "=== Integration Test FAILED ==="
    exit 1
fi
