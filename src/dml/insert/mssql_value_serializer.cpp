#include "dml/insert/mssql_value_serializer.hpp"
#include <cmath>
#include <iomanip>
#include <sstream>
#include "codec/literal_format.hpp"
#include "codec/string_codec.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/uuid.hpp"

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
// Boolean Serialization
//===----------------------------------------------------------------------===//

string MSSQLValueSerializer::SerializeBoolean(bool value) {
	return value ? "1" : "0";
}

//===----------------------------------------------------------------------===//
// Integer Serialization
//===----------------------------------------------------------------------===//

string MSSQLValueSerializer::SerializeInteger(int64_t value) {
	return std::to_string(value);
}

string MSSQLValueSerializer::SerializeUBigInt(uint64_t value) {
	// Values that fit in int64_t can be serialized directly
	if (value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
		return std::to_string(value);
	}
	// Larger values need CAST to DECIMAL(20,0) since SQL Server BIGINT is signed
	return StringUtil::Format("CAST(%llu AS DECIMAL(20,0))", value);
}

//===----------------------------------------------------------------------===//
// Float/Double Serialization
//===----------------------------------------------------------------------===//

void MSSQLValueSerializer::ValidateFloatValue(double value) {
	if (std::isnan(value)) {
		throw InvalidInputException("NaN values are not supported for SQL Server INSERT");
	}
	if (std::isinf(value)) {
		throw InvalidInputException("Infinity values are not supported for SQL Server INSERT");
	}
}

string MSSQLValueSerializer::SerializeFloat(float value) {
	ValidateFloatValue(static_cast<double>(value));

	std::ostringstream oss;
	oss << std::setprecision(9) << value;
	string result = oss.str();

	// Ensure result has decimal point or exponent for SQL Server to treat as float
	if (result.find('.') == string::npos && result.find('e') == string::npos && result.find('E') == string::npos) {
		result += ".0";
	}
	return result;
}

string MSSQLValueSerializer::SerializeDouble(double value) {
	ValidateFloatValue(value);

	std::ostringstream oss;
	oss << std::setprecision(17) << value;
	string result = oss.str();

	// Ensure result has decimal point or exponent
	if (result.find('.') == string::npos && result.find('e') == string::npos && result.find('E') == string::npos) {
		result += ".0";
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
// String Serialization
//===----------------------------------------------------------------------===//

// Blob serialization migrated to codec::binary (spec 045 Phase 6 sub-phase 5).

//===----------------------------------------------------------------------===//
// UUID Serialization
//===----------------------------------------------------------------------===//

string MSSQLValueSerializer::SerializeUUID(const hugeint_t &value) {
	// UUID stored as hugeint, convert to standard format
	auto uuid_str = UUID::ToString(value);
	return "'" + uuid_str + "'";
}

//===----------------------------------------------------------------------===//
// Date/Time Serialization
//===----------------------------------------------------------------------===//

string MSSQLValueSerializer::SerializeDate(date_t value) {
	// ISO format: 'YYYY-MM-DD'
	int32_t year, month, day;
	Date::Convert(value, year, month, day);
	return StringUtil::Format("'%04d-%02d-%02d'", year, month, day);
}

string MSSQLValueSerializer::SerializeTime(dtime_t value) {
	// ISO format with fractional seconds: 'HH:MM:SS.fffffff'
	int32_t hour, min, sec, micros;
	Time::Convert(value, hour, min, sec, micros);

	// Convert microseconds to 100-nanosecond units (7 decimal places)
	int32_t nanos100 = micros * 10;

	return StringUtil::Format("'%02d:%02d:%02d.%07d'", hour, min, sec, nanos100);
}

string MSSQLValueSerializer::SerializeTimestamp(timestamp_t value) {
	// CAST('YYYY-MM-DDTHH:MM:SS.fffffff' AS DATETIME2(7))
	date_t date_part;
	dtime_t time_part;
	Timestamp::Convert(value, date_part, time_part);

	int32_t year, month, day;
	Date::Convert(date_part, year, month, day);

	int32_t hour, min, sec, micros;
	Time::Convert(time_part, hour, min, sec, micros);

	// Convert microseconds to 100-nanosecond units
	int32_t nanos100 = micros * 10;

	return StringUtil::Format("CAST('%04d-%02d-%02dT%02d:%02d:%02d.%07d' AS DATETIME2(7))", year, month, day, hour, min,
							  sec, nanos100);
}

string MSSQLValueSerializer::SerializeTimestampTZ(timestamp_t value, int32_t offset_seconds) {
	// CAST('YYYY-MM-DDTHH:MM:SS.fffffff+HH:MM' AS DATETIMEOFFSET(7))
	date_t date_part;
	dtime_t time_part;
	Timestamp::Convert(value, date_part, time_part);

	int32_t year, month, day;
	Date::Convert(date_part, year, month, day);

	int32_t hour, min, sec, micros;
	Time::Convert(time_part, hour, min, sec, micros);

	// Convert microseconds to 100-nanosecond units
	int32_t nanos100 = micros * 10;

	// Format timezone offset
	char sign = offset_seconds >= 0 ? '+' : '-';
	int32_t abs_offset = std::abs(offset_seconds);
	int32_t offset_hours = abs_offset / 3600;
	int32_t offset_mins = (abs_offset % 3600) / 60;

	return StringUtil::Format("CAST('%04d-%02d-%02dT%02d:%02d:%02d.%07d%c%02d:%02d' AS DATETIMEOFFSET(7))", year, month,
							  day, hour, min, sec, nanos100, sign, offset_hours, offset_mins);
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
