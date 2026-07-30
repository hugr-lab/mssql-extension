//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/decimal_codec.cpp
//
// Decimal family implementation. See codec/decimal_codec.hpp.
//
// Behavior parity (vs pre-spec-045 baseline):
//   - DecodeFromTds mirrors TypeConverter::ConvertDecimal — PhysicalType
//     dispatch on column.precision (≤4 → INT16, ≤9 → INT32, ≤18 → INT64,
//     >18 → INT128). The unscaled hugeint produced by
//     DecimalEncoding::ConvertDecimal is truncated to the appropriate
//     storage width.
//   - EncodeToBcp mirrors BCPRowEncoder::EncodeRow / EncodeValue DECIMAL
//     arms — widens to hugeint and delegates to
//     BCPRowEncoder::EncodeDecimal (which produces the fixed precision-
//     bucket wire layout: 5/9/13/17 bytes).
//   - FormatSqlLiteral unifies on MSSQLValueSerializer::SerializeDecimal
//     (FR-022) for both LiteralContext values — pre-spec-045 the Filter
//     path used Value::ToString() which could diverge on edge cases.
//   - FormatDdlTypeName produces "DECIMAL(p,s)" with p ≤ 38 clamp and
//     s ≤ p clamp — byte-identical in both DdlContext values.
//   - EstimateLiteralSize: 45 — matches the pre-spec-045 upper bound
//     (sign + 38 digits + dot + padding).
//
// Also defines codec::decimal::RenderAsString — a public helper used by
// the issue-#89 VARCHAR-fallback path in TypeConverter::ConvertValue.
// Same rendering as the literal path; reused so the fallback string is
// deterministic and round-trippable.
//===----------------------------------------------------------------------===//

#include "codec/decimal_codec.hpp"

#include "codec/vector_format.hpp"
#include "copy/target_resolver.hpp"
#include "dml/insert/mssql_value_serializer.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "mssql_compat.hpp"
#include "tds/encoding/bcp_row_encoder.hpp"
#include "tds/encoding/decimal_encoding.hpp"
#include "tds/encoding/type_converter.hpp"

#include <cstdint>
#include <cstring>
#include <string>

