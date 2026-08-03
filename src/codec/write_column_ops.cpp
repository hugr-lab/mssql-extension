//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/write_column_ops.cpp
//
// Per-column dispatch resolution for the BCP write path. See the header.
//===----------------------------------------------------------------------===//

#include "codec/write_column_ops.hpp"

#include "duckdb/common/types.hpp"
#include "tds/encoding/bcp_row_encoder.hpp"

namespace duckdb {
namespace mssql {
namespace codec {

namespace {

//! The payload width a direct copy would need for this target type, or 0 if the
//! type is not a direct-copy candidate at all.
//!
//! SQL Server's tinyint is UNSIGNED 0..255 while DuckDB's TINYINT is signed; the
//! byte that goes on the wire is the same byte either way, so a copy is exact
//! and this file does not change that. Realigning the signedness is a
//! wire-compatibility decision and is tracked separately.
uint32_t DirectCopyTargetWidth(const LogicalType &target_type) {
	switch (target_type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
		return 1;
	case LogicalTypeId::SMALLINT:
		return 2;
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::FLOAT:
		return 4;
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::DOUBLE:
		return 8;
	default:
		return 0;
	}
}

ScatterArm DirectCopyArm(uint32_t width) {
	switch (width) {
	case 1:
		return ScatterArm::DirectCopy1;
	case 2:
		return ScatterArm::DirectCopy2;
	case 4:
		return ScatterArm::DirectCopy4;
	case 8:
		return ScatterArm::DirectCopy8;
	default:
		return ScatterArm::Unsupported;
	}
}

}  // namespace

WriteColumnOps ResolveWriteColumnOps(const LogicalType &source, const mssql::BCPColumnMetadata &target) {
	WriteColumnOps ops;
	ops.kind = target.IsPLPType() ? WireKind::Plp
								  : (target.IsVariableLengthUSHORT() ? WireKind::VariableUShort : WireKind::Fixed);

	if (ops.kind != WireKind::Fixed) {
		// Variable-width families need staging and blocked assembly, which is a
		// later commit. Encodable, just not scatterable.
		ops.arm = ScatterArm::RowFallback;
		return ops;
	}

	// Fixed-width families that need a transformation rather than a copy. Each is
	// still one width per column, so the stride model holds; only the arm differs.
	if (target.duckdb_type.id() == LogicalTypeId::DECIMAL) {
		// WidenVectorToHugeint accepts exactly these; anything else would read the
		// wrong width out of the vector.
		const PhysicalType src = source.InternalType();
		const bool src_ok = src == PhysicalType::INT16 || src == PhysicalType::INT32 || src == PhysicalType::INT64 ||
							src == PhysicalType::INT128;
		const uint32_t width = tds::encoding::BCPRowEncoder::GetDecimalByteSize(target.precision);
		if (src_ok && target.max_length == width) {
			ops.arm = ScatterArm::Decimal;
			ops.wire_width = width;
			return ops;
		}
		ops.arm = ScatterArm::RowFallback;
		ops.wire_width = target.max_length;
		return ops;
	}
	switch (target.duckdb_type.id()) {
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_SEC: {
		// The source must be the matching temporal storage: DATE is int32 days,
		// the TIMESTAMP variants int64 ticks. Anything else is a conversion and
		// goes the row path, which performs it.
		const PhysicalType src = source.InternalType();
		const bool date_ok = target.duckdb_type.id() == LogicalTypeId::DATE && src == PhysicalType::INT32 &&
							 source.id() == LogicalTypeId::DATE && target.max_length == 3;
		// The width the kernel WRITES must equal the width the chunk was sized
		// from, or every later column in the row lands where the server is not
		// looking. The row path never had this constraint because it appends —
		// whatever it writes IS the layout. The scatter reserves first.
		//
		// This is not hypothetical: `datetime`, `datetime2` and `smalldatetime`
		// all declare TDS_TYPE_DATETIME2 on the wire, but sys.columns reports
		// max_length 8, 6/7/8 and 4 respectively. An old `datetime` column at
		// scale 0 is sized 8 and would be written 6, desynchronising the stream
		// with "premature end-of-message".
		//
		// **Legacy `datetime` and `smalldatetime` therefore keep the row path,
		// deliberately.** They are correct there — the value still goes out in
		// datetime2 form and the server converts, and the rounding and day-carry
		// rules are shared, so only the speed differs. Giving them the kernel
		// needs SQLServerTypeMaxLength to report what is actually SENT rather
		// than the column's own storage size, which changes COLMETADATA — what
		// the server is told — and so is a separate change with its own
		// verification, for a narrow gain: tables still on the legacy type.
		const uint32_t dt2_written =
			static_cast<uint32_t>(tds::encoding::BCPRowEncoder::GetTimeByteSize(target.scale)) + 3;
		const bool ts_ok = target.duckdb_type.id() != LogicalTypeId::DATE && src == PhysicalType::INT64 &&
						   target.max_length == dt2_written;
		if (date_ok || ts_ok) {
			ops.arm = ScatterArm::Datetime;
			ops.wire_width = target.max_length;
			return ops;
		}
		ops.arm = ScatterArm::RowFallback;
		ops.wire_width = target.max_length;
		return ops;
	}
	default:
		break;
	}

	if (target.duckdb_type.id() == LogicalTypeId::UUID) {
		if (source.id() == LogicalTypeId::UUID && target.max_length == 16) {
			ops.arm = ScatterArm::Guid;
			ops.wire_width = 16;
			return ops;
		}
		ops.arm = ScatterArm::RowFallback;
		ops.wire_width = target.max_length;
		return ops;
	}

	const uint32_t want = DirectCopyTargetWidth(target.duckdb_type);
	if (want == 0) {
		// Temporal families are fixed width but still need a kernel. Resolvable,
		// row path for now.
		ops.arm = ScatterArm::RowFallback;
		ops.wire_width = target.max_length;
		return ops;
	}

	// COLMETADATA must agree with the type it declared, or the scatter would
	// write a different number of bytes than the server is expecting.
	if (target.max_length != want) {
		ops.arm = ScatterArm::RowFallback;
		ops.wire_width = target.max_length;
		return ops;
	}

	// The source has to already store exactly those bytes. A narrower or wider
	// source is a CONVERSION — issue #153's territory — and until that becomes an
	// arm of its own it goes through the row path, which performs it correctly.
	if (GetTypeIdSize(source.InternalType()) != want) {
		ops.arm = ScatterArm::RowFallback;
		ops.wire_width = want;
		return ops;
	}

	ops.arm = DirectCopyArm(want);
	ops.wire_width = want;
	return ops;
}

}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
