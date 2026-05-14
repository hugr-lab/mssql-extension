//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// utf16.hpp  (PROPOSED POST-RENAME PUBLIC HEADER)
//
// This file is the *contract* artifact for spec 044 (Codec Layer
// Consolidation). It documents the post-rename public surface that
// `src/include/tds/encoding/utf16.hpp` MUST expose after the Phase-B
// rename commit. The actual implementation source will live at the same
// path during normal development; this file in `specs/044-.../contracts/`
// is the immutable design reference reviewers check against.
//
// Key invariants (signature-identical to the legacy pre-spec-043 utf16.hpp):
//   - same namespace (`duckdb::tds::encoding`)
//   - same function names (`Utf16LE*`, no `Simdutf` prefix)
//   - same parameter types and return types
// Difference: implementation is simdutf-backed (with private legacy
// fallback on invalid input); the old hand-rolled implementation is no
// longer reachable from this header.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace tds {
namespace encoding {

//===----------------------------------------------------------------------===//
// UTF-16LE encoding / decoding (simdutf-backed)
//
// Spec 044 consolidates every UTF-16 conversion in the extension behind
// this header. Pre-spec-044 the implementation was a hand-rolled
// converter in src/tds/encoding/utf16.cpp; spec 043 introduced the
// simdutf wrapper at simdutf_wrappers.{hpp,cpp} with a `Simdutf*`
// prefix for coexistence; spec 044 finishes the migration and renames
// the simdutf-backed wrapper back to the original Utf16LE* names.
//
// Invalid-input contract (preserved from spec 043 Clarification Q1):
//   1. Pre-validate input via simdutf::validate_utf8 (encode direction)
//      or validate_utf16le (decode direction).
//   2. On valid input, use the SIMD fast path
//      (convert_valid_utf8_to_utf16le / convert_valid_utf16le_to_utf8).
//   3. On invalid input, fall back to a private hand-rolled converter
//      (an anonymous-namespace LegacyUtf16LE* inside utf16.cpp).
//      Preserves the legacy converter's "skip invalid bytes, continue"
//      bit-identical semantics at every consumer.
//   4. Never throws on invalid input.
//===----------------------------------------------------------------------===//

/// Encode a UTF-8 string to UTF-16LE bytes. Bitwise-identical to the
/// legacy hand-rolled converter on valid UTF-8 input.
std::vector<uint8_t> Utf16LEEncode(const std::string &input);

/// Decode UTF-16LE bytes to a UTF-8 string.
/// `byte_length` is in bytes (not code units). Bitwise-identical to the
/// legacy hand-rolled converter on valid UTF-16LE input.
std::string Utf16LEDecode(const uint8_t *data, size_t byte_length);

/// Convenience overload taking a byte vector.
std::string Utf16LEDecode(const std::vector<uint8_t> &data);

/// Number of bytes the input would occupy when encoded as UTF-16LE.
/// Equal to `Utf16LEEncode(input).size()` but avoids the allocation.
size_t Utf16LEByteLength(const std::string &input);

/// Encode UTF-8 directly into a caller-owned output buffer. Returns
/// the number of UTF-16LE bytes written. `output` must have capacity
/// for at least `input_len * 2` bytes. Falls back to the private
/// legacy converter when `output` is not 2-byte aligned (defensive
/// guard; in practice every call site passes an aligned buffer).
size_t Utf16LEEncodeDirect(const char *input, size_t input_len, uint8_t *output);

//===----------------------------------------------------------------------===//
// Test-only re-export of the private legacy fallback.
// Compiled only when MSSQL_BENCH_BUILD is defined (the microbenchmark's
// build flag, not the production extension's). The production extension
// MUST NOT define MSSQL_BENCH_BUILD; the symbols below are unreachable
// in production builds.
//===----------------------------------------------------------------------===//

#ifdef MSSQL_BENCH_BUILD

namespace testing {

/// Re-export of the private hand-rolled UTF-8 -> UTF-16LE converter,
/// for use only by `test/cpp/bench_utf16.cpp` to compare against the
/// public simdutf-backed path. Do not depend on this symbol from
/// production code.
std::vector<uint8_t> LegacyUtf16LEEncode(const std::string &input);
std::string LegacyUtf16LEDecode(const uint8_t *data, size_t byte_length);
size_t LegacyUtf16LEByteLength(const std::string &input);
size_t LegacyUtf16LEEncodeDirect(const char *input, size_t input_len, uint8_t *output);

}  // namespace testing

#endif  // MSSQL_BENCH_BUILD

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb
