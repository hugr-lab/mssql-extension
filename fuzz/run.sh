#!/usr/bin/env bash
# Local deep-discovery campaign with AFL++ (CmpLog/RedQueen + dictionary),
# N parallel cores, ASan + UBSan. Intended to run INSIDE fuzz/Dockerfile (Linux);
# do not run on the macOS host.
#
#   fuzz/run.sh <target> [seconds] [cores]
#     <target>  : browser_response | tds_tokens | utf16   (default: browser_response)
#     seconds   : per-core wall-clock budget (default: 0 = run until Ctrl-C)
#     cores     : parallel instances (default: nproc, capped at 8)
#
# Findings persist under fuzz/findings/<target>/ (queue = corpus, crashes = bugs).
set -euo pipefail

TARGET="${1:-browser_response}"
DURATION="${2:-0}"
CORES="${3:-$(( $(nproc) < 8 ? $(nproc) : 8 ))}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
case "${TARGET}" in
	browser_response|tds_tokens|utf16|envchange_txn) ;;
	*) echo "unknown target '${TARGET}' (browser_response|tds_tokens|utf16|envchange_txn)"; exit 2 ;;
esac
BIN="fuzz_${TARGET}"

# AFL++ driver: compile libFuzzer-style harnesses for afl-fuzz.
DRIVER="$(find / -name 'libAFLDriver.a' 2>/dev/null | head -1)"
[[ -n "${DRIVER}" ]] || { echo "ERROR: libAFLDriver.a not found (need the aflplusplus image)"; exit 1; }

echo "== Building ${BIN} (AFL++ instrumented + CmpLog), ASan+UBSan =="
# (1) Primary instrumented build.
CC=afl-clang-fast CXX=afl-clang-fast++ \
	LIB_FUZZING_ENGINE="${DRIVER}" \
	OUT="${HERE}/out" WORK="${HERE}/work" \
	bash "${HERE}/build.sh"

# (2) CmpLog/RedQueen build (auto-solves magic-byte and length guards).
AFL_LLVM_CMPLOG=1 CC=afl-clang-fast CXX=afl-clang-fast++ \
	LIB_FUZZING_ENGINE="${DRIVER}" \
	OUT="${HERE}/out/cmplog" WORK="${HERE}/work/cmplog" \
	bash "${HERE}/build.sh"

FIND="${HERE}/findings/${TARGET}"
SEED="${HERE}/corpus/${TARGET}"
mkdir -p "${FIND}"
[[ -d "${SEED}" && -n "$(ls -A "${SEED}" 2>/dev/null)" ]] || { mkdir -p "${SEED}"; printf '\x05\x00\x00' > "${SEED}/min"; }

DICT_ARG=()
[[ -f "${HERE}/tds.dict" && "${TARGET}" != "browser_response" ]] && DICT_ARG=(-x "${HERE}/tds.dict")
TMOUT_ARG=()
[[ "${DURATION}" != "0" ]] && TMOUT_ARG=(-V "${DURATION}")

echo "== Launching ${CORES} AFL++ instances on ${TARGET} (CmpLog, dict=${#DICT_ARG[@]} args) =="
common=(-i "${SEED}" -o "${FIND}" -c "${HERE}/out/cmplog/${BIN}" "${DICT_ARG[@]}" "${TMOUT_ARG[@]}")

# Main instance in the foreground; secondaries in the background.
for i in $(seq 2 "${CORES}"); do
	AFL_AUTORESUME=1 afl-fuzz "${common[@]}" -S "fuzzer${i}" -- "${HERE}/out/${BIN}" >/dev/null 2>&1 &
done
AFL_AUTORESUME=1 afl-fuzz "${common[@]}" -M fuzzer1 -- "${HERE}/out/${BIN}"

echo "== Done. Corpus: ${FIND}/fuzzer*/queue   Crashes: ${FIND}/fuzzer*/crashes =="
