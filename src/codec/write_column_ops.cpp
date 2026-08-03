//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/write_column_ops.cpp
//
// Per-column dispatch resolution for the BCP write path. See the header.
//===----------------------------------------------------------------------===//

#include "codec/write_column_ops.hpp"

#include "duckdb/common/types.hpp"

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

	const uint32_t want = DirectCopyTargetWidth(target.duckdb_type);
	if (want == 0) {
		// DECIMAL, temporal and GUID are fixed width but need a transformation
		// kernel, not a copy. They are resolvable and take the row path for now.
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
