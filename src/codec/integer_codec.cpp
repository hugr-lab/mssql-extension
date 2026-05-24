//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/integer_codec.cpp
//
// Integer family implementation. See codec/integer_codec.hpp.
//
// Behavior parity:
//   - DecodeFromTds mirrors TypeConverter::ConvertInteger (size-dispatched
//     on bytes.size(): 1->u8, 2->i16, 4->i32, 8->i64).
//   - EncodeToBcp mirrors BCPRowEncoder Integer arms for TINYINT..UBIGINT
//     (UBIGINT delegates to BCPRowEncoder::EncodeDecimal). HUGEINT and
//     UHUGEINT BCP encoding lands with the Decimal family migration; for
//     now they throw NotImplementedException to preserve the pre-spec-045
//     default-arm behavior.
//   - FormatSqlLiteral produces identical output in Filter and InsertValues
//     contexts (FR-020 (b) — HUGEINT correctness fix; previously Filter
//     fell through filter_encoder's VARCHAR default arm and rendered
//     HUGEINT as N'<digits>').
//   - FormatDdlTypeName is byte-identical in CreateTable and CtasCreateTable
//     contexts (FR-025 / FR-028 — DDL unification). HUGEINT and UHUGEINT
//     now map to DECIMAL(38,0) in both contexts; previously CTAS threw.
//===----------------------------------------------------------------------===//

#include "codec/integer_codec.hpp"

#include "copy/target_resolver.hpp"
#include "dml/insert/mssql_value_serializer.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "mssql_compat.hpp"
#include "tds/encoding/bcp_row_encoder.hpp"
#include "tds/encoding/type_converter.hpp"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace duckdb {
namespace mssql {
namespace codec {
namespace integer {

namespace {

template <typename T>
T GetVectorValue(Vector &vec, idx_t row_idx) {
	UnifiedVectorFormat format;
	vec.ToUnifiedFormat(1, format);
	auto data = UnifiedVectorFormat::GetData<T>(format);
	auto idx = format.sel->get_index(row_idx);
	return data[idx];
}

void AppendInt8Bcp(duckdb::vector<uint8_t> &buf, int8_t value) {
	buf.push_back(1);
	buf.push_back(static_cast<uint8_t>(value));
}

void AppendUInt8Bcp(duckdb::vector<uint8_t> &buf, uint8_t value) {
	buf.push_back(1);
	buf.push_back(value);
}

void AppendInt16Bcp(duckdb::vector<uint8_t> &buf, int16_t value) {
	buf.push_back(2);
	buf.push_back(static_cast<uint8_t>(value & 0xFF));
	buf.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void AppendInt32Bcp(duckdb::vector<uint8_t> &buf, int32_t value) {
	buf.push_back(4);
	for (int i = 0; i < 4; ++i) {
		buf.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
	}
}

void AppendInt64Bcp(duckdb::vector<uint8_t> &buf, int64_t value) {
	buf.push_back(8);
	for (int i = 0; i < 8; ++i) {
		buf.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
	}
}

}  // namespace

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row) {
	(void)col;	// size-dispatched
	switch (bytes.size()) {
	case 1:
		// SQL Server TINYINT is unsigned (0-255).
		FlatVector::GetData<uint8_t>(out)[row] = bytes[0];
		return;
	case 2: {
		int16_t v = 0;
		std::memcpy(&v, bytes.data(), 2);
		FlatVector::GetData<int16_t>(out)[row] = v;
		return;
	}
	case 4: {
		int32_t v = 0;
		std::memcpy(&v, bytes.data(), 4);
		FlatVector::GetData<int32_t>(out)[row] = v;
		return;
	}
	case 8: {
		int64_t v = 0;
		std::memcpy(&v, bytes.data(), 8);
		FlatVector::GetData<int64_t>(out)[row] = v;
		return;
	}
	default:
		throw InvalidInputException("codec::integer::DecodeFromTds: invalid integer length %zu", bytes.size());
	}
}

void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf) {
	switch (col.duckdb_type.id()) {
	case LogicalTypeId::TINYINT:
		AppendInt8Bcp(buf, GetVectorValue<int8_t>(in, row));
		return;
	case LogicalTypeId::UTINYINT:
		AppendUInt8Bcp(buf, GetVectorValue<uint8_t>(in, row));
		return;
	case LogicalTypeId::SMALLINT:
		AppendInt16Bcp(buf, GetVectorValue<int16_t>(in, row));
		return;
	case LogicalTypeId::USMALLINT:
		// USMALLINT (0-65535) widens to int32 to fit without overflow.
		AppendInt32Bcp(buf, static_cast<int32_t>(GetVectorValue<uint16_t>(in, row)));
		return;
	case LogicalTypeId::INTEGER:
		AppendInt32Bcp(buf, GetVectorValue<int32_t>(in, row));
		return;
	case LogicalTypeId::UINTEGER:
		// UINTEGER (0-4B) widens to int64 to fit without overflow.
		AppendInt64Bcp(buf, static_cast<int64_t>(GetVectorValue<uint32_t>(in, row)));
		return;
	case LogicalTypeId::BIGINT:
		AppendInt64Bcp(buf, GetVectorValue<int64_t>(in, row));
		return;
	case LogicalTypeId::UBIGINT: {
		// UBIGINT (0-18e18) uses DECIMAL(20,0) on the wire — SQL Server BIGINT is signed.
		// Two-argument hugeint_t(upper=0, lower=val) avoids sign issues when val > INT64_MAX.
		uint64_t val = GetVectorValue<uint64_t>(in, row);
		tds::encoding::BCPRowEncoder::EncodeDecimal(buf, hugeint_t(0, val), col.precision, col.scale);
		return;
	}
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
		// Deferred to spec-045 Decimal family migration (Phase 6 / T069).
		throw NotImplementedException("MSSQL: BCP encoding for %s is not yet implemented (spec 045 Phase 6)",
									  col.duckdb_type.ToString());
	default:
		throw NotImplementedException("codec::integer::EncodeToBcp: unsupported type %s", col.duckdb_type.ToString());
	}
}

void EncodeToBcp(const Value &value, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf) {
	switch (col.duckdb_type.id()) {
	case LogicalTypeId::TINYINT:
		AppendInt8Bcp(buf, value.GetValue<int8_t>());
		return;
	case LogicalTypeId::UTINYINT:
		AppendUInt8Bcp(buf, value.GetValue<uint8_t>());
		return;
	case LogicalTypeId::SMALLINT:
		AppendInt16Bcp(buf, value.GetValue<int16_t>());
		return;
	case LogicalTypeId::USMALLINT:
		AppendInt32Bcp(buf, static_cast<int32_t>(value.GetValue<uint16_t>()));
		return;
	case LogicalTypeId::INTEGER:
		AppendInt32Bcp(buf, value.GetValue<int32_t>());
		return;
	case LogicalTypeId::UINTEGER:
		AppendInt64Bcp(buf, static_cast<int64_t>(value.GetValue<uint32_t>()));
		return;
	case LogicalTypeId::BIGINT:
		AppendInt64Bcp(buf, value.GetValue<int64_t>());
		return;
	case LogicalTypeId::UBIGINT: {
		uint64_t val = value.GetValue<uint64_t>();
		tds::encoding::BCPRowEncoder::EncodeDecimal(buf, hugeint_t(0, val), col.precision, col.scale);
		return;
	}
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
		throw NotImplementedException("MSSQL: BCP encoding for %s is not yet implemented (spec 045 Phase 6)",
									  col.duckdb_type.ToString());
	default:
		throw NotImplementedException("codec::integer::EncodeToBcp(Value): unsupported type %s",
									  col.duckdb_type.ToString());
	}
}

std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx) {
	(void)ctx;	// Both Filter and InsertValues produce identical output (FR-020 (b)).
	if (v.IsNull()) {
		return "NULL";
	}
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
		return std::to_string(TinyIntValue::Get(v));
	case LogicalTypeId::SMALLINT:
		return std::to_string(SmallIntValue::Get(v));
	case LogicalTypeId::INTEGER:
		return std::to_string(IntegerValue::Get(v));
	case LogicalTypeId::BIGINT:
		return std::to_string(BigIntValue::Get(v));
	case LogicalTypeId::UTINYINT:
		return std::to_string(static_cast<int64_t>(UTinyIntValue::Get(v)));
	case LogicalTypeId::USMALLINT:
		return std::to_string(static_cast<int64_t>(USmallIntValue::Get(v)));
	case LogicalTypeId::UINTEGER:
		return std::to_string(static_cast<int64_t>(UIntegerValue::Get(v)));
	case LogicalTypeId::UBIGINT: {
		// Values > INT64_MAX render as CAST(N AS DECIMAL(20,0)); smaller values render as bare digits.
		// SQL Server BIGINT is signed, so unsigned values above INT64_MAX must use DECIMAL(20,0).
		uint64_t uval = UBigIntValue::Get(v);
		if (uval <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
			return std::to_string(uval);
		}
		return StringUtil::Format("CAST(%llu AS DECIMAL(20,0))", uval);
	}
	case LogicalTypeId::HUGEINT:
		return MSSQLValueSerializer::SerializeDecimal(HugeIntValue::Get(v), 38, 0);
	case LogicalTypeId::UHUGEINT:
		// UHUGEINT not currently produced by any DuckDB path that reaches Filter/InsertValues; keep
		// the arm for completeness with the Decimal family's overflow handling (FR-025).
		throw NotImplementedException("codec::integer::FormatSqlLiteral: UHUGEINT literal not yet implemented");
	default:
		throw NotImplementedException("codec::integer::FormatSqlLiteral: unsupported type %s", type.ToString());
	}
}

