//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/staging/string_finalize.hpp
//
// Batch UTF-16LE -> UTF-8 for a whole staged column (spec 055 T10).
//
// WHAT THE SHIPPED PATH DOES PER VALUE
// ------------------------------------
//   1. Utf8LengthFromUtf16LEView  — a full pass that both validates and counts
//   2. StringVector::EmptyString  — an allocation in the vector's string heap
//   3. Utf16LEDecodeValidInto     — a second pass that converts
//
// Step 1 costs MORE than step 3 (measured 36.6 vs ~21 ns at 200 characters),
// and it exists only to size step 2.
//
// WHAT THIS DOES
// --------------
// One conversion call for the entire column, into one allocation. simdutf's
// checked converter validates and converts in the same pass and returns the
// byte count, so the length pass disappears rather than being made faster.
//
// The count also answers, for free, whether the column was pure ASCII: if the
// output is exactly as many BYTES as the input had code UNITS, every unit
// produced one byte. Value boundaries in the output are then the staged byte
// offsets halved — no delimiter, no scan, no per-value bookkeeping.
//
// Measured against the shipped path, ns/value (2048-row chunks, real Vector):
//
//   chars   shipped   one alloc + no length pass   + single call
//       4      14.8                          7.0            3.1
//      16      18.0                          7.1            2.2
//     200      58.0                         20.1           11.5
//
// Both halves matter, which is why the ASCII fast path is here and not left as
// a refinement: the single call is worth more than everything else combined.
//===----------------------------------------------------------------------===//

#pragma once

#include "codec/staging/column_staging.hpp"
#include "duckdb/common/types/vector.hpp"
#include "tds/tds_column_metadata.hpp"

namespace duckdb {
namespace mssql {
namespace codec {
namespace staging {

//! Decode a staged UTF-16LE column into `out` as UTF-8.
//!
//! `st` must hold raw UTF-16LE wire bytes appended with AppendVar, `count` rows
//! staged, and validity in its mask. NULL rows are left untouched — their
//! offset/length slots are undefined by ColumnStaging's contract.
//!
//! Handles invalid UTF-16 (unpaired surrogates are legal in UCS-2 collations) by
//! falling the whole column back to the per-value replacing decoder, which is
//! the same U+FFFD behaviour the per-value path produces.
void FinalizeStringColumn(const ColumnStaging &st, idx_t count, Vector &out, bool trim_trailing_spaces);

}  // namespace staging
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
