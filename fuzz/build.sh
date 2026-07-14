#!/usr/bin/env bash
# Build the mssql-extension fuzz harnesses.
#
# Works in three environments via the OSS-Fuzz / ClusterFuzzLite contract — it
# honours $CC $CXX $CFLAGS $CXXFLAGS $LIB_FUZZING_ENGINE $OUT $SRC when set, and
# falls back to a self-contained clang + libFuzzer + ASan + UBSan build otherwise:
#
#   1. ClusterFuzzLite / OSS-Fuzz : the base-builder image sets all of the above.
#   2. Local libFuzzer (Dockerfile / run.sh --libfuzzer) : we set them ourselves.
#   3. Local AFL++ (run.sh)       : CC/CXX = afl-clang-fast(++),
#                                    LIB_FUZZING_ENGINE = AFL++ driver.
#
# Only the TDS-parsing layer is compiled — it has ZERO DuckDB dependencies (see
# fuzz/README.md), so harnesses link a minimal set of .cpp files plus simdutf.
set -euo pipefail

# --- locate the repo root (dir containing src/) ------------------------------
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="${SRC:-}"
if [[ -n "${REPO}" && -d "${REPO}/mssql-extension/src" ]]; then
	REPO="${REPO}/mssql-extension"           # ClusterFuzzLite checks out under $SRC/<repo>
elif [[ -d "${HERE}/../src" ]]; then
	REPO="$(cd "${HERE}/.." && pwd)"          # running from a normal checkout
fi
[[ -d "${REPO}/src" ]] || { echo "ERROR: cannot find repo src/ (REPO=${REPO})"; exit 1; }

OUT="${OUT:-${HERE}/out}"
WORK="${WORK:-${HERE}/work}"
mkdir -p "${OUT}" "${WORK}"

CXX="${CXX:-clang++}"
CC="${CC:-clang}"

# Default sanitizer/instrumentation when not driven by OSS-Fuzz/ClusterFuzzLite.
# ASan + UBSan are mandatory (the integer over/underflow shapes need UBSan), and
# we never let UBSan recover so a violation aborts the run.
DEFAULT_FLAGS="-g -O1 -fno-omit-frame-pointer -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all"
CXXFLAGS="${CXXFLAGS:-${DEFAULT_FLAGS}} -std=c++17"
# LIB_FUZZING_ENGINE: OSS-Fuzz sets this; standalone we link libFuzzer directly.
FUZZ_ENGINE="${LIB_FUZZING_ENGINE:--fsanitize=fuzzer}"

INC="-I${REPO}/src/include"

echo ">> repo:   ${REPO}"
echo ">> out:    ${OUT}"
echo ">> CXX:    ${CXX}"
echo ">> flags:  ${CXXFLAGS}"
echo ">> engine: ${FUZZ_ENGINE}"

# --- simdutf (only external dep; used by the TDS/utf16 decoders) -------------
# Fetched lazily as a pinned single-header amalgamation and compiled with the
# same sanitizer flags. The browser_response PoC does NOT need it, so a clean
# offline checkout can still build/run that harness.
SIMDUTF_VER="${SIMDUTF_VER:-5.6.0}"
SIMDUTF_DIR="${WORK}/simdutf-${SIMDUTF_VER}"
SIMDUTF_OBJ="${WORK}/simdutf.o"
SIMDUTF_INC=""
ensure_simdutf() {
	[[ -n "${SIMDUTF_INC}" ]] && return 0
	if [[ ! -f "${SIMDUTF_DIR}/simdutf.cpp" ]]; then
		mkdir -p "${SIMDUTF_DIR}"
		echo ">> fetching simdutf v${SIMDUTF_VER} singleheader"
		curl -fsSL "https://github.com/simdutf/simdutf/releases/download/v${SIMDUTF_VER}/singleheader.zip" \
			-o "${WORK}/simdutf.zip"
		unzip -o -j "${WORK}/simdutf.zip" 'simdutf.h' 'simdutf.cpp' -d "${SIMDUTF_DIR}" >/dev/null
	fi
	if [[ ! -f "${SIMDUTF_OBJ}" ]]; then
		echo ">> compiling simdutf"
		"${CXX}" ${CXXFLAGS} -I"${SIMDUTF_DIR}" -c "${SIMDUTF_DIR}/simdutf.cpp" -o "${SIMDUTF_OBJ}"
	fi
	SIMDUTF_INC="-I${SIMDUTF_DIR}"
}