std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig &cfg, DdlContext ctx) {
	(void)cfg;
	(void)ctx;	// Output is byte-identical in CreateTable and CtasCreateTable (FR-025 / FR-028).
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::UTINYINT:
		// UTINYINT (0-255) fits in SQL Server TINYINT (also 0-255).
		return "TINYINT";
	case LogicalTypeId::SMALLINT:
		return "SMALLINT";
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::INTEGER:
		// USMALLINT widens to INT to fit the full range.
		return "INT";
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::BIGINT:
		// UINTEGER widens to BIGINT to fit the full range.
		return "BIGINT";
	case LogicalTypeId::UBIGINT:
		return "DECIMAL(20,0)";
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
		return "DECIMAL(38,0)";
	default:
		throw NotImplementedException("codec::integer::FormatDdlTypeName: unsupported type %s", type.ToString());
	}
}

size_t EstimateLiteralSize(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::UTINYINT:
		return 4;  // max 3 digits + sign
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::USMALLINT:
		return 6;  // max 5 digits + sign
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::UINTEGER:
		return 11;	// max 10 digits + sign
	case LogicalTypeId::BIGINT:
		return 20;	// max 19 digits + sign
	case LogicalTypeId::UBIGINT:
		// "CAST(18446744073709551615 AS DECIMAL(20,0))" = 43 chars; round up to 50 for safety.
		// Legacy MSSQLValueSerializer::EstimateSerializedSize undercounted at 40 (spec-045 fix).
		return 50;
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
		return 45;	// max 39 digits + sign
	default:
		throw NotImplementedException("codec::integer::EstimateLiteralSize: unsupported type %s", type.ToString());
	}
}

}  // namespace integer
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
