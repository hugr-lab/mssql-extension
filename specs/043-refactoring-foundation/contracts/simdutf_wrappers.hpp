// PROPOSED PUBLIC HEADER — contracts/simdutf_wrappers.hpp
//
// Spec: 043-refactoring-foundation
// Date: 2026-05-14
//
// This file is a CONTRACT, not source code. The actual file lives at
// src/include/tds/encoding/simdutf_wrappers.hpp once spec 043 is
// implemented. Signature finalization happens in tasks.md; this is
// the agreed shape going in.
//
// Why a separate symbol prefix (Simdutf*) vs reusing the legacy names:
// the legacy converter in src/tds/encoding/utf16.cpp stays in place
// for every call site outside LOGIN7 (spec 044 migrates the rest).
// Distinct symbols let both coexist with zero risk of accidental
// resolution to the wrong implementation.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace tds {
namespace encoding {

//===----------------------------------------------------------------------===//
// simdutf-backed UTF-16LE encoding / decoding wrappers
//
// Invalid-input contract (per spec 043 Clarification Q1):
//   1. Pre-validate the input with simdutf::validate_utf8 (or
//      validate_utf16le for the decode direction).
//   2. On valid input, use the SIMD fast path
//      (convert_valid_utf8_to_utf16le / convert_valid_utf16le_to_utf8).
//   3. On invalid input, fall back to the legacy hand-rolled converter
//      in src/tds/encoding/utf16.cpp (Utf16LEEncode / Utf16LEDecode /
//      Utf16LEByteLength / Utf16LEEncodeDirect) for that single call.
//      This preserves the legacy converter's "skip invalid bytes,
//      continue" behavior bit-for-bit.
//   4. Never throw on invalid input.
//===----------------------------------------------------------------------===//

/// Encode a UTF-8 string to UTF-16LE bytes via simdutf (with legacy
/// fallback on invalid input).
///
/// Bitwise-identical to legacy Utf16LEEncode on valid UTF-8 input.
std::vector<uint8_t> SimdutfUtf16LEEncode(const std::string &input);

/// Decode UTF-16LE bytes to a UTF-8 string via simdutf (with legacy
/// fallback on invalid input).
///
/// `byte_length` is in bytes, not UTF-16 code units. Bitwise-identical
/// to legacy Utf16LEDecode on valid UTF-16LE input.
std::string SimdutfUtf16LEDecode(const uint8_t *data, size_t byte_length);

/// Convenience overload taking a byte vector.
std::string SimdutfUtf16LEDecode(const std::vector<uint8_t> &data);

/// Return the byte length the input string would occupy when encoded
/// as UTF-16LE. Equal to `SimdutfUtf16LEEncode(input).size()` but
/// avoids the allocation.
///
/// Bitwise-identical to legacy Utf16LEByteLength on valid UTF-8 input.
size_t SimdutfUtf16LEByteLength(const std::string &input);

/// Encode UTF-8 directly to a caller-owned output buffer. Returns the
/// number of UTF-16LE bytes written. `output` must have capacity for
/// at least `input_len * 2` bytes.
///
/// Bitwise-identical to legacy Utf16LEEncodeDirect on valid UTF-8 input.
size_t SimdutfUtf16LEEncodeDirect(const char *input, size_t input_len, uint8_t *output);

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb

//===----------------------------------------------------------------------===//
// Coexistence with the legacy converter
//
// The legacy header at src/include/tds/encoding/utf16.hpp continues to
// exist unchanged after spec 043. Its functions retain their existing
// names:
//
//   duckdb::tds::encoding::Utf16LEEncode
//   duckdb::tds::encoding::Utf16LEDecode
//   duckdb::tds::encoding::Utf16LEByteLength
//   duckdb::tds::encoding::Utf16LEEncodeDirect
//
// Spec 043 introduces only the Simdutf* variants and uses them solely
// from the LOGIN7 builder. Spec 044 performs the consumer-side
// migration across the codebase.
//===----------------------------------------------------------------------===//
