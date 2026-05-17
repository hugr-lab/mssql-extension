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

#include "copy/target_resolver.hpp"
#include "dml/insert/mssql_value_serializer.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/vector.hpp"
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

template <typename T>
T GetVectorValue(Vector &vec, idx_t row_idx) {
	UnifiedVectorFormat format;
	vec.ToUnifiedFormat(1, format);
	auto data = UnifiedVectorFormat::GetData<T>(format);
	auto idx = format.sel->get_index(row_idx);
	return data[idx];
}

// Widen the value to hugeint based on DuckDB's PhysicalType. Mirrors
// the dispatch in BCPRowEncoder::EncodeRow DECIMAL arm.
hugeint_t WidenVectorToHugeint(Vector &vec, idx_t row_idx) {
	switch (vec.GetType().InternalType()) {
	case PhysicalType::INT16:
		return hugeint_t(GetVectorValue<int16_t>(vec, row_idx));
	case PhysicalType::INT32:
		return hugeint_t(GetVectorValue<int32_t>(vec, row_idx));
	case PhysicalType::INT64:
		return hugeint_t(GetVectorValue<int64_t>(vec, row_idx));
	case PhysicalType::INT128:
		return GetVectorValue<hugeint_t>(vec, row_idx);
	default:
		throw InternalException("codec::decimal::EncodeToBcp: unexpected PhysicalType for DECIMAL");
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

void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf) {
	hugeint_t value = WidenVectorToHugeint(in, row);
	tds::encoding::BCPRowEncoder::EncodeDecimal(buf, value, col.precision, col.scale);
}

void EncodeToBcp(const Value &value, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf) {
	// MSSQLValueSerializer::SerializeFromVector goes through Value::GetValue<hugeint_t>(),
	// which loses scale information for sub-INT128 PhysicalTypes. The legacy
	// BCPRowEncoder::EncodeValue DECIMAL arm did the same. Keep that behavior
	// here for parity — the dispatcher always reaches the Vector overload in
	// practice (BCPRowEncoder::EncodeRow path).
	tds::encoding::BCPRowEncoder::EncodeDecimal(buf, value.GetValue<hugeint_t>(), col.precision, col.scale);
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

std::string RenderAsString(const std::vector<uint8_t> &bytes, uint8_t precision, uint8_t scale) {
	(void)precision;  // SerializeDecimal does not need precision for fixed-point rendering.
	hugeint_t int_value = tds::encoding::DecimalEncoding::ConvertDecimal(bytes.data(), bytes.size());
	return MSSQLValueSerializer::SerializeDecimal(int_value, /*width*/ 38, scale);
}

std::string RenderMoneyAsString(const std::vector<uint8_t> &bytes) {
	// SQL Server MONEY is 8 bytes (value × 10000, scale 4). SMALLMONEY is 4 bytes
	// (same scaling). Both map to DECIMAL(*, 4) for rendering.
	hugeint_t int_value;
	if (bytes.size() == 8) {
		int_value = tds::encoding::DecimalEncoding::ConvertMoney(bytes.data());
	} else if (bytes.size() == 4) {
		int_value = tds::encoding::DecimalEncoding::ConvertSmallMoney(bytes.data());
	} else {
		throw InvalidInputException("codec::decimal::RenderMoneyAsString: unexpected wire length %zu", bytes.size());
	}
	return MSSQLValueSerializer::SerializeDecimal(int_value, /*width*/ 19, /*scale*/ 4);
}

}  // namespace decimal
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
