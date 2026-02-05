#pragma once

#include <array>
#include <string>
#include <vector>
#include "duckdb/common/types.hpp"

namespace duckdb {

class ClientContext;

namespace tds {
class TdsConnection;
}  // namespace tds

namespace mssql {

struct BCPCopyConfig;

//===----------------------------------------------------------------------===//
// BCPCopyTarget - Resolved destination for a COPY operation
//
// Contains all information about the target table including:
// - Catalog/schema/table identifiers
// - Temp table flags
// - Fully qualified name for DDL generation
//===----------------------------------------------------------------------===//

struct BCPCopyTarget {
	// Name of attached MSSQL catalog
	string catalog_name;

	// SQL Server schema (e.g., "dbo")
	string schema_name;

	// Target table name
	string table_name;

	// True if starts with `#` (session-scoped temp table)
	bool is_temp_table = false;

	// True if starts with `##` (global temp table)
	bool is_global_temp = false;

	// Default constructor
	BCPCopyTarget() = default;

	// Constructor with components
	BCPCopyTarget(string catalog, string schema, string table)
		: catalog_name(std::move(catalog)), schema_name(std::move(schema)), table_name(std::move(table)) {
		DetectTempTable();
	}

	//===----------------------------------------------------------------------===//
	// Accessors
	//===----------------------------------------------------------------------===//

	// Get fully qualified table name for SQL generation
	// Returns: [schema].[table] (catalog is handled by connection context)
	string GetFullyQualifiedName() const;

	// Get bracketed schema name
	string GetBracketedSchema() const;

	// Get bracketed table name
	string GetBracketedTable() const;

	// Check if this is any type of temp table
	bool IsTempTable() const {
		return is_temp_table || is_global_temp;
	}

	// Detect temp table from table name prefix
	// Call this after setting table_name if not using the constructor
	void DetectTempTable();
};

//===----------------------------------------------------------------------===//
// BCPColumnMetadata - Column definition for BulkLoadBCP COLMETADATA token
//
// Contains all information needed to encode column metadata and row data
// in TDS BulkLoadBCP wire format.
//===----------------------------------------------------------------------===//

struct BCPColumnMetadata {
	// Column name (will be UTF-16LE encoded in wire format)
	string name;

	// Source DuckDB type
	LogicalType duckdb_type;

	// TDS type identifier (e.g., 0x26 for INTNTYPE)
	uint8_t tds_type_token = 0;

	// Maximum data length in bytes
	uint16_t max_length = 0;

	// Precision for DECIMAL/NUMERIC types (1-38)
	uint8_t precision = 0;

	// Scale for DECIMAL/NUMERIC/TIME types
	uint8_t scale = 0;

	// Whether column accepts NULL values
	bool nullable = true;

	// Collation bytes for character types (5 bytes)
	std::array<uint8_t, 5> collation = {0x09, 0x04, 0xD0, 0x00, 0x34};	// Latin1_General_CI_AS default

	// Default constructor
	BCPColumnMetadata() = default;

	// Constructor with basic fields
	BCPColumnMetadata(string col_name, LogicalType type, bool is_nullable = true)
		: name(std::move(col_name)), duckdb_type(std::move(type)), nullable(is_nullable) {}

	//===----------------------------------------------------------------------===//
	// Wire Format Helpers
	//===----------------------------------------------------------------------===//

	// Get COLMETADATA flags (USHORT)
	// Bit 0: fNullable (0x0001)
	// Bits 2-3: usUpdateable (0x0008 for updatable)
	uint16_t GetFlags() const {
		uint16_t flags = 0x0008;  // usUpdateable = read/write
		if (nullable) {
			flags |= 0x0001;  // fNullable
		}
		return flags;
	}

	// Check if this is a variable-length type using USHORTLEN
	bool IsVariableLengthUSHORT() const;

	// Check if this is a PLP (Partially Length-prefixed) type (MAX types)
	// Returns true if max_length == 0xFFFF, indicating nvarchar(max) or varbinary(max)
	bool IsPLPType() const {
		return max_length == 0xFFFF;
	}

	// Check if this is a fixed-length type
	bool IsFixedLength() const;

	// Get the wire size for the length prefix (0 for fixed, 1 for BYTELEN, 2 for USHORTLEN)
	uint8_t GetLengthPrefixSize() const;

	// Get SQL Server type declaration for INSERT BULK statement
	// Returns the exact type matching the target column (e.g., "nvarchar(50)", "int")
	string GetSQLServerTypeDeclaration() const;
};

//===----------------------------------------------------------------------===//
// TargetResolver - Resolves and validates COPY target destinations
//
// Handles both URL-based (mssql://...) and catalog-based (catalog.schema.table)
// target specifications. Validates target existence, type (table vs view),
// and schema compatibility.
//===----------------------------------------------------------------------===//

struct TargetResolver {
	//===----------------------------------------------------------------------===//
	// Target Resolution
	//===----------------------------------------------------------------------===//

	// Resolve target from URL format: mssql://<alias>/<schema>/<table>
	// or mssql://<alias>/#temp_table
	// @param context DuckDB client context for catalog lookup
	// @param url The target URL
	// @return Resolved BCPCopyTarget
	// @throws InvalidInputException for malformed URLs or unknown catalogs
	static BCPCopyTarget ResolveURL(ClientContext &context, const string &url);

