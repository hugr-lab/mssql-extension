#!/usr/bin/env bash
# LeakSanitizer over the DuckDB-FREE fuzz harnesses' stored corpora.
#
# WHY THIS SET, AND WHY IT NEEDS NO SUPPRESSIONS
#
# LeakSanitizer ships inside ASan and is on by default on Linux, so this repo
# has had it available in every sanitized lane all along -- and switched off in
# the one lane that would run it. test/cpp/codec/run_staging_test.sh says why:
#
#   # detect_leaks=0: libduckdb is not instrumented and its static initialisers
#   # hold allocations for process lifetime, which LeakSanitizer (on by default
#   # on Linux) would report as leaks unrelated to this test.
#
# That is the real blocker everywhere libduckdb is linked, and the answer there
# is a reviewed suppression file, which this repo does not have yet.
#
# But five of the seven harnesses link NO libduckdb at all -- they compile only
# src/tds/ plus simdutf, which is the isolation finding fuzz/README.md
# documents (`grep -L duckdb/` over the parsing layer). On those, LSan is clean
# BY CONSTRUCTION: there is no uninstrumented library to misattribute anything
# to. So this lane costs no suppressions, and it covers exactly the code that
# allocates per token, per row and per login response on lengths a hostile
# server chose.
#
# WHY IT IS NOT REDUNDANT WITH ClusterFuzzLite
#
# libFuzzer does its own leak check PER INPUT, and ClusterFuzzLite runs these
# same five targets under the address sanitizer, so some of this may already be
# happening there -- the run logs neither confirm nor deny it (they print no
# ASAN_OPTIONS, and finding no leak looks identical either way). Two reasons to
# have this regardless:
#
#   1. It is CUMULATIVE. A corpus replay checks once, at exit, over thousands of
#      inputs in one process -- which catches the slow drip that a per-input
#      check resets away. Different question, different answer.
#   2. It is VISIBLE. Whether leak detection runs stops depending on a third
#      party's defaults and becomes something this repo states and can be read
#      off a job name.
#
# PLATFORM
#
# Linux only, and that is not a preference: Apple's toolchain ships no LSan.
# Measured on this tree's dev box --
#   ASAN_OPTIONS=detect_leaks=1 ./probe
#   ==27706==AddressSanitizer: detect_leaks is not supported on this platform.
# -- so on macOS this script SKIPS with that reason rather than passing
# vacuously, which would be the worse failure.
#
# Run it the way CI does, or locally through the fuzz container:
#   scripts/ci/leak_check.sh
#   docker run --rm -v "$PWD:/repo" -w /repo mssql-fuzz scripts/ci/leak_check.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$HERE"

# The DuckDB-free five. Deliberately NOT row_stager or bcp_encode: both link
# libduckdb and need the suppression file that does not exist yet.
TARGETS=(browser_response tds_tokens utf16 envchange_txn login_response)

if [ "$(uname -s)" != "Linux" ]; then
	echo "SKIP: LeakSanitizer is not supported on $(uname -s) — Apple's toolchain ships no LSan."
	echo "      Run this on Linux, or through the fuzz container:"
	echo "        docker build -t mssql-fuzz -f fuzz/Dockerfile ."
	echo "        docker run --rm -v \"\$PWD:/repo\" -w /repo mssql-fuzz scripts/ci/leak_check.sh"
	exit 0
fi

# Dedicated out/work dirs, for two concrete reasons:
#   * they must not clobber a developer's fuzz/out, which holds the binaries
#     `fuzz/run.sh` and `make fuzz-encode-replay` just built;
#   * fuzz/work caches a COMPILED simdutf.o and build.sh reuses it when present.
#     Running this in a Linux container over a bind-mounted macOS checkout would
#     otherwise try to link a Darwin arm64 object. Separate WORK, separate cache.
LEAK_OUT="${LEAK_OUT:-fuzz/out/leak}"
LEAK_WORK="${LEAK_WORK:-fuzz/work/leak}"

echo "== Building the DuckDB-free harnesses as replay executables (ASan + UBSan + LSan) =="
# REPLAY=1 links fuzz/replay_main.cc instead of a fuzzing engine, so this needs
# no libFuzzer runtime and the corpus is replayed in ONE process -- which is
# what makes the leak check cumulative.
REPLAY=1 TARGETS="${TARGETS[*]}" OUT="$LEAK_OUT" WORK="$LEAK_WORK" bash fuzz/build.sh

echo
echo "== Replaying each corpus with detect_leaks=1 =="
failed=()
checked=0
for t in "${TARGETS[@]}"; do
	bin="${LEAK_OUT}/fuzz_${t}"
	corpus="fuzz/corpus/${t}"
	if [ ! -x "$bin" ]; then
		echo "  ERROR: $bin was not built"
		failed+=("$t (not built)")
		continue
	fi
	if [ ! -d "$corpus" ] || [ -z "$(ls -A "$corpus" 2>/dev/null)" ]; then
		echo "  SKIP  fuzz_${t}: no stored corpus at ${corpus}/"
		continue
	fi
	n=$(find "$corpus" -maxdepth 1 -type f | wc -l | tr -d ' ')
	printf '  %-24s %4s input(s) ... ' "fuzz_${t}" "$n"
	# detect_leaks=1 is the point. The harnesses swallow std::exception by
	# design (malformed input is expected), so a non-zero exit here is a
	# sanitizer verdict -- a leak, an overflow or a UB report -- never a
	# rejected input.
	if ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 \
		UBSAN_OPTIONS=print_stacktrace=1 \
		"./$bin" "$corpus" >"${LEAK_WORK}/leak_${t}.log" 2>&1; then
		echo "clean"
	else
		echo "FAILED"
		failed+=("$t")
		echo "    ---- last 40 lines of ${LEAK_WORK}/leak_${t}.log ----"
		tail -40 "${LEAK_WORK}/leak_${t}.log" | sed 's/^/    /'
		echo "    ---------------------------------------------"
	fi
	checked=$((checked + 1))
done

echo
if [ ${#failed[@]} -gt 0 ]; then
	echo "FAIL: ${#failed[@]} harness(es) reported a sanitizer finding: ${failed[*]}"
	echo "      A LeakSanitizer report names the allocation stack. These harnesses"
	echo "      link no libduckdb, so there is no uninstrumented library to blame:"
	echo "      a leak here is in src/tds/ or in the harness itself."
	exit 1
fi
echo "PASS: ${checked} harness corpora replayed with no leak and no sanitizer report."
