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

//! A column's declaration as the server holds it, read from sys.columns / sys.types at the ALTER (not
//! the catalog's cache, which another client or mssql_exec may have outdated): what ALTER COLUMN, which
//! declares the column whole, re-declares (issue #577).
struct MSSQLServerColumnType {
	string type_name;	 // TYPE_NAME(user_type_id): an alias or CLR type's own name
	string type_schema;	 // its schema
	string system_type;	 // TYPE_NAME(system_type_id): the type underneath an alias; empty for a CLR type
	bool user_defined = false;
	bool assembly = false;	 // a CLR type (geometry, geography, hierarchyid, or a user's)
	int16_t max_length = 0;	 // bytes, -1 MAX
	uint8_t precision = 0;
	uint8_t scale = 0;
	string collation;
	bool nullable = true;
	bool sparse = false;
	int32_t xml_collection_id = 0;	// a typed xml column's schema collection (0 none), its name and schema
	string xml_collection;
	string xml_collection_schema;
	bool xml_document = false;
};

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
	static string TranslateCreateTable(const string &schema_name, const string &table_name, const ColumnList &columns,
									   const string &varchar_collation = "");

	//! Generate CREATE TABLE T-SQL from column definitions and constraints
	//! @param schema_name Target schema
	//! @param table_name Target table name
	//! @param columns Column list with definitions
	//! @param constraints Table constraints (PRIMARY KEY, UNIQUE, etc.)
	//! @return T-SQL statement
	static string TranslateCreateTable(const string &schema_name, const string &table_name, const ColumnList &columns,
									   const vector<unique_ptr<Constraint>> &constraints,
									   const string &varchar_collation = "");

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

	//! Generate ALTER COLUMN T-SQL for type change. SQL Server's ALTER COLUMN declares the column whole,
	//! so the caller passes the column's own nullability: a literal NULL made a NOT NULL column
	//! nullable (issue #577).
	//! @param schema_name Schema containing table
	//! @param table_name Table containing column
	//! @param column_name Column to alter
	//! @param new_type New DuckDB type
	//! @param is_nullable Whether column allows nulls -- the column's current answer
	//! @param collation The column's collation, kept when the new type is a character type that names none
	//! (empty: none kept)
	//! @return T-SQL statement
	//! @param sparse Whether the column is SPARSE -- kept (review of #586)
	static string TranslateAlterColumnType(const string &schema_name, const string &table_name,
										   const string &column_name, const LogicalType &new_type, bool is_nullable,
										   const string &collation = string(), bool sparse = false);

	//! Generate ALTER COLUMN T-SQL for nullability change. The column is re-declared as the server
	//! declares it (ColumnDeclaration), so only its nullability changes: its DuckDB type re-declared a
	//! money column decimal(19,4), a varchar(n) varchar(max), dropped its collation (issue #577).
	//! @param schema_name Schema containing table
	//! @param table_name Table containing column
	//! @param column_name Column to alter
	//! @param declaration The column's type as the server declares it (ColumnDeclaration)
	//! @param set_not_null True to set NOT NULL, false for NULL
	//! @return T-SQL statement
	static string TranslateAlterColumnNullability(const string &schema_name, const string &table_name,
												  const string &column_name, const string &declaration,
												  bool set_not_null);

	//! A column's type as the server declares it, its nullability left out: an alias or user CLR type by
	//! its quoted, schema-qualified name (no COLLATE: the server refuses one there, 452); a system type
	//! with its length (bytes; characters for nchar / nvarchar; max), precision and scale, or
	//! fractional-second scale; a typed xml with its schema collection; a string type's collation; SPARSE.
	static string ColumnDeclaration(const MSSQLServerColumnType &column);
	//! The query reading `column` of `schema`.`table` for ColumnDeclaration (one row, or none): type,
	//! type schema, user defined, assembly, system type, max length, precision, scale, collation,
	//! nullable, sparse, xml document, xml collection id.
	static string ServerColumnTypeQuery(const string &schema_name, const string &table_name, const string &column_name);
	//! ServerColumnTypeQuery's row, as strings (NULL as empty).
	static MSSQLServerColumnType ParseServerColumnType(const std::vector<string> &row);
	//! Whether a declaration's type is a character type (char, varchar, nchar, nvarchar, text, ntext).
	static bool IsCharacterType(const string &system_type);

	//===----------------------------------------------------------------------===//
	// Type Mapping
	//===----------------------------------------------------------------------===//

	//! Map DuckDB LogicalType to SQL Server type string
	//! @param type DuckDB type
	//! @return SQL Server type string
	//! @param varchar_collation collation for an MSSQL_VARCHAR(n) column that
	//!        names none of its own; empty emits no COLLATE clause
	static string MapTypeToSQLServer(const LogicalType &type, const string &varchar_collation = "");

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
	static string BuildColumnDefinition(const ColumnDefinition &column, const string &varchar_collation = "");
};

}  // namespace duckdb