namespace duckdb {
namespace mssql {
namespace codec {
namespace decimal {

namespace {

// Widen the value to hugeint based on DuckDB's PhysicalType. Mirrors
// the dispatch in BCPRowEncoder::EncodeRow DECIMAL arm.
hugeint_t WidenVectorToHugeint(Vector &vec, const UnifiedVectorFormat &fmt, idx_t row_idx) {
	switch (vec.GetType().InternalType()) {
	case PhysicalType::INT16:
		return hugeint_t(FormatValue<int16_t>(fmt, row_idx));
	case PhysicalType::INT32:
		return hugeint_t(FormatValue<int32_t>(fmt, row_idx));
	case PhysicalType::INT64:
		return hugeint_t(FormatValue<int64_t>(fmt, row_idx));
	case PhysicalType::INT128:
		return FormatValue<hugeint_t>(fmt, row_idx);
	default:
		throw InternalException("codec::decimal::EncodeToBcp: unexpected PhysicalType for DECIMAL");
	}
}

// #177: SQL Server rejects a row whose mantissa exceeds the declared precision
// with a mid-batch "Invalid data for type numeric" that aborts the whole BCP
// stream. Guard client-side instead: |mantissa| must fit in `precision` digits.
// Reached e.g. when a HUGEINT source feeds a DECIMAL(38,0) target column
// (COPY CREATE_TABLE reads the created table's metadata back, so the column
// dispatches through the Decimal family, not Integer).
void CheckMantissaFitsPrecision(const hugeint_t &value, const mssql::BCPColumnMetadata &col) {
	if (col.precision == 0 || col.precision > 38) {
		return;	 // malformed metadata — let the server decide
	}
	// Bound both ends directly rather than negating `value` first: Hugeint::Negate
	// on HUGEINT min (-2^127) overflows int128, so an abs-then-compare guard let
	// that value slip (or threw a raw overflow). +/-(10^precision - 1) are both
	// representable for precision <= 38, so the check is exact for every input.
	const hugeint_t max = Hugeint::POWERS_OF_TEN[col.precision] - 1;
	const hugeint_t min = Hugeint::Negate(max);
	if (Hugeint::GreaterThan(value, max) || Hugeint::GreaterThan(min, value)) {
		throw InvalidInputException(
			"MSSQL: DECIMAL value (mantissa %s, scale %d) in column \"%s\" is out of range for DECIMAL(%d,%d)",
			Hugeint::ToString(value), static_cast<int>(col.scale), col.name, static_cast<int>(col.precision),
			static_cast<int>(col.scale));
	}
}

}  // namespace

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row) {
	hugeint_t int_value = tds::encoding::DecimalEncoding::ConvertDecimal(bytes.data(), bytes.size());

	// DuckDB DECIMAL uses different storage based on precision.
	// Mirrors the dispatch in TypeConverter::ConvertDecimal.
	if (col.precision <= 4) {
		FlatVector::GetData<int16_t>(out)[row] = static_cast<int16_t>(int_value.lower);
	} else if (col.precision <= 9) {
		FlatVector::GetData<int32_t>(out)[row] = static_cast<int32_t>(int_value.lower);
	} else if (col.precision <= 18) {
		FlatVector::GetData<int64_t>(out)[row] = static_cast<int64_t>(int_value.lower);
	} else {
		FlatVector::GetData<hugeint_t>(out)[row] = int_value;
	}
}

void DecodeChunkFromStaging(const staging::ColumnStaging &st, idx_t count, const tds::ColumnMetadata &col,
							Vector &out) {
	const uint8_t *const base = st.buffer.data();
	const uint32_t stride = st.stride;
	// Total over any byte pattern — a sign byte and a little-endian mantissa are
	// just loads — so the loop runs over NULL rows too and stays branch-free.
	if (col.precision <= 4) {
		int16_t *result = FlatVector::GetData<int16_t>(out);
		for (idx_t row = 0; row < count; row++) {
			result[row] =
				static_cast<int16_t>(tds::encoding::DecimalEncoding::ConvertDecimal(base + row * stride, stride).lower);
		}
	} else if (col.precision <= 9) {
		int32_t *result = FlatVector::GetData<int32_t>(out);
		for (idx_t row = 0; row < count; row++) {
			result[row] =
				static_cast<int32_t>(tds::encoding::DecimalEncoding::ConvertDecimal(base + row * stride, stride).lower);
		}
	} else if (col.precision <= 18) {
		int64_t *result = FlatVector::GetData<int64_t>(out);
		for (idx_t row = 0; row < count; row++) {
			result[row] =
				static_cast<int64_t>(tds::encoding::DecimalEncoding::ConvertDecimal(base + row * stride, stride).lower);
		}
	} else {
		hugeint_t *result = FlatVector::GetData<hugeint_t>(out);
		for (idx_t row = 0; row < count; row++) {
			result[row] = tds::encoding::DecimalEncoding::ConvertDecimal(base + row * stride, stride);
		}
	}
}

void EncodeToBcp(Vector &in, const UnifiedVectorFormat &fmt, idx_t row, const mssql::BCPColumnMetadata &col,
				 duckdb::vector<uint8_t> &buf) {
	hugeint_t value = WidenVectorToHugeint(in, fmt, row);
	CheckMantissaFitsPrecision(value, col);
	tds::encoding::BCPRowEncoder::EncodeDecimal(buf, value, col.precision, col.scale);
}

void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf) {
	EncodeToBcpViaFormat(EncodeToBcp, in, row, col, buf);
}

void EncodeToBcp(const Value &value, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf) {
	// MSSQLValueSerializer::SerializeFromVector goes through Value::GetValue<hugeint_t>(),
	// which loses scale information for sub-INT128 PhysicalTypes. The legacy
	// BCPRowEncoder::EncodeValue DECIMAL arm did the same. Keep that behavior
	// here for parity — the dispatcher always reaches the Vector overload in
	// practice (BCPRowEncoder::EncodeRow path).
	hugeint_t mantissa = value.GetValue<hugeint_t>();
	CheckMantissaFitsPrecision(mantissa, col);
	tds::encoding::BCPRowEncoder::EncodeDecimal(buf, mantissa, col.precision, col.scale);
}

std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext /*ctx*/) {
	if (v.IsNull()) {
		return "NULL";
	}
	if (type.id() == LogicalTypeId::HUGEINT) {
		// FR-025: HUGEINT routes through Decimal as if DECIMAL(38,0).
		return MSSQLValueSerializer::SerializeDecimal(HugeIntValue::Get(v), 38, 0);
	}
	if (type.id() != LogicalTypeId::DECIMAL) {
		throw InternalException("codec::decimal::FormatSqlLiteral: unexpected LogicalType '%s'", type.ToString());
	}
	uint8_t width = DecimalType::GetWidth(type);
	uint8_t scale = DecimalType::GetScale(type);
	switch (type.InternalType()) {
	case PhysicalType::INT16:
		return MSSQLValueSerializer::SerializeDecimal(hugeint_t(v.GetValueUnsafe<int16_t>()), width, scale);
	case PhysicalType::INT32:
		return MSSQLValueSerializer::SerializeDecimal(hugeint_t(v.GetValueUnsafe<int32_t>()), width, scale);
	case PhysicalType::INT64:
		return MSSQLValueSerializer::SerializeDecimal(hugeint_t(v.GetValueUnsafe<int64_t>()), width, scale);
	case PhysicalType::INT128:
		return MSSQLValueSerializer::SerializeDecimal(v.GetValueUnsafe<hugeint_t>(), width, scale);
	default:
		throw InternalException("codec::decimal::FormatSqlLiteral: unexpected PhysicalType for DECIMAL");
	}
}

std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig & /*cfg*/, DdlContext /*ctx*/) {
	if (type.id() == LogicalTypeId::HUGEINT || type.id() == LogicalTypeId::UHUGEINT) {
		// FR-025 — HUGEINT/UHUGEINT map to DECIMAL(38,0) in both DDL contexts.
		return "DECIMAL(38,0)";
	}
	if (type.id() != LogicalTypeId::DECIMAL) {
		throw InternalException("codec::decimal::FormatDdlTypeName: unexpected LogicalType '%s'", type.ToString());
	}
	uint8_t width;
	uint8_t scale;
	type.GetDecimalProperties(width, scale);
	// SQL Server: precision 1-38, scale 0-precision (FR-017 clamp).
	uint8_t precision = width > 38 ? 38 : width;
	if (scale > precision) {
		scale = precision;
	}
	return StringUtil::Format("DECIMAL(%d,%d)", precision, scale);
}

size_t EstimateLiteralSize(const LogicalType & /*type*/) {
	// max precision 38 + scale + sign + decimal point.
	return 45;
}

std::string RenderAsString(const uint8_t *bytes, size_t size, uint8_t precision, uint8_t scale) {
	(void)precision;  // SerializeDecimal does not need precision for fixed-point rendering.
	hugeint_t int_value = tds::encoding::DecimalEncoding::ConvertDecimal(bytes, size);
	return MSSQLValueSerializer::SerializeDecimal(int_value, /*width*/ 38, scale);
}

std::string RenderAsString(const std::vector<uint8_t> &bytes, uint8_t precision, uint8_t scale) {
	return RenderAsString(bytes.data(), bytes.size(), precision, scale);
}

std::string RenderMoneyAsString(const uint8_t *bytes, size_t size) {
	// SQL Server MONEY is 8 bytes (value × 10000, scale 4). SMALLMONEY is 4 bytes
	// (same scaling). Both map to DECIMAL(*, 4) for rendering.
	hugeint_t int_value;
	if (size == 8) {
		int_value = tds::encoding::DecimalEncoding::ConvertMoney(bytes);
	} else if (size == 4) {
		int_value = tds::encoding::DecimalEncoding::ConvertSmallMoney(bytes);
	} else {
		throw InvalidInputException("codec::decimal::RenderMoneyAsString: unexpected wire length %zu", size);
	}
	return MSSQLValueSerializer::SerializeDecimal(int_value, /*width*/ 19, /*scale*/ 4);
}

std::string RenderMoneyAsString(const std::vector<uint8_t> &bytes) {
	return RenderMoneyAsString(bytes.data(), bytes.size());
}

}  // namespace decimal
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
