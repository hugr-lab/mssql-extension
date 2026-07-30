//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// utf16.cpp
//
// UTF-8 <-> UTF-16LE conversion primitives, simdutf-backed.
//
// Spec 044 (Codec Layer Consolidation) folded what spec 043 introduced as
// src/tds/encoding/simdutf_wrappers.{hpp,cpp} back into the original
// utf16.{hpp,cpp} file path. The public function names are unchanged from
// the pre-spec-043 legacy converter; only the implementation behind them
// is now simdutf-backed. The hand-rolled implementation that used to be
// public survives here as private LegacyUtf16LE* helpers in an anonymous
// namespace, invoked only by the public functions' invalid-input fallback
// path (spec 043 Clarification Q1).
//
// Invalid-input contract:
//   1. Pre-validate via simdutf::validate_utf8 (encode direction) or
//      validate_utf16le (decode direction).
//   2. Valid input -> SIMD fast path via convert_valid_*.
//   3. Invalid input:
//      - DECODE (UTF-16LE -> UTF-8): standard U+FFFD substitution, one
//        replacement per ill-formed code unit, decoding resumes at the next
//        unit (DecodeUtf16LeReplacing). Spec 055 D2 replaced the hand-rolled
//        converter here: it consumed the unit *following* an unpaired high
//        surrogate before emitting its single replacement, and dropped a
//        trailing high surrogate entirely — silent data loss, not replacement.
//      - ENCODE (UTF-8 -> UTF-16LE): private LegacyUtf16LE* fallback, still
//        preserving the pre-spec-043 "skip invalid bytes, continue" semantics
//        bit-for-bit.
//   4. Never throws on invalid input.
//===----------------------------------------------------------------------===//

#include "tds/encoding/utf16.hpp"

#include <simdutf.h>

#include <cstring>

