#include "dml/insert/mssql_value_serializer.hpp"
#include "codec/literal_format.hpp"
#include "codec/string_codec.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Identifier and String Escaping
//===----------------------------------------------------------------------===//

string MSSQLValueSerializer::EscapeIdentifier(const string &name) {
	// T-SQL bracket quoting: ] becomes ]]
	string result;
	result.reserve(name.size() + 2);
	result.push_back('[');
	for (char c : name) {
		if (c == ']') {
			result.push_back(']');
		}
		result.push_back(c);
	}
	result.push_back(']');
	return result;
}

string MSSQLValueSerializer::EscapeString(const string &value) {
	// SQL string escaping: ' becomes ''
	string result;
	result.reserve(value.size());
	for (char c : value) {
		if (c == '\'') {
			result.push_back('\'');
		}
		result.push_back(c);
	}
	return result;
}

//===----------------------------------------------------------------------===//
// Decimal Serialization
//===----------------------------------------------------------------------===//

string MSSQLValueSerializer::SerializeDecimal(const hugeint_t &value, uint8_t width, uint8_t scale) {
	// Convert hugeint to string preserving scale
	string unscaled = Hugeint::ToString(value);

	// Handle negative numbers
	bool negative = false;
	size_t start = 0;
	if (!unscaled.empty() && unscaled[0] == '-') {
		negative = true;
		start = 1;
	}

	string digits = unscaled.substr(start);

	// Pad with leading zeros if needed
	while (digits.size() <= scale) {
		digits = "0" + digits;
	}

	// Insert decimal point
	string result;
	if (negative) {
		result = "-";
	}

	if (scale == 0) {
		result += digits;
	} else {
		size_t int_len = digits.size() - scale;
		result += digits.substr(0, int_len);
		result += ".";
		result += digits.substr(int_len);
	}

	return result;
}

//===----------------------------------------------------------------------===//
// Main Entry Points
//===----------------------------------------------------------------------===//

string MSSQLValueSerializer::Serialize(const Value &value, const LogicalType &target_type) {
	// Handle NULL
	if (value.IsNull()) {
		return "NULL";
	}

	// Dispatch based on the value's internal type
	auto &type = value.type();
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return mssql::codec::FormatSqlLiteral(value, type, mssql::codec::LiteralContext::InsertValues);

	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
		return mssql::codec::FormatSqlLiteral(value, type, mssql::codec::LiteralContext::InsertValues);

	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		return mssql::codec::FormatSqlLiteral(value, type, mssql::codec::LiteralContext::InsertValues);

	case LogicalTypeId::DECIMAL:
		return mssql::codec::FormatSqlLiteral(value, type, mssql::codec::LiteralContext::InsertValues);

	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::INTERVAL:
		return mssql::codec::FormatSqlLiteral(value, type, mssql::codec::LiteralContext::InsertValues);

	case LogicalTypeId::BLOB:
	case LogicalTypeId::GEOMETRY:
		return mssql::codec::FormatSqlLiteral(value, type, mssql::codec::LiteralContext::InsertValues);

	case LogicalTypeId::UUID:
		return mssql::codec::FormatSqlLiteral(value, type, mssql::codec::LiteralContext::InsertValues);

	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_TZ:
		return mssql::codec::FormatSqlLiteral(value, type, mssql::codec::LiteralContext::InsertValues);

	default:
		throw InvalidInputException("Cannot serialize DuckDB type '%s' for SQL Server INSERT", type.ToString());
	}
}

string MSSQLValueSerializer::SerializeFromVector(Vector &vector, idx_t index, const LogicalType &target_type) {
	// For simplicity, extract the value and serialize
	// This could be optimized for bulk operations
	auto value = vector.GetValue(index);
	return Serialize(value, target_type);
}

idx_t MSSQLValueSerializer::EstimateSerializedSize(const Value &value, const LogicalType &type) {
	if (value.IsNull()) {
		return 4;  // "NULL"
	}

	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return 1;  // "0" or "1"

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
		return 40;	// CAST(... AS DECIMAL(20,0))

	case LogicalTypeId::HUGEINT:
		return 45;	// max 39 digits + sign

	case LogicalTypeId::FLOAT:
		return 20;	// scientific notation

	case LogicalTypeId::DOUBLE:
		return 30;	// scientific notation

	case LogicalTypeId::DECIMAL:
		return 45;	// max precision 38 + scale + sign + decimal point

	case LogicalTypeId::VARCHAR: {
		auto str_val = StringValue::Get(value);
		// Per-type wrapper overhead from the codec layer + worst-case escape
		// (every char could be a doubled single quote).
		return mssql::codec::string::EstimateLiteralSize(type) + str_val.size() * 2;
	}
	case LogicalTypeId::INTERVAL:
		// INTERVAL renders as N'<Interval::ToString>'. The longest canonical
		// form for any valid DuckDB interval fits comfortably in ~64 bytes.
		return mssql::codec::string::EstimateLiteralSize(type) + 64;

	case LogicalTypeId::BLOB:
	case LogicalTypeId::GEOMETRY:
		return mssql::codec::EstimateLiteralSize(type);

	case LogicalTypeId::UUID:
		return mssql::codec::EstimateLiteralSize(type);

	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_TZ:
		return mssql::codec::EstimateLiteralSize(type);

	default:
		return 50;	// Conservative default
	}
}

}  // namespace duckdb
