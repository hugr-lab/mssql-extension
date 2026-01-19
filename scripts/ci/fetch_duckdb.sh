#!/bin/bash
# fetch_duckdb.sh - Fetch DuckDB sources by version tag or main branch
#
# Usage: fetch_duckdb.sh <version>
#   version: DuckDB version (e.g., "1.4.1") or "main" for nightly
#
# Output:
#   - Clones DuckDB to ./duckdb/
#   - Prints commit hash to stdout
#
# Exit codes:
#   0: Success
#   1: Failed to clone or invalid version

set -euo pipefail

VERSION="${1:-}"

if [[ -z "$VERSION" ]]; then
    echo "ERROR: DuckDB version required" >&2
    echo "Usage: $0 <version>" >&2
    echo "  version: DuckDB version (e.g., '1.4.1') or 'main' for nightly" >&2
    exit 1
fi

# Remove existing duckdb directory if present
if [[ -d "duckdb" ]]; then
    echo "Removing existing duckdb directory..." >&2
    rm -rf duckdb
fi

# Determine branch/tag to clone
if [[ "$VERSION" == "main" ]]; then
    BRANCH="main"
    echo "Fetching DuckDB main branch (nightly)..." >&2
else
    BRANCH="v${VERSION}"
    echo "Fetching DuckDB version ${VERSION} (tag ${BRANCH})..." >&2
fi

# Clone DuckDB
if ! git clone --depth 1 --branch "$BRANCH" https://github.com/duckdb/duckdb.git 2>&2; then
    echo "ERROR: Failed to clone DuckDB branch/tag: $BRANCH" >&2
    echo "Check if the version exists: https://github.com/duckdb/duckdb/releases" >&2
    exit 1
fi

# Get and output commit hash
COMMIT_HASH=$(git -C duckdb rev-parse HEAD)
echo "DuckDB commit: $COMMIT_HASH" >&2

# Output commit hash to stdout (for capture by caller)
echo "$COMMIT_HASH"
