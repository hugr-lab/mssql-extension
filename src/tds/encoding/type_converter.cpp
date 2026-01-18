#include "tds/encoding/type_converter.hpp"
#include "tds/encoding/decimal_encoding.hpp"
#include "tds/encoding/datetime_encoding.hpp"
#include "tds/encoding/guid_encoding.hpp"
#include "tds/encoding/utf16.hpp"
#include "tds/tds_types.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/uuid.hpp"
#include <cstring>
#include <cstdlib>
#include <chrono>

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetTypeConverterDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char* env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define MSSQL_TC_DEBUG_LOG(level, fmt, ...) \
	do { if (GetTypeConverterDebugLevel() >= level) { \
		fprintf(stderr, "[MSSQL TC] " fmt "\n", ##__VA_ARGS__); \
	} } while(0)

namespace duckdb {
namespace tds {
namespace encoding {

LogicalType TypeConverter::GetDuckDBType(const ColumnMetadata& column) {
	switch (column.type_id) {
	// Integer types
	// Note: SQL Server TINYINT is unsigned (0-255), maps to UTINYINT
	case TDS_TYPE_TINYINT:
		return LogicalType::UTINYINT;
	case TDS_TYPE_SMALLINT:
		return LogicalType::SMALLINT;
	case TDS_TYPE_INT:
		return LogicalType::INTEGER;
	case TDS_TYPE_BIGINT:
		return LogicalType::BIGINT;

	// Nullable integer variants
	case TDS_TYPE_INTN:
		switch (column.max_length) {
		case 1: return LogicalType::UTINYINT;  // SQL Server TINYINT is unsigned
		case 2: return LogicalType::SMALLINT;
		case 4: return LogicalType::INTEGER;
		case 8: return LogicalType::BIGINT;
		default:
			throw InvalidInputException("Invalid INTN length: %d", column.max_length);
		}

	// Boolean
	case TDS_TYPE_BIT:
	case TDS_TYPE_BITN:
		return LogicalType::BOOLEAN;

	// Floating-point
	case TDS_TYPE_REAL:
		return LogicalType::FLOAT;
	case TDS_TYPE_FLOAT:
		return LogicalType::DOUBLE;
	case TDS_TYPE_FLOATN:
		return (column.max_length == 4) ? LogicalType::FLOAT : LogicalType::DOUBLE;

	// Decimal/Numeric
	case TDS_TYPE_DECIMAL:
	case TDS_TYPE_NUMERIC:
		return LogicalType::DECIMAL(column.precision, column.scale);

	// Money types -> DECIMAL(19,4) or DECIMAL(10,4)
	case TDS_TYPE_MONEY:
		return LogicalType::DECIMAL(19, 4);
	case TDS_TYPE_SMALLMONEY:
		return LogicalType::DECIMAL(10, 4);
	case TDS_TYPE_MONEYN:
		return (column.max_length == 8) ? LogicalType::DECIMAL(19, 4) : LogicalType::DECIMAL(10, 4);

	// String types -> VARCHAR
	case TDS_TYPE_BIGCHAR:
	case TDS_TYPE_BIGVARCHAR:
	case TDS_TYPE_NCHAR:
	case TDS_TYPE_NVARCHAR:
		return LogicalType::VARCHAR;

	// Binary types -> BLOB
	case TDS_TYPE_BIGBINARY:
	case TDS_TYPE_BIGVARBINARY:
		return LogicalType::BLOB;

	// Date/Time
	case TDS_TYPE_DATE:
		return LogicalType::DATE;
	case TDS_TYPE_TIME:
		return LogicalType::TIME;
	case TDS_TYPE_DATETIME:
	case TDS_TYPE_SMALLDATETIME:
	case TDS_TYPE_DATETIME2:
	case TDS_TYPE_DATETIMEN:
		return LogicalType::TIMESTAMP;

	// GUID
	case TDS_TYPE_UNIQUEIDENTIFIER:
		return LogicalType::UUID;

	// Unsupported types
	case TDS_TYPE_XML:
	case TDS_TYPE_UDT:
	case TDS_TYPE_SQL_VARIANT:
	case TDS_TYPE_IMAGE:
	case TDS_TYPE_TEXT:
	case TDS_TYPE_NTEXT:
		throw InvalidInputException(
		    "MSSQL Error: Unsupported SQL Server type '%s' (0x%02X) for column '%s'. "
		    "Consider casting to VARCHAR or excluding this column.",
		    GetTypeName(column.type_id).c_str(), column.type_id, column.name.c_str());

	default:
		throw InvalidInputException(
		    "MSSQL Error: Unknown SQL Server type (0x%02X) for column '%s'.",
		    column.type_id, column.name.c_str());
	}
}

bool TypeConverter::IsSupported(uint8_t type_id) {
	switch (type_id) {
	case TDS_TYPE_TINYINT:
	case TDS_TYPE_SMALLINT:
	case TDS_TYPE_INT:
	case TDS_TYPE_BIGINT:
	case TDS_TYPE_INTN:
	case TDS_TYPE_BIT:
	case TDS_TYPE_BITN:
	case TDS_TYPE_REAL:
	case TDS_TYPE_FLOAT:
	case TDS_TYPE_FLOATN:
	case TDS_TYPE_DECIMAL:
	case TDS_TYPE_NUMERIC:
	case TDS_TYPE_MONEY:
	case TDS_TYPE_SMALLMONEY:
	case TDS_TYPE_MONEYN:
	case TDS_TYPE_BIGCHAR:
	case TDS_TYPE_BIGVARCHAR:
	case TDS_TYPE_NCHAR:
	case TDS_TYPE_NVARCHAR:
	case TDS_TYPE_BIGBINARY:
	case TDS_TYPE_BIGVARBINARY:
	case TDS_TYPE_DATE:
	case TDS_TYPE_TIME:
	case TDS_TYPE_DATETIME:
	case TDS_TYPE_SMALLDATETIME:
	case TDS_TYPE_DATETIME2:
	case TDS_TYPE_DATETIMEN:
	case TDS_TYPE_UNIQUEIDENTIFIER:
		return true;
	default:
		return false;
	}
}

std::string TypeConverter::GetTypeName(uint8_t type_id) {
	switch (type_id) {
	case TDS_TYPE_TINYINT: return "TINYINT";
	case TDS_TYPE_SMALLINT: return "SMALLINT";
	case TDS_TYPE_INT: return "INT";
	case TDS_TYPE_BIGINT: return "BIGINT";
	case TDS_TYPE_INTN: return "INTN";
	case TDS_TYPE_BIT: return "BIT";
	case TDS_TYPE_BITN: return "BITN";
	case TDS_TYPE_REAL: return "REAL";
	case TDS_TYPE_FLOAT: return "FLOAT";
	case TDS_TYPE_FLOATN: return "FLOATN";
	case TDS_TYPE_DECIMAL: return "DECIMAL";
	case TDS_TYPE_NUMERIC: return "NUMERIC";
	case TDS_TYPE_MONEY: return "MONEY";
	case TDS_TYPE_SMALLMONEY: return "SMALLMONEY";
	case TDS_TYPE_MONEYN: return "MONEYN";
	case TDS_TYPE_BIGCHAR: return "CHAR";
	case TDS_TYPE_BIGVARCHAR: return "VARCHAR";
	case TDS_TYPE_NCHAR: return "NCHAR";
	case TDS_TYPE_NVARCHAR: return "NVARCHAR";
	case TDS_TYPE_BIGBINARY: return "BINARY";
	case TDS_TYPE_BIGVARBINARY: return "VARBINARY";
	case TDS_TYPE_DATE: return "DATE";
	case TDS_TYPE_TIME: return "TIME";
	case TDS_TYPE_DATETIME: return "DATETIME";
	case TDS_TYPE_SMALLDATETIME: return "SMALLDATETIME";
	case TDS_TYPE_DATETIME2: return "DATETIME2";
	case TDS_TYPE_DATETIMEN: return "DATETIMEN";
	case TDS_TYPE_UNIQUEIDENTIFIER: return "UNIQUEIDENTIFIER";
	case TDS_TYPE_XML: return "XML";
	case TDS_TYPE_UDT: return "UDT";
	case TDS_TYPE_SQL_VARIANT: return "SQL_VARIANT";
	case TDS_TYPE_IMAGE: return "IMAGE";
	case TDS_TYPE_TEXT: return "TEXT";
	case TDS_TYPE_NTEXT: return "NTEXT";
	default: return "UNKNOWN";
	}
}

void TypeConverter::ConvertValue(const std::vector<uint8_t>& value, bool is_null,
                                  const ColumnMetadata& column,
                                  Vector& vector, idx_t row_idx) {
	if (is_null) {
		FlatVector::SetNull(vector, row_idx, true);
		return;
	}

	switch (column.type_id) {
	case TDS_TYPE_TINYINT:
	case TDS_TYPE_SMALLINT:
	case TDS_TYPE_INT:
	case TDS_TYPE_BIGINT:
	case TDS_TYPE_INTN:
		ConvertInteger(value, column, vector, row_idx);
		break;

	case TDS_TYPE_BIT:
	case TDS_TYPE_BITN:
		ConvertBoolean(value, vector, row_idx);
		break;

	case TDS_TYPE_REAL:
	case TDS_TYPE_FLOAT:
	case TDS_TYPE_FLOATN:
		ConvertFloat(value, column, vector, row_idx);
		break;

	case TDS_TYPE_DECIMAL:
	case TDS_TYPE_NUMERIC:
		ConvertDecimal(value, column, vector, row_idx);
		break;

	case TDS_TYPE_MONEY:
	case TDS_TYPE_SMALLMONEY:
	case TDS_TYPE_MONEYN:
		ConvertMoney(value, column, vector, row_idx);
		break;

	case TDS_TYPE_BIGCHAR:
	case TDS_TYPE_BIGVARCHAR:
	case TDS_TYPE_NCHAR:
	case TDS_TYPE_NVARCHAR:
		ConvertString(value, column, vector, row_idx);
		break;

	case TDS_TYPE_BIGBINARY:
	case TDS_TYPE_BIGVARBINARY:
		ConvertBinary(value, vector, row_idx);
		break;

	case TDS_TYPE_DATE:
		ConvertDate(value, vector, row_idx);
		break;

	case TDS_TYPE_TIME:
		ConvertTime(value, column, vector, row_idx);
		break;

	case TDS_TYPE_DATETIME:
	case TDS_TYPE_SMALLDATETIME:
	case TDS_TYPE_DATETIME2:
	case TDS_TYPE_DATETIMEN:
		ConvertDateTime(value, column, vector, row_idx);
		break;

	case TDS_TYPE_UNIQUEIDENTIFIER:
		ConvertGuid(value, vector, row_idx);
		break;

	default:
		throw InvalidInputException("Type conversion not implemented for type 0x%02X", column.type_id);
	}
}

void TypeConverter::ConvertInteger(const std::vector<uint8_t>& value,
                                    const ColumnMetadata& column,
                                    Vector& vector, idx_t row_idx) {
	size_t len = value.size();

	switch (len) {
	case 1:
		// SQL Server TINYINT is unsigned (0-255), use uint8_t
		FlatVector::GetData<uint8_t>(vector)[row_idx] = value[0];
		break;
	case 2: {
		int16_t v = 0;
		std::memcpy(&v, value.data(), 2);
		FlatVector::GetData<int16_t>(vector)[row_idx] = v;
		break;
	}
	case 4: {
		int32_t v = 0;
		std::memcpy(&v, value.data(), 4);
		FlatVector::GetData<int32_t>(vector)[row_idx] = v;
		break;
	}
	case 8: {
		int64_t v = 0;
		std::memcpy(&v, value.data(), 8);
		FlatVector::GetData<int64_t>(vector)[row_idx] = v;
		break;
	}
	default:
		throw InvalidInputException("Invalid integer length: %d", len);
	}
}

void TypeConverter::ConvertBoolean(const std::vector<uint8_t>& value,
                                    Vector& vector, idx_t row_idx) {
	bool b = !value.empty() && value[0] != 0;
	FlatVector::GetData<bool>(vector)[row_idx] = b;
}

void TypeConverter::ConvertFloat(const std::vector<uint8_t>& value,
                                  const ColumnMetadata& column,
                                  Vector& vector, idx_t row_idx) {
	if (value.size() == 4) {
		float f = 0;
		std::memcpy(&f, value.data(), 4);
		FlatVector::GetData<float>(vector)[row_idx] = f;
	} else if (value.size() == 8) {
		double d = 0;
		std::memcpy(&d, value.data(), 8);
		FlatVector::GetData<double>(vector)[row_idx] = d;
	}
}

void TypeConverter::ConvertDecimal(const std::vector<uint8_t>& value,
                                    const ColumnMetadata& column,
                                    Vector& vector, idx_t row_idx) {
	hugeint_t int_value = DecimalEncoding::ConvertDecimal(value.data(), value.size());

	// DuckDB DECIMAL uses different storage based on precision
	if (column.precision <= 4) {
		FlatVector::GetData<int16_t>(vector)[row_idx] = static_cast<int16_t>(int_value.lower);
	} else if (column.precision <= 9) {
		FlatVector::GetData<int32_t>(vector)[row_idx] = static_cast<int32_t>(int_value.lower);
	} else if (column.precision <= 18) {
		FlatVector::GetData<int64_t>(vector)[row_idx] = static_cast<int64_t>(int_value.lower);
	} else {
		FlatVector::GetData<hugeint_t>(vector)[row_idx] = int_value;
	}
}

void TypeConverter::ConvertMoney(const std::vector<uint8_t>& value,
                                  const ColumnMetadata& column,
                                  Vector& vector, idx_t row_idx) {
	hugeint_t int_value;

	if (value.size() == 8) {
		// MONEY (8 bytes) -> DECIMAL(19,4) requires hugeint_t storage
		int_value = DecimalEncoding::ConvertMoney(value.data());
		FlatVector::GetData<hugeint_t>(vector)[row_idx] = int_value;
	} else if (value.size() == 4) {
		// SMALLMONEY (4 bytes) -> DECIMAL(10,4) fits in int64_t
		int_value = DecimalEncoding::ConvertSmallMoney(value.data());
		FlatVector::GetData<int64_t>(vector)[row_idx] = static_cast<int64_t>(int_value.lower);
	} else {
		throw InvalidInputException("Invalid MONEY length: %d", value.size());
	}
}

void TypeConverter::ConvertString(const std::vector<uint8_t>& value,
                                   const ColumnMetadata& column,
                                   Vector& vector, idx_t row_idx) {
	auto start = std::chrono::steady_clock::now();
	std::string str;

	// NCHAR/NVARCHAR are UTF-16LE, need conversion
	auto decode_start = std::chrono::steady_clock::now();
	if (column.type_id == TDS_TYPE_NCHAR || column.type_id == TDS_TYPE_NVARCHAR) {
		str = Utf16LEDecode(value.data(), value.size());
	} else {
		// CHAR/VARCHAR are single-byte (respect collation for encoding, but typically CP1252/UTF-8)
		str = std::string(reinterpret_cast<const char*>(value.data()), value.size());
	}
	auto decode_end = std::chrono::steady_clock::now();

	// Trim trailing spaces for CHAR/NCHAR
	if (column.type_id == TDS_TYPE_BIGCHAR || column.type_id == TDS_TYPE_NCHAR) {
		size_t end = str.find_last_not_of(' ');
		if (end != std::string::npos) {
			str.erase(end + 1);
		} else {
			str.clear();  // All spaces
		}
	}

	auto add_start = std::chrono::steady_clock::now();
	FlatVector::GetData<string_t>(vector)[row_idx] = StringVector::AddString(vector, str);
	auto add_end = std::chrono::steady_clock::now();

	auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(add_end - start).count();
	auto decode_us = std::chrono::duration_cast<std::chrono::microseconds>(decode_end - decode_start).count();
	auto add_us = std::chrono::duration_cast<std::chrono::microseconds>(add_end - add_start).count();

	// Log only for large strings (>100 bytes) at debug level 2
	if (value.size() > 100) {
		MSSQL_TC_DEBUG_LOG(2, "ConvertString: len=%zu, total=%ldus, decode=%ldus, addstr=%ldus",
		                   value.size(), (long)total_us, (long)decode_us, (long)add_us);
	}
}

void TypeConverter::ConvertBinary(const std::vector<uint8_t>& value,
                                   Vector& vector, idx_t row_idx) {
	FlatVector::GetData<string_t>(vector)[row_idx] =
	    StringVector::AddStringOrBlob(vector, reinterpret_cast<const char*>(value.data()), value.size());
}

void TypeConverter::ConvertDate(const std::vector<uint8_t>& value,
                                 Vector& vector, idx_t row_idx) {
	date_t d = DateTimeEncoding::ConvertDate(value.data());
	FlatVector::GetData<date_t>(vector)[row_idx] = d;
}

void TypeConverter::ConvertTime(const std::vector<uint8_t>& value,
                                 const ColumnMetadata& column,
                                 Vector& vector, idx_t row_idx) {
	dtime_t t = DateTimeEncoding::ConvertTime(value.data(), column.scale);
	FlatVector::GetData<dtime_t>(vector)[row_idx] = t;
}

void TypeConverter::ConvertDateTime(const std::vector<uint8_t>& value,
                                     const ColumnMetadata& column,
                                     Vector& vector, idx_t row_idx) {
	timestamp_t ts;

	switch (column.type_id) {
	case TDS_TYPE_DATETIME:
		ts = DateTimeEncoding::ConvertDatetime(value.data());
		break;
	case TDS_TYPE_SMALLDATETIME:
		ts = DateTimeEncoding::ConvertSmallDatetime(value.data());
		break;
	case TDS_TYPE_DATETIME2:
		ts = DateTimeEncoding::ConvertDatetime2(value.data(), column.scale);
		break;
	case TDS_TYPE_DATETIMEN:
		if (value.size() == 8) {
			ts = DateTimeEncoding::ConvertDatetime(value.data());
		} else if (value.size() == 4) {
			ts = DateTimeEncoding::ConvertSmallDatetime(value.data());
		} else {
			throw InvalidInputException("Invalid DATETIMEN length: %d", value.size());
		}
		break;
	default:
		throw InvalidInputException("Unexpected datetime type: 0x%02X", column.type_id);
	}

	FlatVector::GetData<timestamp_t>(vector)[row_idx] = ts;
}

void TypeConverter::ConvertGuid(const std::vector<uint8_t>& value,
                                 Vector& vector, idx_t row_idx) {
	hugeint_t guid = GuidEncoding::ConvertGuid(value.data());
	FlatVector::GetData<hugeint_t>(vector)[row_idx] = guid;
}

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb
