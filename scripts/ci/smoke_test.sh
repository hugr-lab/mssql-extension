#!/bin/bash
# smoke_test.sh - Run load-only smoke test for extension verification
#
# Usage: smoke_test.sh <duckdb_cli> <extension_path>
#   duckdb_cli: Path to DuckDB CLI executable
#   extension_path: Path to the .duckdb_extension file
#
# Exit codes:
#   0: Extension loads successfully
#   1: Extension failed to load

set -euo pipefail

DUCKDB_CLI="${1:-}"
EXTENSION_PATH="${2:-}"

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

echo "=== Load-Only Smoke Test ==="
echo "DuckDB CLI: $DUCKDB_CLI"
echo "Extension: $EXTENSION_PATH"
echo ""

# Create a temporary SQL file for the smoke test
SMOKE_SQL=$(mktemp)
trap "rm -f $SMOKE_SQL" EXIT

cat > "$SMOKE_SQL" << 'EOF'
-- Load-only smoke test
-- This test verifies the extension loads correctly without SQL Server connectivity

-- Load the extension
LOAD 'EXTENSION_PATH_PLACEHOLDER';

-- Verify extension is loaded
SELECT
    extension_name,
    loaded,
    installed
FROM duckdb_extensions()
WHERE extension_name = 'mssql';

-- Get extension version
SELECT mssql_version() AS version;

-- Success message
SELECT 'Extension loaded successfully!' AS status;
EOF

# Replace placeholder with actual extension path
sed -i.bak "s|EXTENSION_PATH_PLACEHOLDER|$EXTENSION_PATH|g" "$SMOKE_SQL"

echo "Running smoke test..."
echo ""

if "$DUCKDB_CLI" --unsigned < "$SMOKE_SQL"; then
    echo ""
    echo "=== Smoke Test PASSED ==="
    exit 0
else
    echo ""
    echo "=== Smoke Test FAILED ==="
    exit 1
fi
