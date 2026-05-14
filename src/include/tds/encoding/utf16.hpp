//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// utf16.hpp
//
// UTF-8 <-> UTF-16LE conversion primitives, simdutf-backed (post spec 044).
//
// Spec 044 (Codec Layer Consolidation) folded the simdutf wrapper that
// spec 043 introduced as src/tds/encoding/simdutf_wrappers.{hpp,cpp} back
// into this header's pre-spec-043 path. Public function names are unchanged
// from the pre-spec-043 legacy converter API; the implementation behind them
// is now simdutf-backed (with a private hand-rolled fallback on invalid
// input).
//
// Invalid-input contract (preserved from spec 043 Clarification Q1):
//   1. Pre-validate input via simdutf::validate_utf8 (encode direction) or
//      validate_utf16le (decode direction).
//   2. Valid input -> SIMD fast path via convert_valid_*.
//   3. Invalid input -> private hand-rolled converter (anonymous namespace
//      inside utf16.cpp), preserving the legacy "skip invalid bytes,
//      continue" semantics bit-for-bit at every consumer call site.
//   4. Never throws on invalid input.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace tds {
namespace encoding {

/// Encode a UTF-8 string to UTF-16LE bytes. Bitwise-identical to the
/// pre-spec-043 hand-rolled converter on valid UTF-8 input.
std::vector<uint8_t> Utf16LEEncode(const std::string &input);

/// Decode UTF-16LE bytes to a UTF-8 string.
/// `byte_length` is in bytes (not code units). Bitwise-identical to the
/// pre-spec-043 hand-rolled converter on valid UTF-16LE input.
std::string Utf16LEDecode(const uint8_t *data, size_t byte_length);

/// Convenience overload taking a byte vector.
std::string Utf16LEDecode(const std::vector<uint8_t> &data);

/// Number of bytes the input would occupy when encoded as UTF-16LE.
/// Equal to `Utf16LEEncode(input).size()` but avoids the allocation.
size_t Utf16LEByteLength(const std::string &input);

/// Encode UTF-8 directly into a caller-owned output buffer. Returns the
/// number of UTF-16LE bytes written. `output` must have capacity for at
/// least `input_len * 2` bytes. Falls back to the private hand-rolled
/// converter when `output` is not 2-byte aligned (defensive guard).
size_t Utf16LEEncodeDirect(const char *input, size_t input_len, uint8_t *output);

//===----------------------------------------------------------------------===//
// Test-only re-export of the private legacy hand-rolled converter.
// Compiled only when MSSQL_BENCH_BUILD is defined (the microbenchmark and
// the LOGIN7 unit test enable it). The production extension is built
// without MSSQL_BENCH_BUILD, so these symbols are unreachable in production.
//===----------------------------------------------------------------------===//

#ifdef MSSQL_BENCH_BUILD

namespace testing {

/// Hand-rolled UTF-8 -> UTF-16LE converter, exposed only for the
/// microbenchmark and the LOGIN7 spec-043 bit-identity unit test. Do not
/// depend on this symbol from production code.
std::vector<uint8_t> LegacyUtf16LEEncode(const std::string &input);
std::string LegacyUtf16LEDecode(const uint8_t *data, size_t byte_length);
size_t LegacyUtf16LEByteLength(const std::string &input);
size_t LegacyUtf16LEEncodeDirect(const char *input, size_t input_len, uint8_t *output);

}  // namespace testing

#endif	// MSSQL_BENCH_BUILD

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb
