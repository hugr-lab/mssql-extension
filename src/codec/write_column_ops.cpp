//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/write_column_ops.cpp
//
// Per-column dispatch resolution for the BCP write path. See the header.
//===----------------------------------------------------------------------===//

#include "codec/write_column_ops.hpp"
#include "codec/sql_vector/vector_bytes.hpp"

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

//! The column keeps the row path, `wire_width` its payload width where that is fixed.
WriteColumnOps RowFallback(WriteColumnOps ops, uint32_t wire_width) {
	ops.arm = ScatterArm::RowFallback;
	ops.wire_width = wire_width;
	return ops;
}

//! The column takes the columnar scatter by `arm`.
WriteColumnOps Scatter(WriteColumnOps ops, ScatterArm arm, uint32_t wire_width) {
	ops.arm = arm;
	ops.wire_width = wire_width;
	return ops;
}

//! Spec 093 T3: a FLOAT[n] of the column's n; anything else is the row path's to refuse by name.
WriteColumnOps ResolveVector(const LogicalType &source, const mssql::BCPColumnMetadata &target, WriteColumnOps ops) {
	ops.kind = WireKind::VariableUShort;
	const bool fits = source.id() == LogicalTypeId::ARRAY &&
					  ArrayType::GetChildType(source).id() == LogicalTypeId::FLOAT &&
					  ArrayType::GetSize(source) == target.vector_dims;
	ops.arm = fits ? ScatterArm::Vector : ScatterArm::RowFallback;
	ops.wire_width = static_cast<uint32_t>(sql_vector::ValueBytes(target.vector_dims, target.vector_element_type));
	return ops;
}

//! A value whose width depends on the data: nvarchar(n) and the UTF-8 varchar(n) form, varbinary(n), and
//! their MAX (PLP) forms -- one arm for all of them, the plan owning the framing difference.
//!
//! The SOURCE must actually be a string, not merely the target: spec 045
//! FR-026 sends an INTERVAL as NVARCHAR, so the target is a string wire
//! form over a vector that stores interval_t. Reading it as string_t trips
//! "Expected unified vector format of type VARCHAR, but found type
//! INTERVAL". Any such render-as-text source keeps the row path, which
//! formats it first.
//!
//! varbinary shares the framing exactly and needs no conversion at all, so it rides the same arm. Its
//! source is a BLOB, which DuckDB also stores as string_t.
WriteColumnOps ResolveVariable(const LogicalType &source, const mssql::BCPColumnMetadata &target, WriteColumnOps ops) {
	const bool string_or_binary_target = target.tds_type_token == tds::TDS_TYPE_NVARCHAR ||
										 target.tds_type_token == tds::TDS_TYPE_BIGVARCHAR ||
										 target.tds_type_token == tds::TDS_TYPE_BIGVARBINARY;
	const bool string_source = source.InternalType() == PhysicalType::VARCHAR;
	ops.arm = (string_or_binary_target && string_source) ? ScatterArm::VarString : ScatterArm::RowFallback;
	return ops;
}

//! A target that travels as DECIMAL. UBIGINT and HUGEINT belong here because they TRAVEL as DECIMAL —
//! generated metadata keeps the source's duckdb_type (precision 20/38 set
//! beside it), and gating on DECIMAL alone sent both whole-table row-major
//! (spec 064 D4). The kernel takes UINT64 via an explicit widening; a
//! uint64's maximum is below 10^20-1, so the range check never fires.
WriteColumnOps ResolveDecimal(const LogicalType &source, const mssql::BCPColumnMetadata &target, WriteColumnOps ops) {
	const PhysicalType src = source.InternalType();
	const bool src_ok = src == PhysicalType::INT16 || src == PhysicalType::INT32 || src == PhysicalType::INT64 ||
						src == PhysicalType::INT128 || src == PhysicalType::UINT64;
	const uint32_t width = tds::encoding::BCPRowEncoder::GetDecimalByteSize(target.precision);
	if (src_ok && target.max_length == width) {
		return Scatter(ops, ScatterArm::Decimal, width);
	}
	return RowFallback(ops, target.max_length);
}

bool IsTemporalTarget(LogicalTypeId id) {
	switch (id) {
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIMESTAMP_TZ:
		return true;
	default:
		return false;
	}
}

//! Whether the datetime kernel reads this source into this temporal target. The source must be the
//! matching temporal storage: DATE is int32 days, the TIMESTAMP variants int64 ticks. Anything else is a
//! conversion and goes the row path, which performs it.
//!
//! The width the kernel WRITES must equal the width the chunk was sized
//! from, or every later column in the row lands where the server is not
//! looking. The row path never had this constraint because it appends —
//! whatever it writes IS the layout. The scatter reserves first.
//!
//! `datetime`, `datetime2` and `smalldatetime` all declare
//! TDS_TYPE_DATETIME2 on the wire and share a `duckdb_type`, so the width
//! is the only thing that tells them apart — which is exactly why it is
//! checked rather than assumed.
//!
//! SQLServerTypeMaxLength now reports what is actually SENT for all of
//! them (it used to return a constant 8 for every datetime2, so only
//! datetime2(7) ever matched), which is safe because COLMETADATA for this
//! token carries the SCALE and no length at all.
//! The sources the datetime kernel reads as ticks: codec::datetime::TicksPerSecondFor's types.
bool IsTimestampSource(LogicalTypeId id) {
	return id == LogicalTypeId::TIMESTAMP || id == LogicalTypeId::TIMESTAMP_MS || id == LogicalTypeId::TIMESTAMP_NS ||
		   id == LogicalTypeId::TIMESTAMP_SEC || id == LogicalTypeId::TIMESTAMP_TZ;
}

bool DatetimeKernelFits(const LogicalType &source, const mssql::BCPColumnMetadata &target) {
	const LogicalTypeId to = target.duckdb_type.id();
	const PhysicalType src = source.InternalType();
	const uint32_t time_written = tds::encoding::BCPRowEncoder::GetTimeByteSize(target.scale);
	const uint32_t dt2_written = time_written + 3;
	if (to == LogicalTypeId::DATE) {
		return src == PhysicalType::INT32 && source.id() == LogicalTypeId::DATE && target.max_length == 3;
	}
	// TIME has no date field, DATETIMEOFFSET adds a 2-byte offset, and both
	// must satisfy the same width invariant as datetime2.
	if (to == LogicalTypeId::TIME) {
		return src == PhysicalType::INT64 && source.id() == LogicalTypeId::TIME && target.max_length == time_written;
	}
	// A datetime2 or datetimeoffset target takes the timestamp types, whose unit the kernel asks the source's
	// type for. Storing eight bytes is not enough: a BIGINT, a TIME or a DECIMAL(18) does too, and the kernel
	// answers one of those with an InternalException where the row path converts it or refuses it by name.
	if (to == LogicalTypeId::TIMESTAMP_TZ) {
		return IsTimestampSource(source.id()) && target.max_length == dt2_written + 2;
	}
	if (target.max_length != dt2_written) {
		return false;
	}
	// A DATE source into a DATETIME2 target is an advertised conversion with a
	// kernel of its own (ScatterDateAsDt2) — the time field is zero at every
	// scale, so only the day number is computed.
	const bool date_into_timestamp =
		to == LogicalTypeId::TIMESTAMP && src == PhysicalType::INT32 && source.id() == LogicalTypeId::DATE;
	return date_into_timestamp || IsTimestampSource(source.id());
}

WriteColumnOps ResolveTemporal(const LogicalType &source, const mssql::BCPColumnMetadata &target, WriteColumnOps ops) {
	if (DatetimeKernelFits(source, target)) {
		return Scatter(ops, ScatterArm::Datetime, target.max_length);
	}
	return RowFallback(ops, target.max_length);
}

WriteColumnOps ResolveGuid(const LogicalType &source, const mssql::BCPColumnMetadata &target, WriteColumnOps ops) {
	if (source.id() == LogicalTypeId::UUID && target.max_length == 16) {
		return Scatter(ops, ScatterArm::Guid, 16);
	}
	return RowFallback(ops, target.max_length);
}

bool IsSignedInteger(PhysicalType type) {
	return type == PhysicalType::INT8 || type == PhysicalType::INT16 || type == PhysicalType::INT32 ||
		   type == PhysicalType::INT64;
}

bool IsUnsignedInteger(PhysicalType type) {
	return type == PhysicalType::UINT8 || type == PhysicalType::UINT16 || type == PhysicalType::UINT32 ||
		   type == PhysicalType::UINT64;
}

//! A source of another width than the target's `want` bytes is a CONVERSION — issue
//! #153's territory. It gets its own arm rather than the row path: the pair
//! (source width, target width) is fixed for the column, so the widening or
//! narrowing is resolved once here and the loop carries no type test.
//!
//! Integers and floats convert, each by its own arm — a float narrowing is a
//! precision loss the column asked for, an integer narrowing that does not fit
//! is a different number and is refused. BOOLEAN is one byte by definition, so
//! a mismatch there means malformed metadata and keeps the row path.
WriteColumnOps ResolveConversion(const LogicalType &source, LogicalTypeId to, uint32_t want, WriteColumnOps ops) {
	const PhysicalType sp = source.InternalType();
	ops.src_width = static_cast<uint8_t>(GetTypeIdSize(sp));
	const bool int_target = to == LogicalTypeId::TINYINT || to == LogicalTypeId::SMALLINT ||
							to == LogicalTypeId::INTEGER || to == LogicalTypeId::BIGINT;
	if (int_target && IsSignedInteger(sp)) {
		return Scatter(ops, ScatterArm::IntConvert, want);
	}
	const bool float_target = to == LogicalTypeId::FLOAT || to == LogicalTypeId::DOUBLE;
	const bool float_source = sp == PhysicalType::FLOAT || sp == PhysicalType::DOUBLE;
	if (float_target && float_source) {
		return Scatter(ops, ScatterArm::FloatConvert, want);
	}
	ops.src_width = 0;
	return RowFallback(ops, want);
}

//! Whether copying the source's bytes would put another number in the target: equal width is NOT enough,
//! signedness has to agree too.
//!
//! `UBIGINT -> bigint` is 8 bytes either side, so this took DirectCopy and
//! memcpy'd 18446744073709551615 into a signed column as -1 — while the row
//! path refuses the same value by range. UINTEGER -> int and USMALLINT ->
//! smallint are the same shape, and IsTypeCompatible advertises all of them.
//! They go to the row path rather than to IntConvert: IntConvert's check is
//! `static_cast<SRC>(static_cast<DST>(v)) != v`, a round-trip that a same-width
//! unsigned/signed pair passes for every value, so it would catch nothing.
//!
//! TINYINT is deliberately NOT a signed target here. SQL Server's `tinyint` is the one
//! UNSIGNED integer it has — 0..255 — and a target column of that type is
//! carried here as LogicalTypeId::TINYINT only because that is the 1-byte
//! stand-in. A UTINYINT source is therefore an exact match, not a
//! reinterpretation, and refusing it costs far more than the guard protects:
//! the catalog maps SQL Server `tinyint` to UTINYINT, so ONE such column in an
//! mssql -> mssql load sent the ENTIRE chunk row-major.
//!
//! The mirror image: a SIGNED one-byte source into SQL Server's UNSIGNED `tinyint`.
//! Copying the byte put -1 on the server as 255 — silently, on both paths,
//! because the row encoder had an exact-width fast path that skipped its own
//! range check. It goes the row path rather than being refused here, so the refusal is by
//! VALUE and not by type: a TINYINT column holding only 0..127 loads exactly
//! as before, and only a negative errors. That matters because the columnar
//! resolver decides per column, before any data — refusing here would reject
//! loads that are entirely in range.
bool SignednessDiffers(PhysicalType source, LogicalTypeId to) {
	const bool signed_int_target =
		to == LogicalTypeId::SMALLINT || to == LogicalTypeId::INTEGER || to == LogicalTypeId::BIGINT;
	if (IsUnsignedInteger(source) && signed_int_target) {
		return true;
	}
	return source == PhysicalType::INT8 && to == LogicalTypeId::UTINYINT;
}

//! The types whose bytes can be copied: bit, the integers, real and float.
WriteColumnOps ResolveNumber(const LogicalType &source, const mssql::BCPColumnMetadata &target, WriteColumnOps ops) {
	const LogicalTypeId to = target.duckdb_type.id();
	const uint32_t want = DirectCopyTargetWidth(target.duckdb_type);
	if (want == 0) {
		// A fixed-width type with no kernel here. Resolvable, row path for now.
		return RowFallback(ops, target.max_length);
	}
	// COLMETADATA must agree with the type it declared, or the scatter would
	// write a different number of bytes than the server is expecting.
	if (target.max_length != want) {
		return RowFallback(ops, target.max_length);
	}
	const PhysicalType src = source.InternalType();
	if (GetTypeIdSize(src) != want) {
		return ResolveConversion(source, to, want, ops);
	}
	if (SignednessDiffers(src, to)) {
		return RowFallback(ops, want);
	}
	return Scatter(ops, DirectCopyArm(want), want);
}

}  // namespace

WriteColumnOps ResolveWriteColumnOps(const LogicalType &source, const mssql::BCPColumnMetadata &target) {
	WriteColumnOps ops;
	ops.kind = target.IsPLPType() ? WireKind::Plp
								  : (target.IsVariableLengthUSHORT() ? WireKind::VariableUShort : WireKind::Fixed);
	if (target.IsVector()) {
		return ResolveVector(source, target, ops);
	}
	if (ops.kind != WireKind::Fixed) {
		return ResolveVariable(source, target, ops);
	}
	// Fixed-width families that need a transformation rather than a copy. Each is
	// still one width per column, so the stride model holds; only the arm differs.
	const LogicalTypeId to = target.duckdb_type.id();
	if (to == LogicalTypeId::DECIMAL || to == LogicalTypeId::UBIGINT || to == LogicalTypeId::HUGEINT) {
		return ResolveDecimal(source, target, ops);
	}
	if (IsTemporalTarget(to)) {
		return ResolveTemporal(source, target, ops);
	}
	if (to == LogicalTypeId::UUID) {
		return ResolveGuid(source, target, ops);
	}
	return ResolveNumber(source, target, ops);
}

}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
