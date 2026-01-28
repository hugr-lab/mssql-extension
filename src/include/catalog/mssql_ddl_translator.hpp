#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/constraint.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"

namespace duckdb {

// Forward declaration for CTAS config
namespace mssql {
struct CTASConfig;
struct CTASColumnDef;
}  // namespace mssql

//===----------------------------------------------------------------------===//
// DDLOperation - Identifies the type of DDL operation for error messages
//===----------------------------------------------------------------------===//

enum class DDLOperation : uint8_t {
	CREATE_SCHEMA,
	DROP_SCHEMA,
	CREATE_TABLE,
	DROP_TABLE,
	RENAME_TABLE,
	ADD_COLUMN,
	DROP_COLUMN,
	RENAME_COLUMN,
	ALTER_COLUMN_TYPE,
	ALTER_COLUMN_NULL
};

//! Convert DDLOperation to string for error messages
const char *DDLOperationToString(DDLOperation op);

//===----------------------------------------------------------------------===//
// MSSQLDDLTranslator - Translates DuckDB DDL operations to T-SQL
//
// This class provides static methods for generating T-SQL statements from
// DuckDB catalog DDL operations. All identifiers are properly quoted using
// SQL Server bracket notation with ] escaped as ]].
//===----------------------------------------------------------------------===//

class MSSQLDDLTranslator {
public:
	//===----------------------------------------------------------------------===//
	// Identifier Quoting
	//===----------------------------------------------------------------------===//

	//! Quote a SQL Server identifier using bracket notation
	//! @param identifier The identifier to quote
	//! @return Quoted identifier with ] escaped as ]]
	static string QuoteIdentifier(const string &identifier);

	//===----------------------------------------------------------------------===//
	// Schema Operations
	//===----------------------------------------------------------------------===//

	//! Generate CREATE SCHEMA T-SQL
	//! @param schema_name Schema to create
	//! @return T-SQL statement
	static string TranslateCreateSchema(const string &schema_name);

	//! Generate DROP SCHEMA T-SQL
	//! @param schema_name Schema to drop
	//! @return T-SQL statement
	static string TranslateDropSchema(const string &schema_name);

	//===----------------------------------------------------------------------===//
	// Table Operations
	//===----------------------------------------------------------------------===//

	//! Generate CREATE TABLE T-SQL from column definitions
	//! @param schema_name Target schema
	//! @param table_name Target table name
	//! @param columns Column list with definitions
	//! @return T-SQL statement
	static string TranslateCreateTable(const string &schema_name, const string &table_name, const ColumnList &columns);

	//! Generate CREATE TABLE T-SQL from column definitions and constraints
	//! @param schema_name Target schema
	//! @param table_name Target table name
	//! @param columns Column list with definitions
	//! @param constraints Table constraints (PRIMARY KEY, UNIQUE, etc.)
	//! @return T-SQL statement
	static string TranslateCreateTable(const string &schema_name, const string &table_name, const ColumnList &columns,
									   const vector<unique_ptr<Constraint>> &constraints);

	//! Generate DROP TABLE T-SQL
	//! @param schema_name Schema containing table
	//! @param table_name Table to drop
	//! @return T-SQL statement
	static string TranslateDropTable(const string &schema_name, const string &table_name);

	//! Generate RENAME TABLE T-SQL via sp_rename
	//! @param schema_name Schema containing table
	//! @param old_name Current table name
	//! @param new_name New table name
	//! @return T-SQL statement
	static string TranslateRenameTable(const string &schema_name, const string &old_name, const string &new_name);

	//===----------------------------------------------------------------------===//
	// Column Operations
	//===----------------------------------------------------------------------===//

	//! Generate ADD COLUMN T-SQL
	//! @param schema_name Schema containing table
	//! @param table_name Table to modify
	//! @param column Column definition
	//! @return T-SQL statement
	static string TranslateAddColumn(const string &schema_name, const string &table_name,
									 const ColumnDefinition &column);

	//! Generate DROP COLUMN T-SQL
	//! @param schema_name Schema containing table
	//! @param table_name Table to modify
	//! @param column_name Column to drop
	//! @return T-SQL statement
	static string TranslateDropColumn(const string &schema_name, const string &table_name, const string &column_name);

	//! Generate RENAME COLUMN T-SQL via sp_rename
	//! @param schema_name Schema containing table
	//! @param table_name Table containing column
	//! @param old_name Current column name
	//! @param new_name New column name
	//! @return T-SQL statement
	static string TranslateRenameColumn(const string &schema_name, const string &table_name, const string &old_name,
										const string &new_name);

	//! Generate ALTER COLUMN T-SQL for type change
	//! @param schema_name Schema containing table
	//! @param table_name Table containing column
	//! @param column_name Column to alter
	//! @param new_type New DuckDB type
	//! @param is_nullable Whether column allows nulls
	//! @return T-SQL statement
	static string TranslateAlterColumnType(const string &schema_name, const string &table_name,
										   const string &column_name, const LogicalType &new_type, bool is_nullable);

	//! Generate ALTER COLUMN T-SQL for nullability change
	//! @param schema_name Schema containing table
	//! @param table_name Table containing column
	//! @param column_name Column to alter
	//! @param current_type Current column type
	//! @param set_not_null True to set NOT NULL, false for NULL
	//! @return T-SQL statement
	static string TranslateAlterColumnNullability(const string &schema_name, const string &table_name,
												  const string &column_name, const LogicalType &current_type,
												  bool set_not_null);

	//===----------------------------------------------------------------------===//
	// Type Mapping
	//===----------------------------------------------------------------------===//

	//! Map DuckDB LogicalType to SQL Server type string
	//! @param type DuckDB type
	//! @return SQL Server type string
	static string MapTypeToSQLServer(const LogicalType &type);

	//===----------------------------------------------------------------------===//
	// CTAS-Specific Methods
	//===----------------------------------------------------------------------===//

	//! Map DuckDB LogicalType to SQL Server type for CTAS
	//! Respects CTASConfig.text_type for VARCHAR mapping
	//! @param type DuckDB type
	//! @param config CTAS configuration
	//! @return SQL Server type string
	//! @throws NotImplementedException for unsupported types (LIST, STRUCT, MAP, etc.)
	static string MapLogicalTypeToCTAS(const LogicalType &type, const mssql::CTASConfig &config);

	//! Generate CREATE TABLE SQL from CTAS column definitions
	//! @param schema_name Target schema
	//! @param table_name Target table name
	//! @param columns CTAS column definitions with mssql_type already resolved
	//! @return T-SQL CREATE TABLE statement
	static string TranslateCreateTableFromSchema(const string &schema_name, const string &table_name,
												 const vector<mssql::CTASColumnDef> &columns);

	//===----------------------------------------------------------------------===//
	// String Utilities
	//===----------------------------------------------------------------------===//

	//! Escape a string for use in N'...' literals (doubles single quotes)
	static string EscapeStringLiteral(const string &value);

private:
	//! Build column definition string for CREATE TABLE or ADD COLUMN
	static string BuildColumnDefinition(const ColumnDefinition &column);
};

}  // namespace duckdb
