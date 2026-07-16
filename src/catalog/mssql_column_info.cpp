#include "catalog/mssql_column_info.hpp"
#include <algorithm>
#include <cctype>
#include "duckdb/common/exception.hpp"

namespace duckdb {

MSSQLColumnInfo::MSSQLColumnInfo()
	: column_id(0),
	  max_length(0),
	  precision(0),
	  scale(0),
	  is_nullable(true),
	  is_case_sensitive(false),
	  is_unicode(false),
	  is_utf8(false),
	  is_cast_required(false),
	  is_geometry(false) {}

MSSQLColumnInfo::MSSQLColumnInfo(const string &name, int32_t column_id, const string &sql_type_name, int16_t max_length,
								 uint8_t precision, uint8_t scale, bool is_nullable, const string &collation_name,
								 const string &database_collation)
	: name(name),
	  column_id(column_id),
	  sql_type_name(sql_type_name),
	  max_length(max_length),
	  precision(precision),
	  scale(scale),
	  is_nullable(is_nullable) {
	// Use database collation as fallback if column collation is empty
	if (collation_name.empty() && IsTextType(sql_type_name)) {
		this->collation_name = database_collation;
	} else {
		this->collation_name = collation_name;
	}

	// Derive collation flags
	is_case_sensitive = IsCaseSensitiveCollation(this->collation_name);
	is_unicode = IsUnicodeType(sql_type_name);
	is_utf8 = IsUTF8Collation(this->collation_name);

	// Map to DuckDB type
	duckdb_type = MapSQLServerTypeToDuckDB(sql_type_name, max_length, precision, scale);

	// Detect geometry/geography UDTs — table scan rewrites these to .STAsBinary()
	// so the wire delivers OGC WKB bytes (varbinary(max)) instead of MS's
	// proprietary Spatial Type Binary Format. Catalog reports LogicalType::GEOMETRY().
	{
		string lower_type = sql_type_name;
		std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
					   [](unsigned char c) { return std::tolower(c); });
		is_geometry = (lower_type == "geometry" || lower_type == "geography");
	}

	// Mark columns with unsupported SQL Server types for auto-CAST in pushdown.
	// Geometry/geography are "known" (we handle them via STAsBinary rewrite), not auto-CAST.
	is_cast_required = !IsKnownSQLServerType(sql_type_name);
}

//===----------------------------------------------------------------------===//
// Collation Detection
//===----------------------------------------------------------------------===//

bool MSSQLColumnInfo::IsCaseSensitiveCollation(const string &collation_name) {
	if (collation_name.empty()) {
		return false;  // Assume case-insensitive if unknown
	}

	// Convert to uppercase for comparison
	string upper_collation = collation_name;
	std::transform(upper_collation.begin(), upper_collation.end(), upper_collation.begin(),
				   [](unsigned char c) { return std::toupper(c); });

	// Check for _CS_ (case-sensitive) or _BIN (binary)
	if (upper_collation.find("_CS_") != string::npos || upper_collation.find("_CS") == upper_collation.length() - 3) {
		return true;
	}

	// Binary collations are case-sensitive
	if (upper_collation.find("_BIN") != string::npos) {
		return true;
	}

	// _CI_ indicates case-insensitive
	return false;
}

bool MSSQLColumnInfo::IsAccentSensitiveCollation(const string &collation_name) {
	if (collation_name.empty()) {
		return true;  // Assume accent-sensitive if unknown
	}

	// Convert to uppercase for comparison
	string upper_collation = collation_name;
	std::transform(upper_collation.begin(), upper_collation.end(), upper_collation.begin(),
				   [](unsigned char c) { return std::toupper(c); });

	// Check for _AI_ (accent-insensitive)
	if (upper_collation.find("_AI_") != string::npos || upper_collation.find("_AI") == upper_collation.length() - 3) {
		return false;
	}

	// _AS_ indicates accent-sensitive (default)
	return true;
}

bool MSSQLColumnInfo::IsUTF8Collation(const string &collation_name) {
	if (collation_name.empty()) {
		return false;
	}

	// Convert to uppercase for comparison
	string upper_collation = collation_name;
	std::transform(upper_collation.begin(), upper_collation.end(), upper_collation.begin(),
				   [](unsigned char c) { return std::toupper(c); });

	// Check for _UTF8 suffix
	return upper_collation.find("_UTF8") != string::npos;
}

//===----------------------------------------------------------------------===//
// Type Mapping
//===----------------------------------------------------------------------===//

