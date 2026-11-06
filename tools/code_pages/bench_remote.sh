#!/usr/bin/env bash
# Spec 091: the code-page kernels' unit test and benchmark on whatever machine this runs on,
# from a source tree that holds only what they need (bench_gcp.sh ships one; a checkout of
# the repository works too). No DuckDB, no SQL Server: g++ and simdutf's single header.
#
#   tools/code_pages/bench_remote.sh [source-dir]
#
# Prints the CPU (lscpu's model, the flags the kernels choose by), the unit test's verdict
# for every instruction set the CPU has, and bench_code_page once per instruction set
# (the best one, then each lower one), so one run shows them side by side;
# BENCH_ALIGN_OFFSETS="0 1" also runs the best one with its input 1 byte past a 64-byte boundary.
set -euo pipefail

SRC="${1:-.}"
WORK="$(mktemp -d)"
cd "$SRC"

if ! command -v g++ >/dev/null || ! command -v unzip >/dev/null || ! command -v curl >/dev/null; then
	sudo apt-get update -qq >/dev/null
	sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq g++ unzip curl >/dev/null
fi

echo "== machine"
lscpu | grep -E '^(Architecture|Model name|Vendor ID|CPU\(s\)|Thread|L1d|L2|L3)' || true
if [ -r /proc/cpuinfo ] && grep -q -m1 '^flags' /proc/cpuinfo; then
	echo "flags: $(grep -m1 '^flags' /proc/cpuinfo | grep -o -w -E 'avx2|bmi2|popcnt|avx512f|avx512bw|avx512vbmi|avx512_vbmi2' | sort -u | tr '\n' ' ')"
fi
echo "compiler: $(g++ --version | head -1)"

curl -fsSL https://github.com/simdutf/simdutf/releases/download/v6.1.1/singleheader.zip -o "$WORK/s.zip"
unzip -o -q -j "$WORK/s.zip" simdutf.h simdutf.cpp -d "$WORK/simdutf"

SOURCES="src/codec/code_page/code_page.cpp src/codec/code_page/code_page_avx2.cpp \
	src/codec/code_page/code_page_avx512.cpp src/codec/code_page/code_page_tables.cpp"

echo "== unit test"
# shellcheck disable=SC2086
g++ -std=c++17 -O2 -Wall -Wextra -Werror -I src/include test/cpp/codec/test_code_page.cpp $SOURCES \
	-o "$WORK/test_code_page"
"$WORK/test_code_page" | tee "$WORK/test.txt"
best="$(sed -n 's/^kernels: //p' "$WORK/test.txt" | head -1)"

echo "== bench"
# shellcheck disable=SC2086
g++ -std=c++17 -O3 -DNDEBUG -I src/include -I "$WORK/simdutf" test/cpp/bench_code_page.cpp $SOURCES \
	"$WORK/simdutf/simdutf.cpp" -o "$WORK/bench_code_page"
# The best instruction set at each payload offset from a 64-byte boundary asked for
# (BENCH_ALIGN_OFFSETS, default "0"); the lower ones at the first.
for offset in ${BENCH_ALIGN_OFFSETS:-0}; do
	BENCH_ALIGN_OFFSET="$offset" "$WORK/bench_code_page"
	echo
done
for isa in avx2 scalar; do
	# A lower instruction set the CPU has; the bench refuses one it lacks (exit 2, no output).
	if [ "$isa" != "$best" ] && out="$(CODE_PAGE_ISA="$isa" "$WORK/bench_code_page" 2>/dev/null)"; then
		echo
		echo "$out"
	fi
done
rm -rf "$WORK"
