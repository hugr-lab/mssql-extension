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
//! BIT qualifies too: TDS defines it as exactly 0 or 1 (NULL travels in the
//! length prefix of BITN, not in the value byte), which is precisely DuckDB's
//! one-byte bool. There is no third value to normalise away.
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
	case tds::TDS_TYPE_BIT:
	case tds::TDS_TYPE_BITN:
		return width == 1;
	case tds::TDS_TYPE_INTN:
		return width == 1 || width == 2 || width == 4 || width == 8;
	case tds::TDS_TYPE_FLOATN:
		return width == 4 || width == 8;
	default:
		return false;
	}
}

//! Does this type carry a one-byte length prefix on the wire?
//!
//! Only the direct-writable set is classified here — everything else takes the
//! Convert arm, which re-derives framing through the legacy reader and so needs
//! no answer from us.
bool HasOneBytePrefix(uint8_t type_id) {
	switch (type_id) {
	case tds::TDS_TYPE_INTN:
	case tds::TDS_TYPE_BITN:
	case tds::TDS_TYPE_FLOATN:
		return true;
	default:
		// TINYINT, BIT, SMALLINT, INT, BIGINT, REAL, FLOAT: the non-nullable
		// forms, sent as bare value bytes.
		return false;
	}
}

AppendArm DirectArm(uint8_t type_id, uint32_t width) {
	const bool prefixed = HasOneBytePrefix(type_id);
	switch (width) {
	case 1:
		return prefixed ? AppendArm::P1Direct1 : AppendArm::RawDirect1;
	case 2:
		return prefixed ? AppendArm::P1Direct2 : AppendArm::RawDirect2;
	case 4:
		return prefixed ? AppendArm::P1Direct4 : AppendArm::RawDirect4;
	default:
		return prefixed ? AppendArm::P1Direct8 : AppendArm::RawDirect8;
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
	// A direct write stores wire bytes into the output vector at `width` stride,
	// so the wire width MUST equal the width DuckDB reserves per slot. The two
	// come from different files (FixedWireWidth here, GetDuckDBType in the type
	// converter) and today they agree for every direct type — TINYINT/BIT are one
	// byte in both, REAL/FLOAT four, and so on. Proving it here, once per column,
	// means a future remapping degrades to staging instead of writing 1 byte into
	// a 2-byte slot.
	if (width > 0 && IsDirectWritable(column, width) && width == GetTypeIdSize(target_type.InternalType())) {
		ops.kind = DirectKindForWidth(width);
		ops.arm = DirectArm(column.type_id, width);
		ops.stride = width;
		ops.direct_write = true;
		return ops;
	}
	if (column.type_id == tds::TDS_TYPE_UNIQUEIDENTIFIER) {
		// Fixed 16 bytes, mixed-endian, converted by arithmetic — a straight
		// batch loop with no branch and no allocation.
		ops.kind = StagingKind::Fixed;
		ops.arm = AppendArm::P1StageUuid;
		ops.stride = width;
		return ops;
	}
	if (width > 0) {
		ops.kind = StagingKind::Fixed;
		ops.stride = width;
		return ops;
	}

	// UTF-16 string columns take the batch decode (T10). Deliberately narrow:
	//
	//  - PLP/MAX is excluded because its value arrives as a chunk list with no
	//    length known up front, which the Var slot cannot express yet.
	//  - NCHAR/CHAR are excluded because their trailing-space trim must happen on
	//    the UNTRIMMED payload when the value turns out to be invalid UTF-16
	//    (the pre-054 semantics T9 preserved); trimming at staging time would
	//    throw those bytes away before we know.
	//
	// What is left — NVARCHAR and XML — is the overwhelming majority of real
	// string data.
	const bool utf16_string = column.type_id == tds::TDS_TYPE_NVARCHAR || column.type_id == tds::TDS_TYPE_XML;
	if (utf16_string && !column.IsPLPType() && column.max_length > 0) {
		ops.kind = StagingKind::Var;
		ops.arm = AppendArm::P2StageString;
		ops.max_value_bytes = static_cast<uint32_t>(column.max_length);
		return ops;
	}

	// VARBINARY: no conversion at all, so the batch path is one allocation and
	// one copy per column instead of one of each per value. PLP is excluded for
	// the same reason as strings — the chunked form has no length up front.
	const bool binary = column.type_id == tds::TDS_TYPE_BIGVARBINARY || column.type_id == tds::TDS_TYPE_BIGBINARY;
	if (binary && !column.IsPLPType() && column.max_length > 0) {
		ops.kind = StagingKind::Var;
		ops.arm = AppendArm::P2StageBinary;
		ops.max_value_bytes = static_cast<uint32_t>(column.max_length);
		return ops;
	}

	// Everything else: variable length on the wire (strings, binary, PLP), or
	// fixed width with a precision-dependent size (DECIMAL, DATE, TIME,
	// DATETIME2, DATETIMEOFFSET). Both stage as Var — the second group could be
	// narrowed to Fixed once its per-column width is derived, which is a
	// follow-up, not a correctness matter: Var stages the same bytes and only
	// costs an offset/length pair per value.
	ops.kind = StagingKind::Var;
	// A non-MAX variable column declares its widest possible value, which bounds
	// a whole chunk exactly. PLP/MAX declares nothing, so it stays 0 and the
	// buffer finds its size from the data instead.
	if (!column.IsPLPType() && column.max_length > 0) {
		ops.max_value_bytes = static_cast<uint32_t>(column.max_length);
	}
	return ops;
}

}  // namespace staging
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
