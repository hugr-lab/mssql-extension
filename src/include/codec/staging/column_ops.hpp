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

//! Everything the staged path needs to know about one column, decided once.
struct ColumnOps {
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
