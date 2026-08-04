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
#include "tds/tds_types.hpp"

namespace duckdb {
namespace mssql {
namespace codec {

namespace {

//! The payload width a direct copy would need for this target type, or 0 if the
//! type is not a direct-copy candidate at all.
//!
//! TINYINT is listed for its WIDTH only. SQL Server's tinyint is UNSIGNED
//! 0..255, so whether a one-byte copy is exact depends on the source: it is for
//! UTINYINT and it is not for a signed TINYINT, whose negatives are the top half
//! of the target's range. ResolveWriteColumnOps sorts that out below; this
//! function answers "how wide", not "is it safe".
uint32_t DirectCopyTargetWidth(const LogicalType &target_type) {
	switch (target_type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::UTINYINT:
		return 1;
	case LogicalTypeId::TINYINT:
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

	if (ops.kind == WireKind::VariableUShort) {
		// nvarchar(n) and the UTF-8 varchar(n) form. PLP/MAX is not planned yet.
		//
		// The SOURCE must actually be a string, not merely the target: spec 045
		// FR-026 sends an INTERVAL as NVARCHAR, so the target is a string wire
		// form over a vector that stores interval_t. Reading it as string_t trips
		// "Expected unified vector format of type VARCHAR, but found type
		// INTERVAL". Any such render-as-text source keeps the row path, which
		// formats it first.
		const bool string_target =
			target.tds_type_token == tds::TDS_TYPE_NVARCHAR || target.tds_type_token == tds::TDS_TYPE_BIGVARCHAR;
		// varbinary(n) shares the framing exactly and needs no conversion at all,
		// so it rides the same arm. Its source is a BLOB, which DuckDB also stores
		// as string_t.
		const bool binary_target = target.tds_type_token == tds::TDS_TYPE_BIGVARBINARY;
		const bool string_source = source.InternalType() == PhysicalType::VARCHAR;
		ops.arm = ((string_target || binary_target) && string_source) ? ScatterArm::VarString : ScatterArm::RowFallback;
		return ops;
	}
	if (ops.kind == WireKind::Plp) {
		// nvarchar(max) / varchar(max) / varbinary(max). Same arm: the plan owns
		// the framing difference, so nothing else here has to know about it.
		const bool str_or_bin = target.tds_type_token == tds::TDS_TYPE_NVARCHAR ||
								target.tds_type_token == tds::TDS_TYPE_BIGVARCHAR ||
								target.tds_type_token == tds::TDS_TYPE_BIGVARBINARY;
		const bool string_source = source.InternalType() == PhysicalType::VARCHAR;
		ops.arm = (str_or_bin && string_source) ? ScatterArm::VarString : ScatterArm::RowFallback;
		return ops;
	}
	if (ops.kind != WireKind::Fixed) {
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
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIMESTAMP_TZ: {
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
		// `datetime`, `datetime2` and `smalldatetime` all declare
		// TDS_TYPE_DATETIME2 on the wire and share a `duckdb_type`, so the width
		// is the only thing that tells them apart — which is exactly why it is
		// checked rather than assumed.
		//
		// SQLServerTypeMaxLength now reports what is actually SENT for all of
		// them (it used to return a constant 8 for every datetime2, so only
		// datetime2(7) ever matched), which is safe because COLMETADATA for this
		// token carries the SCALE and no length at all.
		const uint32_t dt2_written =
			static_cast<uint32_t>(tds::encoding::BCPRowEncoder::GetTimeByteSize(target.scale)) + 3;
		// TIME has no date field, DATETIMEOFFSET adds a 2-byte offset, and both
		// must satisfy the same width invariant as datetime2 above.
		const bool time_ok = target.duckdb_type.id() == LogicalTypeId::TIME && src == PhysicalType::INT64 &&
							 source.id() == LogicalTypeId::TIME &&
							 target.max_length == tds::encoding::BCPRowEncoder::GetTimeByteSize(target.scale);
		const bool dto_ok = target.duckdb_type.id() == LogicalTypeId::TIMESTAMP_TZ && src == PhysicalType::INT64 &&
							target.max_length == dt2_written + 2;
		// A DATE source into a DATETIME2 target is an advertised conversion with a
		// kernel of its own (ScatterDateAsDt2) — the time field is zero at every
		// scale, so only the day number is computed.
		const bool date_to_ts_ok = target.duckdb_type.id() == LogicalTypeId::TIMESTAMP && src == PhysicalType::INT32 &&
								   source.id() == LogicalTypeId::DATE && target.max_length == dt2_written;
		const bool ts_ok = target.duckdb_type.id() != LogicalTypeId::DATE &&
						   target.duckdb_type.id() != LogicalTypeId::TIME &&
						   target.duckdb_type.id() != LogicalTypeId::TIMESTAMP_TZ && src == PhysicalType::INT64 &&
						   target.max_length == dt2_written;
		if (date_ok || ts_ok || time_ok || dto_ok || date_to_ts_ok) {
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

	// A source that does not store exactly those bytes is a CONVERSION — issue
	// #153's territory. It gets its own arm rather than the row path: the pair
	// (source width, target width) is fixed for the column, so the widening or
	// narrowing is resolved once here and the loop carries no type test.
	//
	// Integers and floats convert, each by its own arm — a float narrowing is a
	// precision loss the column asked for, an integer narrowing that does not fit
	// is a different number and is refused. BOOLEAN is one byte by definition, so
	// a mismatch there means malformed metadata and keeps the row path.
	const idx_t src_size = GetTypeIdSize(source.InternalType());
	if (src_size != want) {
		const bool int_target =
			target.duckdb_type.id() == LogicalTypeId::TINYINT || target.duckdb_type.id() == LogicalTypeId::SMALLINT ||
			target.duckdb_type.id() == LogicalTypeId::INTEGER || target.duckdb_type.id() == LogicalTypeId::BIGINT;
		const PhysicalType sp = source.InternalType();
		const bool int_source = sp == PhysicalType::INT8 || sp == PhysicalType::INT16 || sp == PhysicalType::INT32 ||
								sp == PhysicalType::INT64;
		if (int_target && int_source) {
			ops.arm = ScatterArm::IntConvert;
			ops.wire_width = want;
			ops.src_width = static_cast<uint8_t>(src_size);
			return ops;
		}
		const bool float_target =
			target.duckdb_type.id() == LogicalTypeId::FLOAT || target.duckdb_type.id() == LogicalTypeId::DOUBLE;
		const bool float_source = sp == PhysicalType::FLOAT || sp == PhysicalType::DOUBLE;
		if (float_target && float_source) {
			ops.arm = ScatterArm::FloatConvert;
			ops.wire_width = want;
			ops.src_width = static_cast<uint8_t>(src_size);
			return ops;
		}
		ops.arm = ScatterArm::RowFallback;
		ops.wire_width = want;
		return ops;
	}

	// Equal width is NOT enough to copy the bytes across: signedness has to agree
	// too. `UBIGINT -> bigint` is 8 bytes either side, so this took DirectCopy and
	// memcpy'd 18446744073709551615 into a signed column as -1 — while the row
	// path refuses the same value by range. UINTEGER -> int and USMALLINT ->
	// smallint are the same shape, and IsTypeCompatible advertises all of them.
	//
	// Send them to the row path rather than to IntConvert: IntConvert's check is
	// `static_cast<SRC>(static_cast<DST>(v)) != v`, a round-trip that a same-width
	// unsigned/signed pair passes for every value, so it would catch nothing.
	const PhysicalType src_phys = source.InternalType();
	const bool unsigned_source = src_phys == PhysicalType::UINT8 || src_phys == PhysicalType::UINT16 ||
								 src_phys == PhysicalType::UINT32 || src_phys == PhysicalType::UINT64;
	// TINYINT is deliberately NOT in this list. SQL Server's `tinyint` is the one
	// UNSIGNED integer it has — 0..255 — and a target column of that type is
	// carried here as LogicalTypeId::TINYINT only because that is the 1-byte
	// stand-in. A UTINYINT source is therefore an exact match, not a
	// reinterpretation, and refusing it costs far more than the guard protects:
	// the catalog maps SQL Server `tinyint` to UTINYINT, so ONE such column in an
	// mssql -> mssql load sent the ENTIRE chunk row-major.
	//
	// The pairs that stay refused are the ones where the unsigned source really
	// can exceed a SIGNED target: USMALLINT -> smallint, UINTEGER -> int,
	// UBIGINT -> bigint. Those cannot be caught by the conversion arm's
	// round-trip test, since every value of a same-width unsigned/signed pair
	// passes it.
	const bool signed_int_target = target.duckdb_type.id() == LogicalTypeId::SMALLINT ||
								   target.duckdb_type.id() == LogicalTypeId::INTEGER ||
								   target.duckdb_type.id() == LogicalTypeId::BIGINT;
	if (unsigned_source && signed_int_target) {
		ops.arm = ScatterArm::RowFallback;
		ops.wire_width = want;
		return ops;
	}

	// The mirror image, and the reason the byte widths agreeing is not enough on
	// its own: a SIGNED one-byte source into SQL Server's UNSIGNED `tinyint`.
	// Copying the byte put -1 on the server as 255 — silently, on both paths,
	// because the row encoder had an exact-width fast path that skipped its own
	// range check.
	//
	// It goes the row path rather than being refused here, so the refusal is by
	// VALUE and not by type: a TINYINT column holding only 0..127 loads exactly
	// as before, and only a negative errors. That matters because the columnar
	// resolver decides per column, before any data — refusing here would reject
	// loads that are entirely in range.
	if (src_phys == PhysicalType::INT8 && target.duckdb_type.id() == LogicalTypeId::UTINYINT) {
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