namespace duckdb {
namespace tds {
namespace encoding {

//===----------------------------------------------------------------------===//
// Private legacy hand-rolled converter (anonymous namespace).
// Used only as the invalid-input fallback by the public simdutf-backed
// functions below. Bit-identical to the pre-spec-043 implementation that
// lived at this same file path.
//===----------------------------------------------------------------------===//

namespace {

inline bool IsAsciiString(const char *data, size_t len) {
	// `data` may come from a DuckDB string_t inline buffer that is only
	// 4-byte aligned; a raw uint64_t cast is undefined behaviour. memcpy
	// expresses the same SWAR fast path with defined alignment semantics
	// — modern compilers fold it down to the identical load instruction.
	const size_t chunks = len / 8;
	for (size_t c = 0; c < chunks; c++) {
		uint64_t chunk;
		std::memcpy(&chunk, data + c * 8, 8);
		if (chunk & 0x8080808080808080ULL) {
			return false;
		}
	}
	for (size_t i = chunks * 8; i < len; i++) {
		if (static_cast<uint8_t>(data[i]) & 0x80) {
			return false;
		}
	}
	return true;
}

inline void AsciiToUtf16LE(const char *input, size_t len, std::vector<uint8_t> &result) {
	result.resize(len * 2);
	uint8_t *out = result.data();
	size_t i = 0;
	for (; i + 4 <= len; i += 4) {
		out[i * 2 + 0] = static_cast<uint8_t>(input[i + 0]);
		out[i * 2 + 1] = 0;
		out[i * 2 + 2] = static_cast<uint8_t>(input[i + 1]);
		out[i * 2 + 3] = 0;
		out[i * 2 + 4] = static_cast<uint8_t>(input[i + 2]);
		out[i * 2 + 5] = 0;
		out[i * 2 + 6] = static_cast<uint8_t>(input[i + 3]);
		out[i * 2 + 7] = 0;
	}
	for (; i < len; i++) {
		out[i * 2 + 0] = static_cast<uint8_t>(input[i]);
		out[i * 2 + 1] = 0;
	}
}

void Utf8ToUtf16LEGeneral(const char *input, size_t input_len, std::vector<uint8_t> &result) {
	result.reserve(input_len * 2);
	size_t i = 0;
	while (i < input_len) {
		uint32_t codepoint = 0;
		uint8_t byte = static_cast<uint8_t>(input[i]);
		if ((byte & 0x80) == 0) {
			codepoint = byte;
			i += 1;
		} else if ((byte & 0xE0) == 0xC0) {
			if (i + 1 >= input_len)
				break;
			codepoint = ((byte & 0x1F) << 6) | (static_cast<uint8_t>(input[i + 1]) & 0x3F);
			i += 2;
		} else if ((byte & 0xF0) == 0xE0) {
			if (i + 2 >= input_len)
				break;
			codepoint = ((byte & 0x0F) << 12) | ((static_cast<uint8_t>(input[i + 1]) & 0x3F) << 6) |
						(static_cast<uint8_t>(input[i + 2]) & 0x3F);
			i += 3;
		} else if ((byte & 0xF8) == 0xF0) {
			if (i + 3 >= input_len)
				break;
			codepoint = ((byte & 0x07) << 18) | ((static_cast<uint8_t>(input[i + 1]) & 0x3F) << 12) |
						((static_cast<uint8_t>(input[i + 2]) & 0x3F) << 6) |
						(static_cast<uint8_t>(input[i + 3]) & 0x3F);
			i += 4;
		} else {
			i += 1;
			continue;
		}
		if (codepoint <= 0xFFFF) {
			result.push_back(static_cast<uint8_t>(codepoint & 0xFF));
			result.push_back(static_cast<uint8_t>((codepoint >> 8) & 0xFF));
		} else if (codepoint <= 0x10FFFF) {
			codepoint -= 0x10000;
			uint16_t high_surrogate = 0xD800 + ((codepoint >> 10) & 0x3FF);
			uint16_t low_surrogate = 0xDC00 + (codepoint & 0x3FF);
			result.push_back(static_cast<uint8_t>(high_surrogate & 0xFF));
			result.push_back(static_cast<uint8_t>((high_surrogate >> 8) & 0xFF));
			result.push_back(static_cast<uint8_t>(low_surrogate & 0xFF));
			result.push_back(static_cast<uint8_t>((low_surrogate >> 8) & 0xFF));
		}
	}
}

std::vector<uint8_t> LegacyUtf16LEEncode(const std::string &input) {
	std::vector<uint8_t> result;
	if (input.empty()) {
		return result;
	}
	if (IsAsciiString(input.data(), input.size())) {
		AsciiToUtf16LE(input.data(), input.size(), result);
		return result;
	}
	Utf8ToUtf16LEGeneral(input.data(), input.size(), result);
	return result;
}

size_t LegacyUtf16LEByteLength(const std::string &input) {
	size_t byte_count = 0;
	size_t i = 0;
	while (i < input.size()) {
		uint8_t byte = static_cast<uint8_t>(input[i]);
		uint32_t codepoint = 0;
		if ((byte & 0x80) == 0) {
			codepoint = byte;
			i += 1;
		} else if ((byte & 0xE0) == 0xC0) {
			if (i + 1 >= input.size())
				break;
			codepoint = ((byte & 0x1F) << 6) | (static_cast<uint8_t>(input[i + 1]) & 0x3F);
			i += 2;
		} else if ((byte & 0xF0) == 0xE0) {
			if (i + 2 >= input.size())
				break;
			codepoint = ((byte & 0x0F) << 12) | ((static_cast<uint8_t>(input[i + 1]) & 0x3F) << 6) |
						(static_cast<uint8_t>(input[i + 2]) & 0x3F);
			i += 3;
		} else if ((byte & 0xF8) == 0xF0) {
			if (i + 3 >= input.size())
				break;
			codepoint = ((byte & 0x07) << 18) | ((static_cast<uint8_t>(input[i + 1]) & 0x3F) << 12) |
						((static_cast<uint8_t>(input[i + 2]) & 0x3F) << 6) |
						(static_cast<uint8_t>(input[i + 3]) & 0x3F);
			i += 4;
		} else {
			i += 1;
			continue;
		}
		if (codepoint <= 0xFFFF) {
			byte_count += 2;
		} else if (codepoint <= 0x10FFFF) {
			byte_count += 4;
		}
	}
	return byte_count;
}

size_t LegacyUtf16LEEncodeDirect(const char *input, size_t input_len, uint8_t *output) {
	if (input_len == 0) {
		return 0;
	}
	if (IsAsciiString(input, input_len)) {
		size_t i = 0;
		for (; i + 4 <= input_len; i += 4) {
			output[i * 2 + 0] = static_cast<uint8_t>(input[i + 0]);
			output[i * 2 + 1] = 0;
			output[i * 2 + 2] = static_cast<uint8_t>(input[i + 1]);
			output[i * 2 + 3] = 0;
			output[i * 2 + 4] = static_cast<uint8_t>(input[i + 2]);
			output[i * 2 + 5] = 0;
			output[i * 2 + 6] = static_cast<uint8_t>(input[i + 3]);
			output[i * 2 + 7] = 0;
		}
		for (; i < input_len; i++) {
			output[i * 2 + 0] = static_cast<uint8_t>(input[i]);
			output[i * 2 + 1] = 0;
		}
		return input_len * 2;
	}
	size_t out_pos = 0;
	size_t i = 0;
	while (i < input_len) {
		uint32_t codepoint = 0;
		uint8_t byte = static_cast<uint8_t>(input[i]);
		if ((byte & 0x80) == 0) {
			codepoint = byte;
			i += 1;
		} else if ((byte & 0xE0) == 0xC0) {
			if (i + 1 >= input_len)
				break;
			codepoint = ((byte & 0x1F) << 6) | (static_cast<uint8_t>(input[i + 1]) & 0x3F);
			i += 2;
		} else if ((byte & 0xF0) == 0xE0) {
			if (i + 2 >= input_len)
				break;
			codepoint = ((byte & 0x0F) << 12) | ((static_cast<uint8_t>(input[i + 1]) & 0x3F) << 6) |
						(static_cast<uint8_t>(input[i + 2]) & 0x3F);
			i += 3;
		} else if ((byte & 0xF8) == 0xF0) {
			if (i + 3 >= input_len)
				break;
			codepoint = ((byte & 0x07) << 18) | ((static_cast<uint8_t>(input[i + 1]) & 0x3F) << 12) |
						((static_cast<uint8_t>(input[i + 2]) & 0x3F) << 6) |
						(static_cast<uint8_t>(input[i + 3]) & 0x3F);
			i += 4;
		} else {
			i += 1;
			continue;
		}
		if (codepoint <= 0xFFFF) {
			output[out_pos++] = static_cast<uint8_t>(codepoint & 0xFF);
			output[out_pos++] = static_cast<uint8_t>((codepoint >> 8) & 0xFF);
		} else if (codepoint <= 0x10FFFF) {
			codepoint -= 0x10000;
			uint16_t high = 0xD800 + ((codepoint >> 10) & 0x3FF);
			uint16_t low = 0xDC00 + (codepoint & 0x3FF);
			output[out_pos++] = static_cast<uint8_t>(high & 0xFF);
			output[out_pos++] = static_cast<uint8_t>((high >> 8) & 0xFF);
			output[out_pos++] = static_cast<uint8_t>(low & 0xFF);
			output[out_pos++] = static_cast<uint8_t>((low >> 8) & 0xFF);
		}
	}
	return out_pos;
}

// Retention cap for the reusable per-thread scratch buffer, in char16_t
// units (64K units = 128 KB per thread). Inputs above the cap are serviced
// through the caller's local vector — freed at scope exit — so a single huge
// nvarchar(max) value cannot pin ~2x its size in every worker thread for the
// thread's remaining lifetime.
constexpr size_t MAX_RETAINED_SCRATCH_UNITS = 64 * 1024;

// Pick the scratch buffer for `units` char16_t: the grow-only per-thread
// buffer below the retention cap, else the caller's `local`. The returned
// buffer has size() >= units.
std::vector<char16_t> &ScratchFor(size_t units, std::vector<char16_t> &local) {
	static thread_local std::vector<char16_t> retained;
	std::vector<char16_t> &chosen = units <= MAX_RETAINED_SCRATCH_UNITS ? retained : local;
	if (chosen.size() < units) {
		chosen.resize(units);
	}
	return chosen;
}

// Return a 2-byte-aligned char16_t view of `data` (code_units > 0). Aligned
// input is returned directly; unaligned input is copied into a scratch
// buffer (thread-local under the retention cap, else `local`) first. The
// view is valid until the next ScratchFor/AlignedUtf16View call on this
// thread or until `local` is destroyed, whichever the copy landed in.
const char16_t *AlignedUtf16View(const uint8_t *data, size_t code_units, std::vector<char16_t> &local) {
	if ((reinterpret_cast<uintptr_t>(data) & 0x1u) == 0u) {
		return reinterpret_cast<const char16_t *>(data);
	}
	std::vector<char16_t> &scratch = ScratchFor(code_units, local);
	std::memcpy(scratch.data(), data, code_units * 2);
	return scratch.data();
}

// Decode UTF-16LE that is NOT valid UTF-16, using standard U+FFFD substitution.
//
// SQL Server NVARCHAR is UCS-2, so an unpaired surrogate is a legal value on the
// wire and must not fail a query. simdutf is strictly conformant and rejects it,
// hence this path.
//
// Semantics are WHATWG maximal-subpart: each ill-formed code unit becomes
// exactly one U+FFFD, and decoding resumes at the NEXT unit. That matters — the
// hand-rolled converter this replaces consumed the unit *after* an unpaired high
// surrogate before emitting its single replacement, so `D800 0041` lost the 'A'
// entirely, and a high surrogate in the final position produced no output at
// all. Both were silent data loss rather than replacement.
//
// simdutf has no lossy mode (checked through 6.1.1), so the loop converts each
// valid run in bulk and splices replacements between them. Bulk conversion of
// the runs is why this is also ~1.3x faster than the scalar converter it
// replaces (spec 055 D2).
std::string DecodeUtf16LeReplacing(const char16_t *src, size_t code_units) {
	static const char kReplacement[] = "\xEF\xBF\xBD";	// U+FFFD

	std::string result;
	// One replacement per unit is the worst case; 3 bytes/unit also covers every
	// well-formed BMP run, so a single reserve holds for both.
	result.reserve(code_units * 3);

	size_t pos = 0;
	while (pos < code_units) {
		// Locate the next ill-formed unit WITHOUT converting: the conversion
		// entry points all write through their output pointer, so they cannot be
		// used as a probe.
		const simdutf::result r = simdutf::validate_utf16le_with_errors(src + pos, code_units - pos);
		const bool ok = r.error == simdutf::error_code::SUCCESS;
		const size_t valid_units = ok ? code_units - pos : r.count;

		if (valid_units > 0) {
			const size_t out_bytes = simdutf::utf8_length_from_utf16le(src + pos, valid_units);
			const size_t base = result.size();
			result.resize(base + out_bytes);
			if (out_bytes > 0) {
				// The byte count is already known — utf8_length_from_utf16le just
				// computed it over the same run simdutf declared valid — so the
				// return is discarded. Assigned rather than (void)-cast because
				// GCC ignores a void cast on warn_unused_result.
				const size_t written = simdutf::convert_valid_utf16le_to_utf8(src + pos, valid_units, &result[base]);
				(void)written;
			}
		}
		if (ok) {
			return result;
		}
		result.append(kReplacement, 3);
		pos += valid_units + 1;	 // step over exactly the offending unit
	}
	return result;
}

}  // anonymous namespace

//===----------------------------------------------------------------------===//
// Public simdutf-backed UTF-16LE conversion primitives.
//===----------------------------------------------------------------------===//

// D4 (spec 054): per-thread count of legacy-fallback conversions — see the
// header comment on Utf16FallbackCount(). Incremented only on the fallback
// (already-slow) path; never read on the fast path.
static thread_local uint64_t utf16_fallback_count = 0;

uint64_t Utf16FallbackCount() {
	return utf16_fallback_count;
}

std::vector<uint8_t> Utf16LEEncode(const std::string &input) {
	if (input.empty()) {
		return {};
	}

	const char *src = input.data();
	const size_t src_len = input.size();

	if (!simdutf::validate_utf8(src, src_len)) {
		utf16_fallback_count++;
		return LegacyUtf16LEEncode(input);
	}

	const size_t code_units = simdutf::utf16_length_from_utf8(src, src_len);
	std::vector<uint8_t> result(code_units * 2);
	if (code_units == 0) {
		return result;
	}

	char16_t *out = reinterpret_cast<char16_t *>(result.data());
	const size_t written = simdutf::convert_valid_utf8_to_utf16le(src, src_len, out);
	result.resize(written * 2);
	return result;
}

size_t Utf16LEEncodeDirect(const char *input, size_t input_len, uint8_t *output) {
	if (input_len == 0) {
		return 0;
	}

	if (!simdutf::validate_utf8(input, input_len)) {
		utf16_fallback_count++;
		return LegacyUtf16LEEncodeDirect(input, input_len, output);
	}

	// Defensive guard against unaligned output buffers; in practice every
	// known call site passes a 2-byte-aligned destination.
	if ((reinterpret_cast<uintptr_t>(output) & 0x1u) != 0u) {
		utf16_fallback_count++;
		return LegacyUtf16LEEncodeDirect(input, input_len, output);
	}

	char16_t *out = reinterpret_cast<char16_t *>(output);
	const size_t written = simdutf::convert_valid_utf8_to_utf16le(input, input_len, out);
	return written * 2;
}

size_t Utf16LEByteLength(const std::string &input) {
	if (input.empty()) {
		return 0;
	}

	if (!simdutf::validate_utf8(input.data(), input.size())) {
		return LegacyUtf16LEByteLength(input);
	}

	return simdutf::utf16_length_from_utf8(input.data(), input.size()) * 2;
}

size_t Utf16LEByteLengthView(const char *input, size_t input_len, bool &valid_utf8) {
	if (input_len == 0) {
		valid_utf8 = true;
		return 0;
	}
	valid_utf8 = simdutf::validate_utf8(input, input_len);
	if (!valid_utf8) {
		// NOT counted as a fallback here — the follow-up encode call counts
		// it once (see the Utf16FallbackCount contract in the header).
		return LegacyUtf16LEByteLength(std::string(input, input_len));
	}
	return simdutf::utf16_length_from_utf8(input, input_len) * 2;
}

size_t Utf16LEEncodeValidDirect(const char *input, size_t input_len, uint8_t *output) {
	if (input_len == 0) {
		return 0;
	}
	if ((reinterpret_cast<uintptr_t>(output) & 0x1u) == 0u) {
		char16_t *out = reinterpret_cast<char16_t *>(output);
		return simdutf::convert_valid_utf8_to_utf16le(input, input_len, out) * 2;
	}
	// Unaligned destination (odd offset inside a packet buffer): convert via
	// a scratch buffer, then memcpy. Stays on the simdutf path — the old
	// Utf16LEEncodeDirect fell back to the scalar legacy converter here, which
	// silently serialized ~half of all BCP string values through the slow
	// path (buffer parity is arbitrary after variable-width tokens).
	std::vector<char16_t> local;
	std::vector<char16_t> &scratch = ScratchFor(input_len, local);
	const size_t units = simdutf::convert_valid_utf8_to_utf16le(input, input_len, scratch.data());
	std::memcpy(output, scratch.data(), units * 2);
	return units * 2;
}

std::string Utf16LEDecode(const uint8_t *data, size_t byte_length) {
	if (byte_length == 0) {
		return {};
	}

	const size_t code_units = byte_length / 2;
	std::vector<char16_t> local;
	const char16_t *src = AlignedUtf16View(data, code_units, local);
	if (!simdutf::validate_utf16le(src, code_units)) {
		utf16_fallback_count++;
		return DecodeUtf16LeReplacing(src, code_units);
	}

	const size_t out_bytes = simdutf::utf8_length_from_utf16le(src, code_units);
	std::string result(out_bytes, '\0');
	if (out_bytes == 0) {
		return result;
	}
	const size_t written = simdutf::convert_valid_utf16le_to_utf8(src, code_units, &result[0]);
	result.resize(written);
	return result;
}

std::string Utf16LEDecode(const std::vector<uint8_t> &data) {
	return Utf16LEDecode(data.data(), data.size());
}

size_t Utf8LengthFromUtf16LEView(const uint8_t *data, size_t byte_length) {
	if (byte_length == 0) {
		return 0;
	}
	const size_t code_units = byte_length / 2;
	// TDS wire payloads come out of vector<uint8_t> (malloc-aligned), so the
	// unaligned copy inside AlignedUtf16View is defensive only.
	std::vector<char16_t> local;
	const char16_t *src = AlignedUtf16View(data, code_units, local);
	if (!simdutf::validate_utf16le(src, code_units)) {
		// NOT counted as a fallback — the caller's legacy decode call
		// counts it once (Utf16FallbackCount contract).
		return SIZE_MAX;
	}
	return simdutf::utf8_length_from_utf16le(src, code_units);
}

size_t Utf16LEDecodeValidInto(const uint8_t *data, size_t byte_length, char *out) {
	if (byte_length == 0) {
		return 0;
	}
	const size_t code_units = byte_length / 2;
	std::vector<char16_t> local;
	const char16_t *src = AlignedUtf16View(data, code_units, local);
	return simdutf::convert_valid_utf16le_to_utf8(src, code_units, out);
}

//===----------------------------------------------------------------------===//
// Test-only re-export of the private legacy hand-rolled converter.
// Visible only when MSSQL_BENCH_BUILD is defined at compile time (the
// microbenchmark target and the LOGIN7 unit test enable it). The production
// extension is built without MSSQL_BENCH_BUILD, so these symbols are
// unreachable from production code.
//===----------------------------------------------------------------------===//

#ifdef MSSQL_BENCH_BUILD

namespace testing {

std::vector<uint8_t> LegacyUtf16LEEncode(const std::string &input) {
	return ::duckdb::tds::encoding::LegacyUtf16LEEncode(input);
}

size_t LegacyUtf16LEByteLength(const std::string &input) {
	return ::duckdb::tds::encoding::LegacyUtf16LEByteLength(input);
}

size_t LegacyUtf16LEEncodeDirect(const char *input, size_t input_len, uint8_t *output) {
	return ::duckdb::tds::encoding::LegacyUtf16LEEncodeDirect(input, input_len, output);
}

}  // namespace testing

#endif	// MSSQL_BENCH_BUILD

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb
