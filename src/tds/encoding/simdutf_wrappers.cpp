//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// simdutf_wrappers.cpp
//
// SIMD-accelerated UTF-8 <-> UTF-16LE conversion primitives, with a legacy
// fallback for invalid input. See header for the full contract.
//
// Spec: 043 (LOGIN7 non-ASCII fix + simdutf foundation)
//===----------------------------------------------------------------------===//

#include "tds/encoding/simdutf_wrappers.hpp"

#include "tds/encoding/utf16.hpp"

#include <simdutf.h>

#include <cstring>

namespace duckdb {
namespace tds {
namespace encoding {

//===----------------------------------------------------------------------===//
// Encode: UTF-8 -> UTF-16LE
//===----------------------------------------------------------------------===//

std::vector<uint8_t> SimdutfUtf16LEEncode(const std::string &input) {
	if (input.empty()) {
		return {};
	}

	const char *src = input.data();
	const size_t src_len = input.size();

	// Pre-validate. On invalid input, fall back to legacy converter so the
	// observed "skip invalid bytes" semantics from v0.1.18 are preserved.
	if (!simdutf::validate_utf8(src, src_len)) {
		return Utf16LEEncode(input);
	}

	const size_t code_units = simdutf::utf16_length_from_utf8(src, src_len);
	std::vector<uint8_t> result(code_units * 2);
	if (code_units == 0) {
		return result;
	}

	// simdutf writes UTF-16LE units into a char16_t* output buffer; the
	// little-endian byte order is guaranteed by the function regardless of
	// host endianness. std::vector<uint8_t>::data() is at least 2-byte
	// aligned (alignof(max_align_t) >= 2), so the reinterpret_cast is safe.
	char16_t *out = reinterpret_cast<char16_t *>(result.data());
	const size_t written = simdutf::convert_valid_utf8_to_utf16le(src, src_len, out);
	result.resize(written * 2);
	return result;
}

size_t SimdutfUtf16LEEncodeDirect(const char *input, size_t input_len, uint8_t *output) {
	if (input_len == 0) {
		return 0;
	}

	if (!simdutf::validate_utf8(input, input_len)) {
		return Utf16LEEncodeDirect(input, input_len, output);
	}

	// `output` may be arbitrarily aligned; if it is not 2-byte aligned we
	// must not write through char16_t*. Fall back to the legacy converter
	// for that rare case (this is a defensive guard; in practice every
	// known call site passes a 2-byte-aligned buffer).
	if ((reinterpret_cast<uintptr_t>(output) & 0x1u) != 0u) {
		return Utf16LEEncodeDirect(input, input_len, output);
	}

	char16_t *out = reinterpret_cast<char16_t *>(output);
	const size_t written = simdutf::convert_valid_utf8_to_utf16le(input, input_len, out);
	return written * 2;
}

size_t SimdutfUtf16LEByteLength(const std::string &input) {
	if (input.empty()) {
		return 0;
	}

	if (!simdutf::validate_utf8(input.data(), input.size())) {
		return Utf16LEByteLength(input);
	}

	return simdutf::utf16_length_from_utf8(input.data(), input.size()) * 2;
}

//===----------------------------------------------------------------------===//
// Decode: UTF-16LE -> UTF-8
//===----------------------------------------------------------------------===//

namespace {

// Validate UTF-16LE bytes by interpreting them as a sequence of LE code
// units. simdutf::validate_utf16le requires a 2-byte-aligned char16_t*;
// if the source is unaligned (rare in our use, but possible for arbitrary
// byte buffers) we copy into an aligned scratch vector first.
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

}  // namespace

std::string SimdutfUtf16LEDecode(const uint8_t *data, size_t byte_length) {
	if (byte_length == 0) {
		return {};
	}

	std::vector<char16_t> aligned_scratch;
	if (!ValidateAlignedUtf16Le(data, byte_length, aligned_scratch)) {
		return Utf16LEDecode(data, byte_length);
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

std::string SimdutfUtf16LEDecode(const std::vector<uint8_t> &data) {
	return SimdutfUtf16LEDecode(data.data(), data.size());
}

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb
