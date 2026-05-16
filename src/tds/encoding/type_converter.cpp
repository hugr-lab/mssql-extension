#include "tds/encoding/type_converter.hpp"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include "codec/boolean_codec.hpp"
#include "codec/decimal_codec.hpp"
#include "codec/float_codec.hpp"
#include "codec/integer_codec.hpp"
#include "codec/string_codec.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "tds/encoding/datetime_encoding.hpp"
#include "tds/encoding/decimal_encoding.hpp"
#include "tds/encoding/guid_encoding.hpp"
#include "tds/encoding/utf16.hpp"
#include "tds/tds_types.hpp"

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetTypeConverterDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define MSSQL_TC_DEBUG_LOG(level, fmt, ...)                         \
	do {                                                            \
		if (GetTypeConverterDebugLevel() >= level) {                \
			fprintf(stderr, "[MSSQL TC] " fmt "\n", ##__VA_ARGS__); \
		}                                                           \
	} while (0)

namespace duckdb {
namespace tds {
namespace encoding {

LogicalType TypeConverter::GetDuckDBType(const ColumnMetadata &column) {
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
		case 1:
			return LogicalType::UTINYINT;  // SQL Server TINYINT is unsigned
		case 2:
			return LogicalType::SMALLINT;
		case 4:
			return LogicalType::INTEGER;
		case 8:
			return LogicalType::BIGINT;
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
	case TDS_TYPE_DATETIMEOFFSET:
		return LogicalType::TIMESTAMP_TZ;

	// GUID
	case TDS_TYPE_UNIQUEIDENTIFIER:
		return LogicalType::UUID;

	// XML -> VARCHAR (PLP + UTF-16LE, same as NVARCHAR(MAX))
	case TDS_TYPE_XML:
		return LogicalType::VARCHAR;

	// Unsupported types
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
		throw InvalidInputException("MSSQL Error: Unknown SQL Server type (0x%02X) for column '%s'.", column.type_id,
									column.name.c_str());
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
	case TDS_TYPE_DATETIMEOFFSET:
	case TDS_TYPE_UNIQUEIDENTIFIER:
	case TDS_TYPE_XML:
		return true;
	default:
		return false;
	}
}

std::string TypeConverter::GetTypeName(uint8_t type_id) {
	switch (type_id) {
	case TDS_TYPE_TINYINT:
		return "TINYINT";
	case TDS_TYPE_SMALLINT:
		return "SMALLINT";
	case TDS_TYPE_INT:
		return "INT";
	case TDS_TYPE_BIGINT:
		return "BIGINT";
	case TDS_TYPE_INTN:
		return "INTN";
	case TDS_TYPE_BIT:
		return "BIT";
	case TDS_TYPE_BITN:
		return "BITN";
	case TDS_TYPE_REAL:
		return "REAL";
	case TDS_TYPE_FLOAT:
		return "FLOAT";
	case TDS_TYPE_FLOATN:
		return "FLOATN";
	case TDS_TYPE_DECIMAL:
		return "DECIMAL";
	case TDS_TYPE_NUMERIC:
		return "NUMERIC";
	case TDS_TYPE_MONEY:
		return "MONEY";
	case TDS_TYPE_SMALLMONEY:
		return "SMALLMONEY";
	case TDS_TYPE_MONEYN:
		return "MONEYN";
	case TDS_TYPE_BIGCHAR:
		return "CHAR";
	case TDS_TYPE_BIGVARCHAR:
		return "VARCHAR";
	case TDS_TYPE_NCHAR:
		return "NCHAR";
	case TDS_TYPE_NVARCHAR:
		return "NVARCHAR";
	case TDS_TYPE_BIGBINARY:
		return "BINARY";
	case TDS_TYPE_BIGVARBINARY:
		return "VARBINARY";
	case TDS_TYPE_DATE:
		return "DATE";
	case TDS_TYPE_TIME:
		return "TIME";
	case TDS_TYPE_DATETIME:
		return "DATETIME";
	case TDS_TYPE_SMALLDATETIME:
		return "SMALLDATETIME";
	case TDS_TYPE_DATETIME2:
		return "DATETIME2";
	case TDS_TYPE_DATETIMEN:
		return "DATETIMEN";
	case TDS_TYPE_DATETIMEOFFSET:
		return "DATETIMEOFFSET";
	case TDS_TYPE_UNIQUEIDENTIFIER:
		return "UNIQUEIDENTIFIER";
	case TDS_TYPE_XML:
		return "XML";
	case TDS_TYPE_UDT:
		return "UDT";
	case TDS_TYPE_SQL_VARIANT:
		return "SQL_VARIANT";
	case TDS_TYPE_IMAGE:
		return "IMAGE";
	case TDS_TYPE_TEXT:
		return "TEXT";
	case TDS_TYPE_NTEXT:
		return "NTEXT";
	default:
		return "UNKNOWN";
	}
}

void TypeConverter::ConvertValue(const std::vector<uint8_t> &value, bool is_null, const ColumnMetadata &column,
								 Vector &vector, idx_t row_idx) {
	if (is_null) {
		FlatVector::SetNull(vector, row_idx, true);
		return;
	}

	// Issue #89: catalog vs runtime type divergence. SQL Server views can project a column
	// at a different type than sys.columns reports (typically via CAST/CONVERT inside the
	// view definition). When the destination vector was allocated as VARCHAR from the catalog
	// but TDS sends back a non-string type, render the value as a string instead of crashing
	// inside FlatVector::GetData<T> with a vector-type assertion.
	if (vector.GetType().id() == LogicalTypeId::VARCHAR && !IsStringTdsType(column.type_id)) {
		WriteAsStringFallback(value, column, vector, row_idx);
		return;
	}

	switch (column.type_id) {
	case TDS_TYPE_TINYINT:
	case TDS_TYPE_SMALLINT:
	case TDS_TYPE_INT:
	case TDS_TYPE_BIGINT:
	case TDS_TYPE_INTN:
		mssql::codec::integer::DecodeFromTds(value, column, vector, row_idx);
		break;

	case TDS_TYPE_BIT:
	case TDS_TYPE_BITN:
		mssql::codec::boolean::DecodeFromTds(value, column, vector, row_idx);
		break;

	case TDS_TYPE_REAL:
	case TDS_TYPE_FLOAT:
	case TDS_TYPE_FLOATN:
		mssql::codec::float_family::DecodeFromTds(value, column, vector, row_idx);
		break;

	case TDS_TYPE_DECIMAL:
	case TDS_TYPE_NUMERIC:
		mssql::codec::decimal::DecodeFromTds(value, column, vector, row_idx);
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
	case TDS_TYPE_XML:
		mssql::codec::string::DecodeFromTds(value, column, vector, row_idx);
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

	case TDS_TYPE_DATETIMEOFFSET:
		ConvertDatetimeOffset(value, column, vector, row_idx);
		break;

	case TDS_TYPE_UNIQUEIDENTIFIER:
		ConvertGuid(value, vector, row_idx);
		break;

	default:
		throw InvalidInputException("Type conversion not implemented for type 0x%02X", column.type_id);
	}
}

void TypeConverter::ConvertMoney(const std::vector<uint8_t> &value, const ColumnMetadata &column, Vector &vector,
								 idx_t row_idx) {
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

void TypeConverter::ConvertBinary(const std::vector<uint8_t> &value, Vector &vector, idx_t row_idx) {
	FlatVector::GetData<string_t>(vector)[row_idx] =
		StringVector::AddStringOrBlob(vector, reinterpret_cast<const char *>(value.data()), value.size());
}

void TypeConverter::ConvertDate(const std::vector<uint8_t> &value, Vector &vector, idx_t row_idx) {
	date_t d = DateTimeEncoding::ConvertDate(value.data());
	FlatVector::GetData<date_t>(vector)[row_idx] = d;
}

void TypeConverter::ConvertTime(const std::vector<uint8_t> &value, const ColumnMetadata &column, Vector &vector,
								idx_t row_idx) {
	dtime_t t = DateTimeEncoding::ConvertTime(value.data(), column.scale);
	FlatVector::GetData<dtime_t>(vector)[row_idx] = t;
}

void TypeConverter::ConvertDateTime(const std::vector<uint8_t> &value, const ColumnMetadata &column, Vector &vector,
									idx_t row_idx) {
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

void TypeConverter::ConvertDatetimeOffset(const std::vector<uint8_t> &value, const ColumnMetadata &column,
										  Vector &vector, idx_t row_idx) {
	timestamp_t ts = DateTimeEncoding::ConvertDatetimeOffset(value.data(), column.scale);
	FlatVector::GetData<timestamp_t>(vector)[row_idx] = ts;
}

void TypeConverter::ConvertGuid(const std::vector<uint8_t> &value, Vector &vector, idx_t row_idx) {
	hugeint_t guid = GuidEncoding::ConvertGuid(value.data());
	FlatVector::GetData<hugeint_t>(vector)[row_idx] = guid;
}

//===----------------------------------------------------------------------===//
// Issue #89 — VARCHAR fallback for catalog-vs-runtime type divergence
//===----------------------------------------------------------------------===//

bool TypeConverter::IsStringTdsType(uint8_t type_id) {
	switch (type_id) {
	case TDS_TYPE_BIGCHAR:
	case TDS_TYPE_BIGVARCHAR:
	case TDS_TYPE_NCHAR:
	case TDS_TYPE_NVARCHAR:
	case TDS_TYPE_XML:
	case TDS_TYPE_TEXT:
	case TDS_TYPE_NTEXT:
		return true;
	default:
		return false;
	}
}

namespace {

std::string FormatIntegerFallback(const std::vector<uint8_t> &value) {
	// Mirror codec::integer::DecodeFromTds size-dispatch — TINYINT is unsigned (0-255),
	// SMALLINT/INT/BIGINT are signed.
	switch (value.size()) {
	case 1:
		return std::to_string(static_cast<unsigned int>(value[0]));
	case 2: {
		int16_t v = 0;
		std::memcpy(&v, value.data(), 2);
		return std::to_string(v);
	}
	case 4: {
		int32_t v = 0;
		std::memcpy(&v, value.data(), 4);
		return std::to_string(v);
	}
	case 8: {
		int64_t v = 0;
		std::memcpy(&v, value.data(), 8);
		return std::to_string(v);
	}
	default:
		throw InvalidInputException("VARCHAR fallback: unexpected integer length %zu", value.size());
	}
}

std::string FormatFloatFallback(const std::vector<uint8_t> &value) {
	std::ostringstream oss;
	if (value.size() == 4) {
		float f = 0;
		std::memcpy(&f, value.data(), 4);
		oss << std::setprecision(9) << f;
	} else if (value.size() == 8) {
		double d = 0;
		std::memcpy(&d, value.data(), 8);
		oss << std::setprecision(17) << d;
	} else {
		throw InvalidInputException("VARCHAR fallback: unexpected float length %zu", value.size());
	}
	return oss.str();
}

}  // namespace

void TypeConverter::WriteAsStringFallback(const std::vector<uint8_t> &value, const ColumnMetadata &column,
										  Vector &vector, idx_t row_idx) {
	std::string rendered;
	switch (column.type_id) {
	case TDS_TYPE_TINYINT:
	case TDS_TYPE_SMALLINT:
	case TDS_TYPE_INT:
	case TDS_TYPE_BIGINT:
	case TDS_TYPE_INTN:
		rendered = FormatIntegerFallback(value);
		break;
	case TDS_TYPE_BIT:
	case TDS_TYPE_BITN:
		rendered = (!value.empty() && value[0] != 0) ? "1" : "0";
		break;
	case TDS_TYPE_REAL:
	case TDS_TYPE_FLOAT:
	case TDS_TYPE_FLOATN:
		rendered = FormatFloatFallback(value);
		break;
	case TDS_TYPE_DECIMAL:
	case TDS_TYPE_NUMERIC:
		rendered = mssql::codec::decimal::RenderAsString(value, column.precision, column.scale);
		break;
	case TDS_TYPE_MONEY:
	case TDS_TYPE_SMALLMONEY:
	case TDS_TYPE_MONEYN:
		rendered = mssql::codec::decimal::RenderMoneyAsString(value);
		break;
	case TDS_TYPE_UNIQUEIDENTIFIER: {
		hugeint_t guid = GuidEncoding::ConvertGuid(value.data());
		rendered = UUID::ToString(guid);
		break;
	}
	default:
		throw InvalidInputException(
			"MSSQL: catalog reported VARCHAR for this column but SQL Server returned TDS type 0x%02X — "
			"this typically happens with VIEWs that CAST/CONVERT a column to a different type. "
			"Workaround: re-attach the database after the view definition changed, or use mssql_scan() "
			"with an explicit CAST in the query.",
			column.type_id);
	}
	FlatVector::GetData<string_t>(vector)[row_idx] = StringVector::AddString(vector, rendered);
}

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb
