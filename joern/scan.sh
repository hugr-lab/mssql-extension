#!/usr/bin/env bash
#
# Run the Joern query suite (joern/scan.sc) against a CPG built by
# joern/build.sh and write a markdown report under joern/out/.
#
# Usage:
#   joern/scan.sh [tds|all] [query-ids]
#
#   joern/scan.sh                 # tds scope, all queries
#   joern/scan.sh tds q3,q5       # only those queries
#   joern/scan.sh all q6          # credential-to-log over the whole extension
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCOPE="${1:-tds}"
ONLY="${2:-}"
OUT_DIR="${JOERN_OUT:-$ROOT/joern/out}"
CPG="$OUT_DIR/$SCOPE.cpg.bin"

# A single-query run writes its own file: report-<scope>.md is the full baseline
# that a later tree gets diffed against, and a partial run must not clobber it.
if [ -n "${2:-}" ]; then
	REPORT="$OUT_DIR/report-$SCOPE-$(echo "$2" | tr -cd '[:alnum:],' | tr ',' '-').md"
else
	REPORT="$OUT_DIR/report-$SCOPE.md"
fi

if ! command -v joern >/dev/null 2>&1; then
	echo "joern not found. Install it with 'brew install joern' (macOS) or see" >&2
	echo "https://docs.joern.io/installation" >&2
	exit 127
fi

if [ ! -f "$CPG" ]; then
	echo "no CPG at $CPG — build it first:" >&2
	echo "  joern/build.sh $SCOPE" >&2
	exit 1
fi

export JAVA_OPTS="${JAVA_OPTS:--Xmx${JOERN_HEAP:-8g}}"

PARAMS=(--param "cpgFile=$CPG")
[ -n "$ONLY" ] && PARAMS+=(--param "only=$ONLY")

# Joern creates a `workspace/` directory (a working copy of every CPG it opens,
# tens of MB) in the CURRENT directory and has no flag to move it, so run it
# from joern/out/ -- which is gitignored -- rather than from the repo root.
cd "$OUT_DIR"
joern --script "$ROOT/joern/scan.sc" "${PARAMS[@]}" | tee "$REPORT"

echo
echo "report: $REPORT"