	// Resolve target from catalog syntax: catalog.schema.table
	// @param context DuckDB client context for catalog lookup
	// @param catalog Catalog name (attached database alias)
	// @param schema Schema name (e.g., "dbo") - may be empty for temp tables
	// @param table Table name
	// @param allow_empty_schema If true, empty schema allowed for temp tables (catalog..#temp)
	// @return Resolved BCPCopyTarget
	// @throws InvalidInputException for unknown catalogs or invalid empty schema usage
	static BCPCopyTarget ResolveCatalog(ClientContext &context, const string &catalog, const string &schema,
										const string &table, bool allow_empty_schema = false);

	//===----------------------------------------------------------------------===//
	// Target Validation
	//===----------------------------------------------------------------------===//

	// Validate target and optionally create/recreate table
	// - Checks if target is a table (not a view)
	// - Handles CREATE_TABLE and OVERWRITE options
	// - Validates schema compatibility if table exists
	// - Sets config.is_new_table if table was created or recreated (Issue #45)
	// @param context DuckDB client context
	// @param conn TDS connection for SQL execution
	// @param target The target to validate
	// @param config COPY configuration options (is_new_table may be modified)
	// @param source_types DuckDB types from the source query
	// @param source_names Column names from the source query
	// @throws InvalidInputException for validation failures
	static void ValidateTarget(ClientContext &context, tds::TdsConnection &conn, BCPCopyTarget &target,
							   BCPCopyConfig &config, const vector<LogicalType> &source_types,
							   const vector<string> &source_names);

	//===----------------------------------------------------------------------===//
	// Table Management
	//===----------------------------------------------------------------------===//

	// Create a new table based on source schema
	// @param conn TDS connection for SQL execution
	// @param target The target table
	// @param source_types DuckDB types to map to SQL Server
	// @param source_names Column names
	static void CreateTable(tds::TdsConnection &conn, const BCPCopyTarget &target,
							const vector<LogicalType> &source_types, const vector<string> &source_names);

	// Drop a table if it exists
	// @param conn TDS connection for SQL execution
	// @param target The target table to drop
	static void DropTable(tds::TdsConnection &conn, const BCPCopyTarget &target);

	// Validate that existing table schema is compatible with source
	// @param conn TDS connection for SQL execution
	// @param target The target table
	// @param source_types DuckDB types from the source query
	// @param source_names Column names from the source query
	// @throws InvalidInputException if schema is incompatible
	static void ValidateExistingTableSchema(tds::TdsConnection &conn, const BCPCopyTarget &target,
											const vector<LogicalType> &source_types,
											const vector<string> &source_names);

	// Get column metadata for an existing table
	// Used when copying to existing table - BCP COLMETADATA must match target schema
	// @param conn TDS connection for SQL execution
	// @param target The target table
	// @return Vector of BCPColumnMetadata matching the target table's schema
	static vector<BCPColumnMetadata> GetExistingTableColumnMetadata(tds::TdsConnection &conn,
																	const BCPCopyTarget &target);

	//===----------------------------------------------------------------------===//
	// Column Mapping
	//===----------------------------------------------------------------------===//

	// Build column mapping from source columns to target columns by name
	// Returns a mapping where mapping[target_idx] = source_idx, or -1 if source doesn't have this column
	// @param source_names Column names from the source query
	// @param target_columns Target table column metadata
	// @return Vector of size target_columns.size() with source indices (-1 for missing)
	static vector<int32_t> BuildColumnMapping(const vector<string> &source_names,
											  const vector<BCPColumnMetadata> &target_columns);

	//===----------------------------------------------------------------------===//
	// Column Metadata Generation
	//===----------------------------------------------------------------------===//

	// Generate BCPColumnMetadata from DuckDB source types
	// Maps DuckDB types to appropriate TDS type tokens and parameters
	// @param source_types DuckDB logical types
	// @param source_names Column names
	// @return Vector of BCPColumnMetadata for COLMETADATA token generation
	static vector<BCPColumnMetadata> GenerateColumnMetadata(const vector<LogicalType> &source_types,
															const vector<string> &source_names);

	//===----------------------------------------------------------------------===//
	// Type Mapping
	//===----------------------------------------------------------------------===//

	// Get SQL Server type declaration for CREATE TABLE
	// @param duckdb_type DuckDB logical type
	// @return SQL Server type string (e.g., "int", "nvarchar(max)")
	static string GetSQLServerTypeDeclaration(const LogicalType &duckdb_type);

	// Map DuckDB type to TDS type token
	// @param duckdb_type DuckDB logical type
	// @return TDS type token (e.g., 0x26 for INTNTYPE)
	static uint8_t GetTDSTypeToken(const LogicalType &duckdb_type);

	// Get max_length for TDS type
	// @param duckdb_type DuckDB logical type
	// @return max_length value for COLMETADATA
	static uint16_t GetTDSMaxLength(const LogicalType &duckdb_type);
};

}  // namespace mssql
}  // namespace duckdb
