#!/usr/bin/env bash
# Build and run the RowStager differential framing test (spec 055 T5).
#
# ONE source list, called from both `make test-row-stager-framing` and the C++
# unit-test job in .github/workflows/ci.yml. Keeping it in one place is the same
# argument the test itself makes: two independently-maintained copies of the
# same list drift, and here the drift would be a new src/codec/staging/*.cpp
# that is compiled in one place and not the other.
#
# Usage: run_row_stager_framing.sh <vcpkg-prefix> [build-dir]
#
#   vcpkg-prefix  directory holding include/simdutf.h and lib/libsimdutf.*
#                 (e.g. build/release/vcpkg_installed/x64-linux). Passed in
#                 rather than derived: the Makefile and CI each already know
#                 their own triplet, and deriving it here with `ls | head -1`
#                 picks the wrong directory on Linux, where `vcpkg` sorts
#                 before `x64-linux`.
#   build-dir     tree holding src/libduckdb.* (default: build/release)

set -euo pipefail

if [ $# -lt 1 ]; then
	echo "usage: $0 <vcpkg-prefix> [build-dir]" >&2
	exit 2
fi

PREFIX="$1"
BUILD_DIR="${2:-build/release}"
CXX="${CXX:-c++}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

cd "$REPO_ROOT"

if [ ! -f "$PREFIX/include/simdutf.h" ]; then
	echo "ERROR: simdutf.h not found under $PREFIX — run 'make release' first." >&2
	exit 1
fi
if ! ls "$BUILD_DIR"/src/libduckdb* >/dev/null 2>&1; then
	echo "ERROR: no libduckdb in $BUILD_DIR/src — run 'make release' first." >&2
	exit 1
fi

mkdir -p build/test

# Sanitizers are the point, not a precaution. The staged row walk carries no
# per-value bounds checks (RowReader::SkipRow already proved the row is
# buffered), so a framing disagreement between the two walks is a heap
# over-read rather than a wrong answer — and only ASan sees that half.
# -fno-sanitize-recover makes the first report fatal.
SOURCES=(
	test/cpp/codec/test_row_stager_framing.cpp
	src/codec/staging/column_staging.cpp
	src/codec/staging/column_ops.cpp
	src/codec/staging/row_stager.cpp
	src/tds/tds_row_reader.cpp
	src/tds/tds_column_metadata.cpp
	src/tds/tds_types.cpp
	src/tds/encoding/type_converter.cpp
	src/tds/encoding/utf16.cpp
	src/tds/encoding/datetime_encoding.cpp
	src/tds/encoding/decimal_encoding.cpp
	src/tds/encoding/guid_encoding.cpp
	src/tds/encoding/bcp_row_encoder.cpp
)
# The per-family codec sources (non-recursive: staging/ is listed explicitly
# above). row_stager.cpp calls every family's DecodeChunkFromStaging.
for f in src/codec/*.cpp; do
	SOURCES+=("$f")
done

echo "Building RowStager framing tests (spec 055 T5, ASan+UBSan)..."
"$CXX" -std=c++17 -g -O1 -pthread -Wno-deprecated-declarations \
	-fsanitize=address,undefined -fno-sanitize-recover=all \
	-I src/include -I duckdb/src/include -I "$PREFIX/include" \
	"${SOURCES[@]}" \
	-L "$PREFIX/lib" -lsimdutf \
	-L "$BUILD_DIR/src" -lduckdb \
	-o build/test/test_row_stager_framing

# detect_leaks=0: libduckdb is not instrumented and its static initialisers hold
# allocations for process lifetime, which LeakSanitizer (on by default on Linux)
# would report as leaks unrelated to this test. Buffer overflows — the thing
# being tested for — are unaffected.
echo
ASAN_OPTIONS=detect_leaks=0 \
	DYLD_LIBRARY_PATH="$BUILD_DIR/src" LD_LIBRARY_PATH="$BUILD_DIR/src" \
	build/test/test_row_stager_framing
