#pragma once

#include <string>
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLColumnInfo - Column metadata including SQL Server-specific information
//===----------------------------------------------------------------------===//

struct MSSQLColumnInfo {
	// Basic column info
	string name;			  // Column name
	int32_t column_id;		  // Ordinal position (1-based)
	string sql_type_name;	  // SQL Server type name (e.g., "varchar", "int")
	LogicalType duckdb_type;  // Mapped DuckDB type

	// Size/precision info
	int16_t max_length;	 // Max length in bytes (-1 for MAX types)
	uint8_t precision;	 // Numeric precision
	uint8_t scale;		 // Numeric scale

	// Nullability
	bool is_nullable;  // Allows NULL values

	// Collation info (for text types)
	string collation_name;	 // Column collation (may be empty for non-text)
	bool is_case_sensitive;	 // Derived from collation (_CS_ or _BIN)
	bool is_unicode;		 // True for NVARCHAR/NCHAR/NTEXT
	bool is_utf8;			 // Derived from collation (_UTF8)

	// Default constructor
	MSSQLColumnInfo();

	// Constructor from SQL Server metadata
	MSSQLColumnInfo(const string &name, int32_t column_id, const string &sql_type_name, int16_t max_length,
					uint8_t precision, uint8_t scale, bool is_nullable, const string &collation_name,
					const string &database_collation);

	// Parse collation name to extract sensitivity flags
	static bool IsCaseSensitiveCollation(const string &collation_name);
	static bool IsAccentSensitiveCollation(const string &collation_name);
	static bool IsUTF8Collation(const string &collation_name);

	// Map SQL Server type to DuckDB LogicalType
	static LogicalType MapSQLServerTypeToDuckDB(const string &sql_type_name, int16_t max_length, uint8_t precision,
												uint8_t scale);

	// Check if type is a text type that has collation
	static bool IsTextType(const string &sql_type_name);

	// Check if type is Unicode (NVARCHAR, NCHAR, NTEXT)
	static bool IsUnicodeType(const string &sql_type_name);
};

}  // namespace duckdb
