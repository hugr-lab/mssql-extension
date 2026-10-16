#!/usr/bin/env bash
#
# Open the Joern REPL on a CPG built by joern/build.sh, with the helpers from
# joern/shell.sc in scope.
#
#   joern/shell.sh [tds|all]
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCOPE="${1:-tds}"
OUT_DIR="${JOERN_OUT:-$ROOT/joern/out}"
CPG="$OUT_DIR/$SCOPE.cpg.bin"

if [ ! -f "$CPG" ]; then
	echo "no CPG at $CPG — run: joern/build.sh $SCOPE" >&2
	exit 1
fi

export JAVA_OPTS="${JAVA_OPTS:--Xmx${JOERN_HEAP:-8g}}"

# Run from joern/out/ so Joern's own `workspace/` directory (a working copy of
# every CPG it opens) lands in the gitignored tree, not in the repo root.
cd "$OUT_DIR"

echo "Loading $CPG — helpers: wireEntry, parsers, copies, allocs, indexes, casts,"
echo "where(node); call cheatsheet() for the list and some starting queries."
exec joern --import "$ROOT/joern/shell.sc" "$CPG"