# --- minimal source sets per harness -----------------------------------------
TDS="${REPO}/src/tds"
TDS_DECODE_SRCS=(
	"${TDS}/tds_token_parser.cpp"
	"${TDS}/tds_row_reader.cpp"
	"${TDS}/tds_column_metadata.cpp"
	"${TDS}/tds_types.cpp"
	"${TDS}/encoding/utf16.cpp"
)

compile_objs() {  # $1=tag, rest=sources ; echoes object paths
	local tag="$1"; shift
	local objs=()
	for s in "$@"; do
		local o="${WORK}/${tag}_$(basename "${s%.cpp}").o"
		"${CXX}" ${CXXFLAGS} ${INC} ${SIMDUTF_INC} -c "${s}" -o "${o}"
		objs+=("${o}")
	done
	echo "${objs[@]}"
}

build_harness() {  # $1=name $2=harness.cc ; remaining = library objects
	local name="$1"; local hcc="$2"; shift 2
	local simd=()
	[[ -n "${SIMDUTF_INC}" ]] && simd=("${SIMDUTF_OBJ}")
	echo ">> linking ${name}"
	"${CXX}" ${CXXFLAGS} ${INC} ${SIMDUTF_INC} \
		"${HERE}/${hcc}" "$@" "${simd[@]}" ${FUZZ_ENGINE} -o "${OUT}/${name}"
}

# TARGETS selects which harnesses to build (default: all). Lets CI / run.sh /
# verification build a subset; e.g. TARGETS=browser_response builds offline.
TARGETS="${TARGETS:-browser_response tds_tokens utf16 envchange_txn login_response}"
want() { [[ " ${TARGETS} " == *" $1 "* ]]; }

# 1. SQL Browser response parser — standalone: no simdutf, no TDS sources, no
#    network needed to build (the offline-friendly PoC).
if want browser_response; then
	BR_OBJ="$(compile_objs br "${REPO}/src/connection/instance_resolver.cpp")"
	build_harness fuzz_browser_response fuzz_browser_response.cc ${BR_OBJ}
fi

# 2 & 3 decode server strings via simdutf-backed Utf16LEDecode.
if want tds_tokens || want utf16 || want envchange_txn; then
	ensure_simdutf
	TDS_OBJS="$(compile_objs tds "${TDS_DECODE_SRCS[@]}")"
	# 2. TDS token-stream decoder (COLMETADATA / ROW / type+length).
	want tds_tokens && build_harness fuzz_tds_tokens fuzz_tds_tokens.cc ${TDS_OBJS}
	# 3. UTF-16LE decoders (reuses the utf16.o compiled above).
	want utf16 && build_harness fuzz_utf16 fuzz_utf16.cc "${WORK}/tds_utf16.o"
	# 4. ENVCHANGE transaction-descriptor scan (FindBeginTxnDescriptor).
	want envchange_txn && build_harness fuzz_envchange_txn fuzz_envchange_txn.cc ${TDS_OBJS}
fi

# 5. LOGIN7 response parser (ParseLoginResponse) — pre-auth token stream; distinct
#    from tds_tokens' post-auth TokenParser. Needs tds_protocol/tds_packet, which
#    the decode set above does not compile, so build its own object set.
if want login_response; then
	ensure_simdutf
	LOGIN_OBJS="$(compile_objs login \
		"${TDS}/tds_protocol.cpp" "${TDS}/tds_packet.cpp" "${TDS}/tds_types.cpp" "${TDS}/encoding/utf16.cpp")"
	build_harness fuzz_login_response fuzz_login_response.cc ${LOGIN_OBJS}
fi

# --- seed corpora + dictionary (OSS-Fuzz packages these next to the binary) ---
for t in browser_response tds_tokens utf16 login_response; do
	if [[ -d "${HERE}/corpus/${t}" ]]; then
		( cd "${HERE}/corpus/${t}" && zip -qr "${OUT}/fuzz_${t}_seed_corpus.zip" . ) || true
	fi
done
cp -f "${HERE}/tds.dict" "${OUT}/fuzz_tds_tokens.dict" 2>/dev/null || true
cp -f "${HERE}/tds.dict" "${OUT}/fuzz_utf16.dict" 2>/dev/null || true

echo ">> done. fuzzers in ${OUT}:"
ls -1 "${OUT}" | sed 's/^/   /'
