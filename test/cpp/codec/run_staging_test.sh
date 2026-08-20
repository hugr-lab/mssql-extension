#!/usr/bin/env bash
# Build and run one of the staged-read-path C++ tests (specs 055 / 059).
#
# ONE source list, shared by every caller: `make test-row-stager`,
# `make test-row-stager-framing`, and the C++ unit-test job in
# .github/workflows/ci.yml. Keeping it in one place is the same argument the
# framing test itself makes: two independently-maintained copies of the same
# list drift, and here the drift would be a new src/codec/staging/*.cpp that is
# compiled in one place and not the other.
#
# The list is identical for every test here because they all drive the whole
# staged walk — the family codecs' finalize kernels, the encoding helpers those
# call, and the legacy row reader. Only the test file differs.
#
# Usage: run_staging_test.sh <vcpkg-prefix> <test-source> [build-dir]
#
#   vcpkg-prefix  directory holding include/simdutf.h and lib/libsimdutf.*
#                 (e.g. build/release/vcpkg_installed/x64-linux). Passed in
#                 rather than derived: the Makefile and CI each already know
#                 their own triplet, and deriving it here with `ls | head -1`
#                 picks the wrong directory on Linux, where `vcpkg` sorts
#                 before `x64-linux`.
#   test-source   the .cpp holding main() (e.g. test/cpp/codec/test_row_stager.cpp)
#   build-dir     tree holding src/libduckdb.* (default: build/release)
#
# Environment:
#   SANITIZE=0    build without ASan/UBSan (default 1 — see below)
#   CXX           compiler (default: c++)

set -euo pipefail

if [ $# -lt 2 ]; then
	echo "usage: $0 <vcpkg-prefix> <test-source> [build-dir]" >&2
	exit 2
fi

PREFIX="$1"
TEST_SRC="$2"
BUILD_DIR="${3:-build/release}"
CXX="${CXX:-c++}"
SANITIZE="${SANITIZE:-1}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

cd "$REPO_ROOT"

if [ ! -f "$TEST_SRC" ]; then
	echo "ERROR: test source $TEST_SRC not found." >&2
	exit 1
fi
if [ ! -f "$PREFIX/include/simdutf.h" ]; then
	echo "ERROR: simdutf.h not found under $PREFIX — run 'make release' first." >&2
	exit 1
fi
if ! ls "$BUILD_DIR"/src/libduckdb* >/dev/null 2>&1; then
	echo "ERROR: no libduckdb in $BUILD_DIR/src — run 'make release' first." >&2
	exit 1
fi

BIN="build/test/$(basename "$TEST_SRC" .cpp)"
mkdir -p build/test

SOURCES=(
	"$TEST_SRC"
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

# Sanitizers are the point, not a precaution. The staged row walk carries no
# per-value bounds checks (RowReader::SkipRow already proved the row is
# buffered), so a framing disagreement between the two walks is a heap
# over-read rather than a wrong answer — and only ASan sees that half.
# -fno-sanitize-recover makes the first report fatal.
#
# SANITIZE=0 exists for one measured reason: ASan hangs at process init on
# Darwin 25.5, so the sanitized binary cannot be run on a current macOS dev box
# at all — `make test-row-stager*` passes it there and says so. CI keeps both
# legs sanitized; its macOS runner is older and runs them green.
SAN_FLAGS=()
LABEL="assertions only, SANITIZE=0"
if [ "$SANITIZE" != "0" ]; then
	SAN_FLAGS=(-fsanitize=address,undefined -fno-sanitize-recover=all)
	LABEL="ASan+UBSan"
fi

# No -DNDEBUG, on purpose: both walks end with `D_ASSERT(p == end)`, which is
# itself the framing check — it fires when a walk consumes a different number of
# bytes than the row holds.
#
# ${arr[@]+"${arr[@]}"} rather than "${arr[@]}": under `set -u`, bash 3.2 —
# which is what /bin/bash still is on macOS — treats an EMPTY array expansion as
# an unbound variable and aborts.
echo "Building $(basename "$TEST_SRC") ($LABEL)..."
"$CXX" -std=c++17 -g -O1 -pthread -Wno-deprecated-declarations \
	${SAN_FLAGS[@]+"${SAN_FLAGS[@]}"} \
	-I src/include -I duckdb/src/include -I duckdb/third_party/fmt/include -I "$PREFIX/include" \
	"${SOURCES[@]}" \
	-L "$PREFIX/lib" -lsimdutf \
	-L "$BUILD_DIR/src" -lduckdb \
	-o "$BIN"

# detect_leaks=0: libduckdb is not instrumented and its static initialisers hold
# allocations for process lifetime, which LeakSanitizer (on by default on Linux)
# would report as leaks unrelated to this test. Buffer overflows — the thing
# being tested for — are unaffected.
echo
ASAN_OPTIONS=detect_leaks=0 \
	DYLD_LIBRARY_PATH="$BUILD_DIR/src" LD_LIBRARY_PATH="$BUILD_DIR/src" \
	"$BIN"
