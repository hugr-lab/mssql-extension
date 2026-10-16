#!/usr/bin/env bash
#
# Build a Code Property Graph (CPG) of this extension's C++ sources for Joern.
#
# Joern's C/C++ frontend (c2cpg, Eclipse CDT based) parses source WITHOUT
# compiling it: no build, no vcpkg tree and no DuckDB object files are needed,
# and a missing header degrades the graph locally instead of failing the run.
# Include paths are still passed when they exist, because CDT resolves types
# and call targets far better with them (an unresolved callee shows up as
# `<unresolvedSignature>` and silently drops out of call-graph queries).
#
# Scopes (first argument):
#   tds   (default) the untrusted-input surface: src/tds + the SQL Browser
#         datagram parser + the row stager. This is the set the fuzz harnesses
#         cover (see fuzz/README.md), it is DuckDB-free, and it parses in
#         seconds -- use it for day-to-day queries.
#   all   every source file under src/. Slower and noisier (DuckDB headers pull
#         in a large template surface), but needed for whole-extension
#         questions such as "does a password reach a log sink".
#
# Usage:
#   joern/build.sh [tds|all]
#   JOERN_HEAP=12g joern/build.sh all
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCOPE="${1:-tds}"
OUT_DIR="${JOERN_OUT:-$ROOT/joern/out}"
CPG="$OUT_DIR/$SCOPE.cpg.bin"

if ! command -v joern-parse >/dev/null 2>&1; then
	echo "joern-parse not found. Install Joern first:" >&2
	echo "  brew install joern          # macOS" >&2
	echo "  https://docs.joern.io/installation" >&2
	exit 127
fi

mkdir -p "$OUT_DIR"

# c2cpg runs on the JVM; the default heap is not enough for the 'all' scope.
export JAVA_OPTS="${JAVA_OPTS:--Xmx${JOERN_HEAP:-8g}}"

# Include paths, best effort: only the ones that exist on this machine are
# passed. The vcpkg triplet directory differs per platform and is absent until
# a build has run.
INCLUDES=()
add_include() { [ -d "$1" ] && INCLUDES+=(--include "$1") || true; }
add_include "$ROOT/src/include"
add_include "$ROOT/duckdb/src/include"
add_include "$ROOT/duckdb/third_party/fmt/include"
for triplet in arm64-osx x64-osx x64-linux arm64-linux x64-windows-static-release; do
	add_include "$ROOT/build/release/vcpkg_installed/$triplet/include"
	add_include "$ROOT/build/debug/vcpkg_installed/$triplet/include"
done

case "$SCOPE" in
tds)
	# One directory plus two single files: c2cpg takes a single input path, so
	# the focused scope is staged as a tree of symlinks under out/.
	STAGE="$OUT_DIR/stage-tds"
	rm -rf "$STAGE"
	mkdir -p "$STAGE/src/tds" "$STAGE/src/connection" "$STAGE/src/codec/staging"
	ln -s "$ROOT/src/include" "$STAGE/src/include"
	for d in "$ROOT"/src/tds/*; do ln -s "$d" "$STAGE/src/tds/$(basename "$d")"; done
	ln -s "$ROOT/src/connection/instance_resolver.cpp" "$STAGE/src/connection/instance_resolver.cpp"
	ln -s "$ROOT/src/codec/staging/row_stager.cpp" "$STAGE/src/codec/staging/row_stager.cpp"
	ln -s "$ROOT/src/codec/staging/column_ops.cpp" "$STAGE/src/codec/staging/column_ops.cpp"
	INPUT="$STAGE"
	;;
all)
	INPUT="$ROOT/src"
	;;
*)
	echo "unknown scope '$SCOPE' (expected 'tds' or 'all')" >&2
	exit 2
	;;
esac

echo "==> scope      : $SCOPE"
echo "==> input      : $INPUT"
echo "==> output     : $CPG"
echo "==> includes   : ${#INCLUDES[@]} path(s)"
echo "==> JAVA_OPTS  : $JAVA_OPTS"
echo

# Frontend args go after the --frontend-args separator, verbatim to c2cpg.
# --with-include-auto-discovery lets CDT find the system/SDK headers (without it
# every <cstdint> is unresolved and the integer typedefs that q3 keys on are
# lost).
time joern-parse "$INPUT" \
	--language NEWC \
	--output "$CPG" \
	--frontend-args \
	--with-include-auto-discovery \
	"${INCLUDES[@]}" \
	--define MSSQL_VERSION='"joern"' \
	--define DUCKDB_EXTENSION_MAIN

echo
echo "CPG written: $CPG"
echo "Query it with:  joern/scan.sh $SCOPE     (the query suite)"
echo "                joern/shell.sh $SCOPE    (interactive REPL)"