LogicalType MSSQLColumnInfo::MapSQLServerTypeToDuckDB(const string &sql_type_name, int16_t max_length,
													  uint8_t precision, uint8_t scale) {
	// Convert to lowercase for comparison
	string lower_type = sql_type_name;
	std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
				   [](unsigned char c) { return std::tolower(c); });

	// Integer types
	if (lower_type == "bit") {
		return LogicalType::BOOLEAN;
	}
	if (lower_type == "tinyint") {
		return LogicalType::UTINYINT;
	}
	if (lower_type == "smallint") {
		return LogicalType::SMALLINT;
	}
	if (lower_type == "int") {
		return LogicalType::INTEGER;
	}
	if (lower_type == "bigint") {
		return LogicalType::BIGINT;
	}

	// Floating point types
	if (lower_type == "real") {
		return LogicalType::FLOAT;
	}
	if (lower_type == "float") {
		return LogicalType::DOUBLE;
	}

	// Decimal/numeric types
	if (lower_type == "decimal" || lower_type == "numeric") {
		return LogicalType::DECIMAL(precision, scale);
	}
	if (lower_type == "money") {
		return LogicalType::DECIMAL(19, 4);
	}
	if (lower_type == "smallmoney") {
		return LogicalType::DECIMAL(10, 4);
	}

	// Character types
	if (lower_type == "char" || lower_type == "varchar" || lower_type == "text") {
		return LogicalType::VARCHAR;
	}
	if (lower_type == "nchar" || lower_type == "nvarchar" || lower_type == "ntext") {
		return LogicalType::VARCHAR;  // Unicode also maps to VARCHAR in DuckDB
	}

	// Date/time types
	if (lower_type == "date") {
		return LogicalType::DATE;
	}
	if (lower_type == "time") {
		return LogicalType::TIME;
	}
	if (lower_type == "datetime" || lower_type == "smalldatetime") {
		// Fixed wire precision (~3 ms for DATETIME, 1 min for SMALLDATETIME) —
		// always fits in DuckDB's µs TIMESTAMP.
		return LogicalType::TIMESTAMP;
	}
	if (lower_type == "datetime2") {
		// Pick the narrowest DuckDB TIMESTAMP variant that can losslessly hold
		// the column's wire precision (spec 045 — type round-trip transparency).
		// scale 0       → TIMESTAMP_S  (seconds)
		// scale 1-3     → TIMESTAMP_MS (milliseconds)
		// scale 4-6     → TIMESTAMP    (microseconds — DuckDB native)
		// scale 7       → TIMESTAMP_NS (DuckDB ns can hold 100-ns wire ticks losslessly)
		if (scale == 0) {
			return LogicalType::TIMESTAMP_S;
		}
		if (scale <= 3) {
			return LogicalType::TIMESTAMP_MS;
		}
		if (scale <= 6) {
			return LogicalType::TIMESTAMP;
		}
		return LogicalType::TIMESTAMP_NS;
	}
	if (lower_type == "datetimeoffset") {
		// DuckDB has no nanosecond-precision time-zone-aware type; collapse to
		// µs TIMESTAMP_TZ regardless of source scale (lossless for ≤6, drops
		// the trailing digit for scale 7).
		return LogicalType::TIMESTAMP_TZ;
	}

	// Binary types
	if (lower_type == "binary" || lower_type == "varbinary" || lower_type == "image") {
		return LogicalType::BLOB;
	}

	// Special types
	if (lower_type == "uniqueidentifier") {
		return LogicalType::UUID;
	}

	// Spatial types — geometry and geography both arrive via STAsBinary() rewrite
	// (see is_geometry handling in the constructor + table_scan::BuildColumnExpression).
	// DuckDB's first-class GEOMETRY type stores WKB bytes — same physical storage as BLOB.
	if (lower_type == "geometry" || lower_type == "geography") {
		return LogicalType::GEOMETRY();
	}

	// Default to VARCHAR for unknown types
	return LogicalType::VARCHAR;
}

//===----------------------------------------------------------------------===//
// Type Checks
//===----------------------------------------------------------------------===//

bool MSSQLColumnInfo::IsKnownSQLServerType(const string &sql_type_name) {
	string lower_type = sql_type_name;
	std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
				   [](unsigned char c) { return std::tolower(c); });

	// All types explicitly handled in MapSQLServerTypeToDuckDB
	return lower_type == "bit" || lower_type == "tinyint" || lower_type == "smallint" || lower_type == "int" ||
		   lower_type == "bigint" || lower_type == "real" || lower_type == "float" || lower_type == "decimal" ||
		   lower_type == "numeric" || lower_type == "money" || lower_type == "smallmoney" || lower_type == "char" ||
		   lower_type == "varchar" || lower_type == "text" || lower_type == "nchar" || lower_type == "nvarchar" ||
		   lower_type == "ntext" || lower_type == "date" || lower_type == "time" || lower_type == "datetime" ||
		   lower_type == "datetime2" || lower_type == "smalldatetime" || lower_type == "datetimeoffset" ||
		   lower_type == "binary" || lower_type == "varbinary" || lower_type == "image" ||
		   lower_type == "uniqueidentifier" ||
		   // XML has dedicated TDS-level support (0xF1) and works without CAST
		   lower_type == "xml" ||
		   // Spatial UDTs — handled by table-scan rewrite to STAsBinary() (spec 045 / sub-phase 5).
		   lower_type == "geometry" || lower_type == "geography";
}

bool MSSQLColumnInfo::IsTextType(const string &sql_type_name) {
	string lower_type = sql_type_name;
	std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
				   [](unsigned char c) { return std::tolower(c); });

	return lower_type == "char" || lower_type == "varchar" || lower_type == "text" || lower_type == "nchar" ||
		   lower_type == "nvarchar" || lower_type == "ntext";
}

bool MSSQLColumnInfo::IsUnicodeType(const string &sql_type_name) {
	string lower_type = sql_type_name;
	std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
				   [](unsigned char c) { return std::tolower(c); });

	return lower_type == "nchar" || lower_type == "nvarchar" || lower_type == "ntext";
}

bool MSSQLColumnInfo::IsLegacyLobType(const string &sql_type_name) {
	string lower_type = sql_type_name;
	std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
				   [](unsigned char c) { return std::tolower(c); });

	return lower_type == "text" || lower_type == "ntext" || lower_type == "image";
}

}  // namespace duckdb
