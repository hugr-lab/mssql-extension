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
	bool is_cast_required;	 // Unsupported type: needs CAST to NVARCHAR(MAX)
	bool is_geometry;		 // True for SQL Server geometry/geography columns; table scan projects
							 // [col].STAsBinary() AS [col] so the wire delivers OGC WKB which lands
							 // in a LogicalType::GEOMETRY() vector via the Binary codec.

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

	// Check if SQL Server type is natively supported (has explicit mapping or TDS-level support)
	static bool IsKnownSQLServerType(const string &sql_type_name);

	// Check if type is a text type that has collation
	static bool IsTextType(const string &sql_type_name);

	// Check if type is Unicode (NVARCHAR, NCHAR, NTEXT)
	static bool IsUnicodeType(const string &sql_type_name);

	// Check if type is a deprecated LOB (TEXT, NTEXT, IMAGE), for which sys.columns reports
	// max_length 16 — the size of the in-row pointer, not of the data (which runs to 2 GB). Any
	// length derived from that 16 is meaningless: these must be treated as MAX types.
	//
	// The right MAX target differs per type: TEXT/NTEXT are character data (NVARCHAR(MAX)), IMAGE
	// is binary (VARBINARY(MAX)). This predicate says only "max_length is a pointer size, ignore
	// it" — it is NOT a licence to give all three the same CAST. Routing IMAGE through
	// NVARCHAR(MAX) would silently turn binary into a garbage string rather than failing. See
	// issue #197 (NTEXT/IMAGE are unreadable through the catalog today).
	static bool IsLegacyLobType(const string &sql_type_name);
};

}  // namespace duckdb
