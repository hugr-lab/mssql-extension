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
// Spec 043: introduced as the foundation for future migration. The LOGIN7
// builder is the only production consumer in this spec; bulk migration of
// other call sites lives in spec 044 (Codec Layer).
//
// Invalid-input contract (spec 043 Clarification Q1):
//   1. Pre-validate the input with simdutf::validate_utf8 (or
//      validate_utf16le for the decode direction).
//   2. On valid input, use the SIMD fast path
//      (convert_valid_utf8_to_utf16le / convert_valid_utf16le_to_utf8).
//   3. On invalid input, fall back to the legacy hand-rolled converter
//      in src/tds/encoding/utf16.cpp for that single call. This preserves
//      the legacy converter's "skip invalid bytes, continue" behavior
//      bit-for-bit at every consumer call site that spec 044 will migrate.
//   4. Never throw on invalid input.
//
// Coexistence with the legacy converter (src/include/tds/encoding/utf16.hpp):
//   These Simdutf* symbols are distinct from the legacy Utf16LE* names so
//   the two implementations can coexist during spec 044's migration.
//===----------------------------------------------------------------------===//

/// Encode a UTF-8 string to UTF-16LE bytes via simdutf, with legacy
/// fallback on invalid input. Bitwise-identical to legacy
/// Utf16LEEncode on valid UTF-8 input.
std::vector<uint8_t> SimdutfUtf16LEEncode(const std::string &input);

/// Decode UTF-16LE bytes to a UTF-8 string via simdutf, with legacy
/// fallback on invalid input. `byte_length` is in bytes (not code units).
/// Bitwise-identical to legacy Utf16LEDecode on valid UTF-16LE input.
std::string SimdutfUtf16LEDecode(const uint8_t *data, size_t byte_length);

/// Convenience overload taking a byte vector.
std::string SimdutfUtf16LEDecode(const std::vector<uint8_t> &data);

/// Number of bytes the input would occupy when encoded as UTF-16LE.
/// Equal to `SimdutfUtf16LEEncode(input).size()` but skips the allocation.
size_t SimdutfUtf16LEByteLength(const std::string &input);

/// Encode UTF-8 directly to a caller-owned output buffer. Returns the
/// number of UTF-16LE bytes written. `output` must have capacity for at
/// least `input_len * 2` bytes.
size_t SimdutfUtf16LEEncodeDirect(const char *input, size_t input_len, uint8_t *output);

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb
