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
	bool is_identity;		 // sys.columns.is_identity (spec 062 W4, issue #327). Set by the
							 // metadata loaders after construction; the INSERT planner keeps a
							 // column list that names one on the statement path, where the server
							 // decides about the explicit value (error 544 without IDENTITY_INSERT).

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
	//! Spec 060: the type to REPORT for this column — MSSQL_VARCHAR(n) /
	//! MSSQL_NVARCHAR(n) where the column is a bounded character one, and plain
	//! `duckdb_type` otherwise. Values are ordinary DuckDB strings either way;
	//! the annotation exists so a target created from this column inherits its
	//! declared type instead of becoming nvarchar(max). Gated by
	//! mssql_catalog_native_types at the point the table entry is built.
	LogicalType NativeDuckDBType() const;

	static LogicalType MapSQLServerTypeToDuckDB(const string &sql_type_name, int16_t max_length, uint8_t precision,
												uint8_t scale);

	// The two spatial CLR UDTs, by name. ONE predicate, because four things key
	// on the same pair and they have to agree: the catalog sets `is_geometry`
	// from it, the type map answers GEOMETRY() from it, `IsKnownSQLServerType`
	// admits them from it, and the DML literal renderer decides from it whether
	// to wrap a value in the server's WKB reader. If the routing and the
	// rendering ever disagreed the failure would be silent in one direction
	// (WKB sent as text on the bulk wire) and a server error in the other.
	static bool IsSpatialType(const string &sql_type_name);

	// Check if SQL Server type is natively supported (has explicit mapping or TDS-level support)
	static bool IsKnownSQLServerType(const string &sql_type_name);

	// Check if type is a text type that has collation
	static bool IsTextType(const string &sql_type_name);

	// Check if type is Unicode (NVARCHAR, NCHAR, NTEXT)
	static bool IsUnicodeType(const string &sql_type_name);

	//! The T-SQL expression that reads this column so its bytes are decodable.
	//!
	//! Four rewrites, and the point of having ONE function for them is that two
	//! lists need them and used to carry different subsets. The table scan's
	//! SELECT list is one; the other is the `OUTPUT INSERTED` list of an
	//! `INSERT … RETURNING`, which carries every column of the table because
	//! DuckDB's RETURNING projection sits above the operator and expects the
	//! table's full width — so one undecodable column fails a statement that
	//! never mentioned it. When OUTPUT copied only two of the four, a `text`
	//! value still failed the whole INSERT with `Unsupported type in RowReader:
	//! TEXT` while the very same column read fine through a SELECT.
	//!
	//!   - spatial UDTs -> `.STAsBinary()`, so the wire carries OGC WKB rather
	//!     than SQL Server's own Spatial Type Binary Format;
	//!   - `ntext` -> `NVARCHAR(MAX)`, `image` -> `VARBINARY(MAX)`: no codec
	//!     handles their wire tokens;
	//!   - a type with no decodable wire form at all (sql_variant, hierarchyid,
	//!     a CLR UDT) -> `NVARCHAR(MAX)`;
	//!   - a non-UTF-8 CHAR/VARCHAR/TEXT -> `NVARCHAR(n)`, because a DuckDB
	//!     VARCHAR is UTF-8 by contract.
	//!
	//! Every flag it needs is a pure function of the type name and the
	//! collation, which is why it takes those rather than an MSSQLColumnInfo:
	//! the INSERT path holds an MSSQLInsertColumn and would otherwise have to
	//! grow a copy of this logic, which is exactly what went wrong.
	//!
	//! @param qualifier prefix for the column REFERENCE, e.g. `"INSERTED."`.
	//!                  The alias is never qualified, so the result always comes
	//!                  back under the column's own name.
	static string BuildReadExpression(const string &col_name, const string &sql_type_name, int16_t max_length,
									  const string &collation_name, bool convert_varchar_max,
									  const string &qualifier = "");
};

}  // namespace duckdb
