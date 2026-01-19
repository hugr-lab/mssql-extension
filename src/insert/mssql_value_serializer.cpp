#include "insert/mssql_value_serializer.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/types/decimal.hpp"
#include <cmath>
#include <sstream>
#include <iomanip>

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
	if (result.find('.') == string::npos && result.find('e') == string::npos &&
	    result.find('E') == string::npos) {
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
	if (result.find('.') == string::npos && result.find('e') == string::npos &&
	    result.find('E') == string::npos) {
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

string MSSQLValueSerializer::SerializeString(const string_t &value) {
	// Always use N'...' Unicode literal for proper collation handling
	string result = "N'";
	const char *data = value.GetData();
	idx_t length = value.GetSize();

	for (idx_t i = 0; i < length; i++) {
		char c = data[i];
		if (c == '\'') {
			result += "''";
		} else {
			result += c;
		}
	}
	result += "'";
	return result;
}

//===----------------------------------------------------------------------===//
// Blob Serialization
//===----------------------------------------------------------------------===//

string MSSQLValueSerializer::SerializeBlob(const string_t &value) {
	// 0x hex encoding
	const char *data = value.GetData();
	idx_t length = value.GetSize();

	string result = "0x";
	result.reserve(2 + length * 2);

	static const char hex_chars[] = "0123456789ABCDEF";
	for (idx_t i = 0; i < length; i++) {
		unsigned char byte = static_cast<unsigned char>(data[i]);
		result += hex_chars[byte >> 4];
		result += hex_chars[byte & 0x0F];
	}

	return result;
}

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

	return StringUtil::Format(
	    "CAST('%04d-%02d-%02dT%02d:%02d:%02d.%07d' AS DATETIME2(7))",
	    year, month, day, hour, min, sec, nanos100);
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

	return StringUtil::Format(
	    "CAST('%04d-%02d-%02dT%02d:%02d:%02d.%07d%c%02d:%02d' AS DATETIMEOFFSET(7))",
	    year, month, day, hour, min, sec, nanos100, sign, offset_hours, offset_mins);
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
		return SerializeBoolean(BooleanValue::Get(value));

	case LogicalTypeId::TINYINT:
		return SerializeInteger(TinyIntValue::Get(value));

	case LogicalTypeId::SMALLINT:
		return SerializeInteger(SmallIntValue::Get(value));

	case LogicalTypeId::INTEGER:
		return SerializeInteger(IntegerValue::Get(value));

	case LogicalTypeId::BIGINT:
		return SerializeInteger(BigIntValue::Get(value));

	case LogicalTypeId::UTINYINT:
		return SerializeInteger(static_cast<int64_t>(UTinyIntValue::Get(value)));

	case LogicalTypeId::USMALLINT:
		return SerializeInteger(static_cast<int64_t>(USmallIntValue::Get(value)));

	case LogicalTypeId::UINTEGER:
		return SerializeInteger(static_cast<int64_t>(UIntegerValue::Get(value)));

	case LogicalTypeId::UBIGINT:
		return SerializeUBigInt(UBigIntValue::Get(value));

	case LogicalTypeId::HUGEINT: {
		auto hugeint_val = HugeIntValue::Get(value);
		return SerializeDecimal(hugeint_val, 38, 0);
	}

	case LogicalTypeId::FLOAT:
		return SerializeFloat(FloatValue::Get(value));

	case LogicalTypeId::DOUBLE:
		return SerializeDouble(DoubleValue::Get(value));

	case LogicalTypeId::DECIMAL: {
		auto width = DecimalType::GetWidth(type);
		auto scale = DecimalType::GetScale(type);
		// Get the internal storage type
		switch (type.InternalType()) {
		case PhysicalType::INT16:
			return SerializeDecimal(hugeint_t(value.GetValue<int16_t>()), width, scale);
		case PhysicalType::INT32:
			return SerializeDecimal(hugeint_t(value.GetValue<int32_t>()), width, scale);
		case PhysicalType::INT64:
			return SerializeDecimal(hugeint_t(value.GetValue<int64_t>()), width, scale);
		case PhysicalType::INT128:
			return SerializeDecimal(value.GetValue<hugeint_t>(), width, scale);
		default:
			throw InternalException("Unknown decimal internal type");
		}
	}

	case LogicalTypeId::VARCHAR:
		return SerializeString(StringValue::Get(value));

	case LogicalTypeId::BLOB:
		return SerializeBlob(StringValue::Get(value));

	case LogicalTypeId::UUID: {
		// UUID is stored as hugeint_t
		auto uuid_val = value.GetValue<hugeint_t>();
		return SerializeUUID(uuid_val);
	}

	case LogicalTypeId::DATE:
		return SerializeDate(DateValue::Get(value));

	case LogicalTypeId::TIME:
		return SerializeTime(TimeValue::Get(value));

	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
		return SerializeTimestamp(TimestampValue::Get(value));

	case LogicalTypeId::TIMESTAMP_TZ: {
		// For TIMESTAMP_TZ, we need to handle the timezone offset
		// DuckDB stores TIMESTAMP_TZ as UTC timestamp internally
		auto ts = TimestampValue::Get(value);
		// Default to UTC (0 offset) - the server will handle conversion
		return SerializeTimestampTZ(ts, 0);
	}

	default:
		throw InvalidInputException("Cannot serialize DuckDB type '%s' for SQL Server INSERT",
		                            type.ToString());
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
		return 4; // "NULL"
	}

	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return 1; // "0" or "1"

	case LogicalTypeId::TINYINT:
	case LogicalTypeId::UTINYINT:
		return 4; // max 3 digits + sign

	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::USMALLINT:
		return 6; // max 5 digits + sign

	case LogicalTypeId::INTEGER:
	case LogicalTypeId::UINTEGER:
		return 11; // max 10 digits + sign

	case LogicalTypeId::BIGINT:
		return 20; // max 19 digits + sign

	case LogicalTypeId::UBIGINT:
		return 40; // CAST(... AS DECIMAL(20,0))

	case LogicalTypeId::HUGEINT:
		return 45; // max 39 digits + sign

	case LogicalTypeId::FLOAT:
		return 20; // scientific notation

	case LogicalTypeId::DOUBLE:
		return 30; // scientific notation

	case LogicalTypeId::DECIMAL:
		return 45; // max precision 38 + scale + sign + decimal point

	case LogicalTypeId::VARCHAR: {
		auto str_val = StringValue::Get(value);
		// N'...' + escaping (worst case doubles length)
		return 3 + str_val.size() * 2;
	}

	case LogicalTypeId::BLOB: {
		auto blob_val = StringValue::Get(value);
		// 0x + 2 hex chars per byte
		return 2 + blob_val.size() * 2;
	}

	case LogicalTypeId::UUID:
		return 38; // 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx'

	case LogicalTypeId::DATE:
		return 12; // 'YYYY-MM-DD'

	case LogicalTypeId::TIME:
		return 20; // 'HH:MM:SS.fffffff'

	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
		return 60; // CAST('...' AS DATETIME2(7))

	case LogicalTypeId::TIMESTAMP_TZ:
		return 75; // CAST('...' AS DATETIMEOFFSET(7))

	default:
		return 50; // Conservative default
	}
}

}  // namespace duckdb
