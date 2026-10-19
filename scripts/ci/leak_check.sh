#!/usr/bin/env bash
# LeakSanitizer over the DuckDB-FREE fuzz harnesses: their stored seed corpora
# plus a deterministic generated corpus, replayed cumulatively in one process.
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
#   1. It is CUMULATIVE. One process, ~4000 inputs per harness, and the leak
#      verdict taken once at exit -- which catches the slow drip that a
#      per-input check resets away. Different question, different answer.
#      (The stored seeds alone are only ~30 inputs in total, which is why
#      this lane generates its own corpus; see GEN_PER_TARGET below.)
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

# How many generated inputs to replay per harness, on top of the stored corpus.
#
# WHY GENERATE AT ALL: the whole argument for this lane over ClusterFuzzLite's
# per-input leak check is that it is CUMULATIVE -- one process, many inputs, so a
# slow drip shows up where a per-input check resets it away. But the repo tracks
# only SEED corpora: 4 to 9 files per harness, 30 in total. At thirty inputs
# "cumulative" is not a meaningful claim, it is a smoke test. ClusterFuzzLite's
# own accumulated corpus for fuzz_tds_tokens alone is ~15k elements, but that
# lives in CFLite storage and is not retrievable here without reimplementing its
# corpus plumbing.
#
# So the lane generates its own. These are byte-stream parsers whose entire
# contract is to survive arbitrary bytes from a hostile server, so pseudo-random
# input is not a toy here -- it is the threat model. A FIXED SEED keeps a failure
# reproducible, which is the one property a generated corpus must have to be
# worth anything in CI.
GEN_PER_TARGET="${GEN_PER_TARGET:-4000}"
GEN_SEED="${GEN_SEED:-20261019}"

echo
echo "== Generating ${GEN_PER_TARGET} deterministic inputs per harness (seed ${GEN_SEED}) =="
python3 - "$LEAK_WORK" "$GEN_PER_TARGET" "$GEN_SEED" "${TARGETS[@]}" <<'PY'
import os
import random
import sys

work, n, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
targets = sys.argv[4:]

for t in targets:
    rnd = random.Random(f"{seed}:{t}")
    outdir = os.path.join(work, "gen", t)
    os.makedirs(outdir, exist_ok=True)
    # Start from the stored seeds so generated inputs include MUTATIONS of
    # structurally valid ones, not only noise. A token stream that parses far
    # enough to allocate is where a leak would live; pure noise mostly dies at
    # the first length check.
    seeds = []
    src = os.path.join("fuzz", "corpus", t)
    if os.path.isdir(src):
        for fn in sorted(os.listdir(src)):
            p = os.path.join(src, fn)
            if os.path.isfile(p):
                with open(p, "rb") as fh:
                    seeds.append(bytearray(fh.read()))
    for i in range(n):
        if seeds and rnd.random() < 0.7:
            b = bytearray(rnd.choice(seeds))
            for _ in range(rnd.randint(1, 8)):
                if not b:
                    break
                op = rnd.randrange(4)
                if op == 0:
                    b[rnd.randrange(len(b))] = rnd.randrange(256)
                elif op == 1 and len(b) > 1:
                    del b[rnd.randrange(len(b))]
                elif op == 2:
                    b.insert(rnd.randrange(len(b) + 1), rnd.randrange(256))
                else:
                    # Declared lengths are where the bugs have been (#183 ERROR
                    # token MsgText, #185 FEDAUTHINFO overflow), so bias toward
                    # writing 0xFF runs over them.
                    at = rnd.randrange(len(b))
                    for k in range(min(4, len(b) - at)):
                        b[at + k] = 0xFF
        else:
            b = bytearray(rnd.randrange(256) for _ in range(rnd.randrange(1, 4096)))
        with open(os.path.join(outdir, f"g{i:05d}"), "wb") as fh:
            fh.write(bytes(b))
    print(f"  {t}: {n} generated")
PY

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
	gen="${LEAK_WORK}/gen/${t}"
	stored=$(find "$corpus" -maxdepth 1 -type f | wc -l | tr -d ' ')
	generated=0
	gen_arg=()
	if [ -d "$gen" ]; then
		generated=$(find "$gen" -maxdepth 1 -type f | wc -l | tr -d ' ')
		# Guard on the COUNT, not on the string: `${generated:+...}` expands for
		# the string "0" too, which would hand the replay an empty directory and
		# make it exit 2 ("nothing to replay") on a clean run.
		[ "$generated" -gt 0 ] && gen_arg=("$gen")
	fi
	printf '  %-24s %5s stored + %5s generated ... ' "fuzz_${t}" "$stored" "$generated"
	# BOTH sets in ONE process, which is what makes the check cumulative: the
	# leak verdict is taken once, at exit, after every input has run.
	#
	# detect_leaks=1 is the point. The harnesses swallow std::exception by
	# design (malformed input is expected), so a non-zero exit here is a
	# sanitizer verdict -- a leak, an overflow or a UB report -- never a
	# rejected input.
	if ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 \
		UBSAN_OPTIONS=print_stacktrace=1 \
		"./$bin" "$corpus" ${gen_arg[@]+"${gen_arg[@]}"} >"${LEAK_WORK}/leak_${t}.log" 2>&1; then
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
