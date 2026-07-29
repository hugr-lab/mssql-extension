//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/staging/column_ops.hpp
//
// Per-column dispatch resolution for the staged read path (spec 055 D4).
//
// Resolved ONCE per column after COLMETADATA, so the per-value path carries no
// type dispatch at all — today's `TypeConverter::ConvertValue` runs a switch
// over the TDS type for every cell (type_converter.cpp:263-345). Same shape as
// the shipped write-path W2 (`BCPRowEncoder::ResolveEncoder`).
//===----------------------------------------------------------------------===//

#pragma once

#include "codec/staging/column_staging.hpp"
#include "duckdb/common/types.hpp"
#include "tds/tds_column_metadata.hpp"

namespace duckdb {
namespace mssql {
namespace codec {
namespace staging {

//! The single per-value dispatch of the staged row walk.
//!
//! Wire framing and destination handling are FUSED into one selector rather than
//! kept as two nested switches. Both are column-invariant, so either way the
//! predictor is right every time — but two switches mean two indirect jumps per
//! value, and the whole per-value budget is a couple of nanoseconds.
//!
//! The prefix in the name is the framing:
//!   Raw — the value has no length prefix (TDS's non-nullable fixed types).
//!   P1  — one length byte, 0 meaning NULL (the *N nullable variants).
//! The suffix is the width in bytes.
enum class AppendArm : uint8_t {
	RawDirect1 = 0,
	RawDirect2 = 1,
	RawDirect4 = 2,
	RawDirect8 = 3,
	P1Direct1 = 4,
	P1Direct2 = 5,
	P1Direct4 = 6,
	P1Direct8 = 7,
	//! Framing and conversion both go through the legacy per-value path. This is
	//! every family that does not yet have a batch kernel, plus the issue-#89
	//! divergence case. Cost is identical to the pre-staging path — one copy into
	//! a reused scratch buffer, then TypeConverter::ConvertValue — so a family
	//! without a kernel is not made slower by staging existing.
	Convert = 8,
	//! Parsed for its length only; the value is discarded. Columns the query does
	//! not project (COUNT(*), a narrower output chunk) still have to be walked to
	//! find where the next column starts.
	Skip = 9
};

//! Everything the staged path needs to know about one column, decided once.
struct ColumnOps {
	//! The per-value arm. Set by ResolveColumnOps; the caller downgrades it to
	//! Skip for columns it does not intend to fill.
	AppendArm arm = AppendArm::Convert;
	//! Which append arm the row reader uses for this column.
	StagingKind kind = StagingKind::Var;
	//! Bytes per value for Direct / Fixed; 0 for Var.
	uint32_t stride = 0;
	//! True when the wire bytes ARE the DuckDB representation, so the value can
	//! be stored into the output vector with no conversion whatsoever. This is
	//! the "direct-write bypass": no staging buffer, no finalize kernel.
	bool direct_write = false;
	//! True when the column must go through the per-value legacy converter
	//! instead of a batch kernel. Set when the catalog-declared type and the
	//! type actually arriving on the wire disagree — a view with an inline CAST
	//! can do that (issue #89). Today that check is a branch on every cell
	//! (type_converter.cpp:275); here it is a column-level property.
	bool needs_value_fallback = false;
	//! Var only: the declared upper bound on ONE value's wire size, in bytes, or
	//! 0 when the type has no bound (PLP / MAX). Lets the staging buffer be
	//! preallocated to a chunk's provable worst case, so a narrow column never
	//! resizes at all.
	uint32_t max_value_bytes = 0;
};

//! Resolve a column.
//!
//! `target_type` is the type of the vector the values will land in, which is NOT
//! always what the wire metadata implies: the scan can CAST (spec 026 VARCHAR ->
//! NVARCHAR) and views can diverge (issue #89). When they disagree the column is
//! marked `needs_value_fallback` and never takes a direct or batch path, so a
//! divergence degrades to today's behaviour instead of writing wrong bytes into
//! a vector of a different physical type.
ColumnOps ResolveColumnOps(const tds::ColumnMetadata &column, const LogicalType &target_type);

}  // namespace staging
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
