//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/staging/column_ops.cpp — spec 055 D4.
//===----------------------------------------------------------------------===//

#include "codec/staging/column_ops.hpp"

#include "tds/encoding/type_converter.hpp"
#include "tds/tds_types.hpp"

namespace duckdb {
namespace mssql {
namespace codec {
namespace staging {

using duckdb::tds::ColumnMetadata;

namespace {

//! Width of a fixed-width TDS value in bytes, or 0 if the type is not fixed
//! width. `max_length` carries the width for the nullable *N variants.
uint32_t FixedWireWidth(const tds::ColumnMetadata &column) {
	switch (column.type_id) {
	case tds::TDS_TYPE_TINYINT:
	case tds::TDS_TYPE_BIT:
		return 1;
	case tds::TDS_TYPE_SMALLINT:
		return 2;
	case tds::TDS_TYPE_INT:
	case tds::TDS_TYPE_REAL:
	case tds::TDS_TYPE_SMALLMONEY:
		return 4;
	case tds::TDS_TYPE_BIGINT:
	case tds::TDS_TYPE_FLOAT:
	case tds::TDS_TYPE_MONEY:
	case tds::TDS_TYPE_DATETIME:
		return 8;
	case tds::TDS_TYPE_SMALLDATETIME:
		return 4;
	case tds::TDS_TYPE_INTN:
	case tds::TDS_TYPE_FLOATN:
	case tds::TDS_TYPE_BITN:
	case tds::TDS_TYPE_MONEYN:
	case tds::TDS_TYPE_DATETIMEN:
		// Nullable variants: the declared width lives in max_length. The value
		// still arrives length-prefixed, but every non-NULL value is exactly
		// this wide.
		return static_cast<uint32_t>(column.max_length);
	case tds::TDS_TYPE_UNIQUEIDENTIFIER:
		return 16;
	default:
		// DECIMAL/NUMERIC, DATE, TIME, DATETIME2, DATETIMEOFFSET are fixed width
		// per column but their width depends on precision/scale, and they all
		// need a conversion pass regardless — they are classified Fixed below
		// via the codec's own knowledge, not here.
		return 0;
	}
}

//! Is this TDS type's wire representation byte-identical to how DuckDB stores
//! the corresponding logical type?
//!
//! Only integers and IEEE-754 floats qualify: TDS sends them little-endian
//! two's complement / IEEE-754, which is exactly DuckDB's in-memory layout on
//! every platform this builds for.
//!
//! BIT is deliberately NOT here. The wire byte is nominally 0/1 but nothing in
//! the protocol guarantees it, and DuckDB's bool must be exactly 0 or 1 —
//! storing a stray 2 would produce a value that compares and hashes
//! inconsistently. It costs one normalising pass to be certain, so BIT stays
//! Fixed.
bool IsDirectWritable(const tds::ColumnMetadata &column, uint32_t width) {
	switch (column.type_id) {
	case tds::TDS_TYPE_TINYINT:
		return width == 1;
	case tds::TDS_TYPE_SMALLINT:
		return width == 2;
	case tds::TDS_TYPE_INT:
		return width == 4;
	case tds::TDS_TYPE_BIGINT:
		return width == 8;
	case tds::TDS_TYPE_REAL:
		return width == 4;
	case tds::TDS_TYPE_FLOAT:
		return width == 8;
	case tds::TDS_TYPE_INTN:
		return width == 1 || width == 2 || width == 4 || width == 8;
	case tds::TDS_TYPE_FLOATN:
		return width == 4 || width == 8;
	default:
		return false;
	}
}

}  // namespace

ColumnOps ResolveColumnOps(const tds::ColumnMetadata &column, const LogicalType &target_type) {
	ColumnOps ops;

	// The wire type must agree with the vector we are writing into. A view with
	// an inline CAST can make the catalog promise VARCHAR while TDS delivers
	// something else (issue #89); the scan can also CAST VARCHAR to NVARCHAR
	// itself (spec 026). Resolving that disagreement here, once, keeps the
	// per-cell branch out of the hot path — and makes the safe answer the
	// default: anything we cannot prove matches goes down the legacy per-value
	// path rather than into a batch kernel with the wrong physical type.
	LogicalType wire_type;
	try {
		wire_type = tds::encoding::TypeConverter::GetDuckDBType(column);
	} catch (const std::exception &) {
		// Unknown wire type — the per-value converter owns the error path (D8).
		ops.kind = StagingKind::Var;
		ops.needs_value_fallback = true;
		return ops;
	}

	if (wire_type != target_type) {
		ops.kind = StagingKind::Var;
		ops.needs_value_fallback = true;
		return ops;
	}

	const uint32_t width = FixedWireWidth(column);
	if (width > 0 && IsDirectWritable(column, width)) {
		ops.kind = StagingKind::Direct;
		ops.stride = width;
		ops.direct_write = true;
		return ops;
	}
	if (width > 0) {
		ops.kind = StagingKind::Fixed;
		ops.stride = width;
		return ops;
	}

	// Everything else: variable length on the wire (strings, binary, PLP), or
	// fixed width with a precision-dependent size (DECIMAL, DATE, TIME,
	// DATETIME2, DATETIMEOFFSET). Both stage as Var — the second group could be
	// narrowed to Fixed once its per-column width is derived, which is a
	// follow-up, not a correctness matter: Var stages the same bytes and only
	// costs an offset/length pair per value.
	ops.kind = StagingKind::Var;
	return ops;
}

}  // namespace staging
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
