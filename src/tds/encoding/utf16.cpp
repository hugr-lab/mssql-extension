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
//   3. Invalid input -> private LegacyUtf16LE* fallback, preserving the
//      pre-spec-043 "skip invalid bytes, continue" semantics bit-for-bit.
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
	size_t i = 0;
	const uint64_t *ptr64 = reinterpret_cast<const uint64_t *>(data);
	const size_t chunks = len / 8;
	for (size_t c = 0; c < chunks; c++) {
		if (ptr64[c] & 0x8080808080808080ULL) {
			return false;
		}
	}
	i = chunks * 8;
	for (; i < len; i++) {
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

std::string LegacyUtf16LEDecode(const uint8_t *data, size_t byte_length) {
	std::string result;
	result.reserve(byte_length);
	size_t i = 0;
	while (i + 1 < byte_length) {
		uint16_t code_unit = static_cast<uint16_t>(data[i]) | (static_cast<uint16_t>(data[i + 1]) << 8);
		i += 2;
		uint32_t codepoint;
		if (code_unit >= 0xD800 && code_unit <= 0xDBFF) {
			if (i + 1 >= byte_length)
				break;
			uint16_t low_surrogate = static_cast<uint16_t>(data[i]) | (static_cast<uint16_t>(data[i + 1]) << 8);
			i += 2;
			if (low_surrogate >= 0xDC00 && low_surrogate <= 0xDFFF) {
				codepoint = 0x10000 + ((static_cast<uint32_t>(code_unit - 0xD800) << 10) | (low_surrogate - 0xDC00));
			} else {
				codepoint = 0xFFFD;
			}
		} else if (code_unit >= 0xDC00 && code_unit <= 0xDFFF) {
			codepoint = 0xFFFD;
		} else {
			codepoint = code_unit;
		}
		if (codepoint <= 0x7F) {
			result.push_back(static_cast<char>(codepoint));
		} else if (codepoint <= 0x7FF) {
			result.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
			result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
		} else if (codepoint <= 0xFFFF) {
			result.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
			result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
			result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
		} else if (codepoint <= 0x10FFFF) {
			result.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
			result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
			result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
			result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
		}
	}
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

bool ValidateAlignedUtf16Le(const uint8_t *data, size_t byte_length, std::vector<char16_t> &scratch) {
	if (byte_length == 0) {
		return true;
	}
	const bool aligned = (reinterpret_cast<uintptr_t>(data) & 0x1u) == 0u;
	const size_t code_units = byte_length / 2;
	if (aligned) {
		return simdutf::validate_utf16le(reinterpret_cast<const char16_t *>(data), code_units);
	}
	scratch.resize(code_units);
	std::memcpy(scratch.data(), data, code_units * 2);
	return simdutf::validate_utf16le(scratch.data(), code_units);
}

}  // anonymous namespace

//===----------------------------------------------------------------------===//
// Public simdutf-backed UTF-16LE conversion primitives.
//===----------------------------------------------------------------------===//

std::vector<uint8_t> Utf16LEEncode(const std::string &input) {
	if (input.empty()) {
		return {};
	}

	const char *src = input.data();
	const size_t src_len = input.size();

	if (!simdutf::validate_utf8(src, src_len)) {
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
		return LegacyUtf16LEEncodeDirect(input, input_len, output);
	}

	// Defensive guard against unaligned output buffers; in practice every
	// known call site passes a 2-byte-aligned destination.
	if ((reinterpret_cast<uintptr_t>(output) & 0x1u) != 0u) {
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

std::string Utf16LEDecode(const uint8_t *data, size_t byte_length) {
	if (byte_length == 0) {
		return {};
	}

	std::vector<char16_t> aligned_scratch;
	if (!ValidateAlignedUtf16Le(data, byte_length, aligned_scratch)) {
		return LegacyUtf16LEDecode(data, byte_length);
	}

	const size_t code_units = byte_length / 2;
	const char16_t *src;
	std::vector<char16_t> copy;
	if ((reinterpret_cast<uintptr_t>(data) & 0x1u) == 0u) {
		src = reinterpret_cast<const char16_t *>(data);
	} else if (!aligned_scratch.empty()) {
		src = aligned_scratch.data();
	} else {
		copy.resize(code_units);
		std::memcpy(copy.data(), data, code_units * 2);
		src = copy.data();
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

std::string LegacyUtf16LEDecode(const uint8_t *data, size_t byte_length) {
	return ::duckdb::tds::encoding::LegacyUtf16LEDecode(data, byte_length);
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
