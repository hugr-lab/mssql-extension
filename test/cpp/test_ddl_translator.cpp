// test/cpp/test_ddl_translator.cpp
// Unit tests for MSSQLDDLTranslator
//
// These tests do NOT require a running SQL Server instance.
// They test the T-SQL generation logic in isolation.
//
// Compile:
//   See Makefile or CMakeLists.txt
//
// Run:
//   ./test_ddl_translator

#include <cassert>
#include <cstring>
#include <iostream>

#include "catalog/mssql_ddl_translator.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "query/mssql_identifier.hpp"

using namespace duckdb;

//==============================================================================
// Helper macros for assertions with messages
//==============================================================================
#define ASSERT_EQ(actual, expected)                                                          \
	do {                                                                                     \
		if ((actual) != (expected)) {                                                        \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  Expected: " << (expected) << std::endl;                          \
			std::cerr << "  Actual:   " << (actual) << std::endl;                            \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

#define ASSERT_CONTAINS(str, substr)                                                         \
	do {                                                                                     \
		if ((str).find(substr) == std::string::npos) {                                       \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  String does not contain: " << (substr) << std::endl;             \
			std::cerr << "  Actual string: " << (str) << std::endl;                          \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

//==============================================================================
// Test: QuoteIdentifier - Basic identifiers
//==============================================================================
void test_quote_identifier_basic() {
	std::cout << "\n=== Test: QuoteIdentifier - Basic ===" << std::endl;

	// Simple identifier
	ASSERT_EQ(mssql::QuoteIdentifier("foo"), "[foo]");

	// With spaces
	ASSERT_EQ(mssql::QuoteIdentifier("my table"), "[my table]");

	// With numbers
	ASSERT_EQ(mssql::QuoteIdentifier("table123"), "[table123]");

	// Empty identifier
	ASSERT_EQ(mssql::QuoteIdentifier(""), "[]");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: QuoteIdentifier - Special characters
//==============================================================================
void test_quote_identifier_special() {
	std::cout << "\n=== Test: QuoteIdentifier - Special Characters ===" << std::endl;

	// Contains closing bracket - must be escaped as ]]
	ASSERT_EQ(mssql::QuoteIdentifier("foo]bar"), "[foo]]bar]");

	// Multiple closing brackets
	ASSERT_EQ(mssql::QuoteIdentifier("a]b]c"), "[a]]b]]c]");

	// Closing bracket at start
	ASSERT_EQ(mssql::QuoteIdentifier("]foo"), "[]]foo]");

	// Closing bracket at end
	ASSERT_EQ(mssql::QuoteIdentifier("foo]"), "[foo]]]");

	// Only closing bracket: [ + the doubled ] + the closing ] = four characters
	ASSERT_EQ(mssql::QuoteIdentifier("]"), "[]]]");

	// Opening bracket (no escaping needed for [)
	ASSERT_EQ(mssql::QuoteIdentifier("foo[bar"), "[foo[bar]");

	// Unicode characters
	ASSERT_EQ(mssql::QuoteIdentifier("tbl_name"), "[tbl_name]");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: MapTypeToSQLServer - Integer types
//==============================================================================
void test_map_type_integers() {
	std::cout << "\n=== Test: MapTypeToSQLServer - Integers ===" << std::endl;

	// Signed integers
	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::BOOLEAN), "BIT");
	// Signed TINYINT widens to SMALLINT, deliberately (spec 057): SQL Server's
	// `tinyint` is its ONLY unsigned integer (0..255) while DuckDB's TINYINT is
	// signed (-128..127), so the narrow mapping silently turned -1 into 255 on
	// the wire. UTINYINT is the type that actually matches `tinyint`.
	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::TINYINT), "SMALLINT");
	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::SMALLINT), "SMALLINT");
	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::INTEGER), "INT");
	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::BIGINT), "BIGINT");

	// Unsigned integers - map to next larger signed type
	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::UTINYINT), "TINYINT");
	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::USMALLINT), "INT");
	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::UINTEGER), "BIGINT");
	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::UBIGINT), "DECIMAL(20,0)");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: MapTypeToSQLServer - Float/Decimal types
//==============================================================================
void test_map_type_floats() {
	std::cout << "\n=== Test: MapTypeToSQLServer - Floats/Decimals ===" << std::endl;

	// Floats
	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::FLOAT), "REAL");
	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::DOUBLE), "FLOAT");

	// Decimals
	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::DECIMAL(10, 2)), "DECIMAL(10,2)");
	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::DECIMAL(38, 10)), "DECIMAL(38,10)");

	// HUGEINT maps to max decimal
	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::HUGEINT), "DECIMAL(38,0)");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: MapTypeToSQLServer - String/Binary types
//==============================================================================
void test_map_type_strings() {
	std::cout << "\n=== Test: MapTypeToSQLServer - Strings/Binary ===" << std::endl;

	// VARCHAR maps to NVARCHAR for Unicode safety
	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::VARCHAR), "NVARCHAR(MAX)");

	// BLOB maps to VARBINARY
	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::BLOB), "VARBINARY(MAX)");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: MapTypeToSQLServer - Date/Time types
//==============================================================================
void test_map_type_datetime() {
	std::cout << "\n=== Test: MapTypeToSQLServer - Date/Time ===" << std::endl;

	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::DATE), "DATE");
	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::TIME), "TIME(6)");
	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::TIMESTAMP), "DATETIME2(6)");
	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::TIMESTAMP_TZ), "DATETIMEOFFSET(7)");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: MapTypeToSQLServer - UUID
//==============================================================================
void test_map_type_uuid() {
	std::cout << "\n=== Test: MapTypeToSQLServer - UUID ===" << std::endl;

	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::UUID), "UNIQUEIDENTIFIER");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Schema DDL Translation
//==============================================================================
void test_schema_ddl() {
	std::cout << "\n=== Test: Schema DDL Translation ===" << std::endl;

	// CREATE SCHEMA
	ASSERT_EQ(MSSQLDDLTranslator::TranslateCreateSchema("test_schema"), "CREATE SCHEMA [test_schema];");

	// CREATE SCHEMA with special characters
	ASSERT_EQ(MSSQLDDLTranslator::TranslateCreateSchema("my]schema"), "CREATE SCHEMA [my]]schema];");

	// DROP SCHEMA
	ASSERT_EQ(MSSQLDDLTranslator::TranslateDropSchema("old_schema"), "DROP SCHEMA [old_schema];");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Table DDL Translation - DROP TABLE
//==============================================================================
void test_drop_table() {
	std::cout << "\n=== Test: DROP TABLE Translation ===" << std::endl;

	ASSERT_EQ(MSSQLDDLTranslator::TranslateDropTable("dbo", "users"), "DROP TABLE [dbo].[users];");

	// With special characters
	ASSERT_EQ(MSSQLDDLTranslator::TranslateDropTable("my]schema", "my]table"), "DROP TABLE [my]]schema].[my]]table];");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Table DDL Translation - RENAME TABLE
//==============================================================================
void test_rename_table() {
	std::cout << "\n=== Test: RENAME TABLE Translation ===" << std::endl;

	std::string sql = MSSQLDDLTranslator::TranslateRenameTable("dbo", "old_name", "new_name");

	// The old name's parts bracketed (sp_rename parses it as a multi-part
	// name), the new name literal.
	ASSERT_EQ(sql, "EXEC sp_rename N'[dbo].[old_name]', N'new_name';");
	ASSERT_EQ(MSSQLDDLTranslator::TranslateRenameTable("s.x", "a.b]'c", "n.m'"),
			  "EXEC sp_rename N'[s.x].[a.b]]''c]', N'n.m''';");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Column DDL Translation - DROP COLUMN
//==============================================================================
void test_drop_column() {
	std::cout << "\n=== Test: DROP COLUMN Translation ===" << std::endl;

	ASSERT_EQ(MSSQLDDLTranslator::TranslateDropColumn("dbo", "users", "old_column"),
			  "ALTER TABLE [dbo].[users] DROP COLUMN [old_column];");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Column DDL Translation - RENAME COLUMN
//==============================================================================
void test_rename_column() {
	std::cout << "\n=== Test: RENAME COLUMN Translation ===" << std::endl;

	std::string sql = MSSQLDDLTranslator::TranslateRenameColumn("dbo", "users", "old_col", "new_col");

	ASSERT_EQ(sql, "EXEC sp_rename N'[dbo].[users].[old_col]', N'new_col', N'COLUMN';");
	ASSERT_EQ(MSSQLDDLTranslator::TranslateRenameColumn("dbo", "t.1", "c.1", "d.1"),
			  "EXEC sp_rename N'[dbo].[t.1].[c.1]', N'd.1', N'COLUMN';");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Column DDL Translation - ALTER COLUMN TYPE
//==============================================================================
void test_alter_column_type() {
	std::cout << "\n=== Test: ALTER COLUMN TYPE Translation ===" << std::endl;

	std::string sql = MSSQLDDLTranslator::TranslateAlterColumnType("dbo", "users", "age", LogicalType::BIGINT, true);

	ASSERT_CONTAINS(sql, "ALTER TABLE [dbo].[users]");
	ASSERT_CONTAINS(sql, "ALTER COLUMN [age]");
	ASSERT_CONTAINS(sql, "BIGINT");
	ASSERT_CONTAINS(sql, "NULL");

	// NOT NULL case
	sql = MSSQLDDLTranslator::TranslateAlterColumnType("dbo", "users", "id", LogicalType::INTEGER, false);

	ASSERT_CONTAINS(sql, "NOT NULL");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Column DDL Translation - ALTER COLUMN NULLABILITY
//==============================================================================
void test_alter_column_nullability() {
	std::cout << "\n=== Test: ALTER COLUMN NULLABILITY Translation ===" << std::endl;

	// Set NOT NULL: the column re-declared as given, only its nullability changed (issue #577).
	std::string sql = MSSQLDDLTranslator::TranslateAlterColumnNullability(
		"dbo", "users", "email", "varchar(100) COLLATE Latin1_General_CI_AS", true);
	ASSERT_EQ(sql,
			  "ALTER TABLE [dbo].[users] ALTER COLUMN [email] varchar(100) COLLATE Latin1_General_CI_AS NOT NULL;");

	// Set NULL
	sql = MSSQLDDLTranslator::TranslateAlterColumnNullability("dbo", "users", "price", "money", false);
	ASSERT_EQ(sql, "ALTER TABLE [dbo].[users] ALTER COLUMN [price] money NULL;");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: a column's server declaration (issue #577)
//==============================================================================
void test_column_declaration() {
	std::cout << "\n=== Test: Column declaration as the server declares it ===" << std::endl;
	auto system = [](const std::string &type, int16_t max_length, uint8_t precision, uint8_t scale,
					 const std::string &collation) {
		MSSQLServerColumnType c;
		c.type_name = type;
		c.type_schema = "sys";
		c.system_type = type;
		c.max_length = max_length;
		c.precision = precision;
		c.scale = scale;
		c.collation = collation;
		return MSSQLDDLTranslator::ColumnDeclaration(c);
	};
	ASSERT_EQ(system("money", 8, 19, 4, ""), "money");
	ASSERT_EQ(system("datetime", 8, 23, 3, ""), "datetime");
	ASSERT_EQ(system("smalldatetime", 4, 16, 0, ""), "smalldatetime");
	ASSERT_EQ(system("datetime2", 7, 24, 4, ""), "datetime2(4)");
	ASSERT_EQ(system("datetimeoffset", 10, 34, 7, ""), "datetimeoffset(7)");
	ASSERT_EQ(system("time", 3, 11, 2, ""), "time(2)");
	ASSERT_EQ(system("decimal", 9, 12, 3, ""), "decimal(12, 3)");
	ASSERT_EQ(system("numeric", 5, 5, 0, ""), "numeric(5, 0)");
	ASSERT_EQ(system("varchar", 30, 0, 0, "Latin1_General_CS_AS"), "varchar(30) COLLATE Latin1_General_CS_AS");
	ASSERT_EQ(system("nvarchar", 60, 0, 0, "Latin1_General_CI_AS"), "nvarchar(30) COLLATE Latin1_General_CI_AS");
	ASSERT_EQ(system("nvarchar", -1, 0, 0, "Latin1_General_CI_AS"), "nvarchar(max) COLLATE Latin1_General_CI_AS");
	ASSERT_EQ(system("char", 10, 0, 0, "SQL_Latin1_General_CP1_CI_AS"),
			  "char(10) COLLATE SQL_Latin1_General_CP1_CI_AS");
	ASSERT_EQ(system("varbinary", -1, 0, 0, ""), "varbinary(max)");
	ASSERT_EQ(system("binary", 16, 0, 0, ""), "binary(16)");
	ASSERT_EQ(system("uniqueidentifier", 16, 0, 0, ""), "uniqueidentifier");
	ASSERT_EQ(system("geometry", -1, 0, 0, ""), "geometry");
	ASSERT_EQ(system("xml", -1, 0, 0, ""), "xml");

	// An alias type by its own qualified name (its length is its own), never with COLLATE (error 452).
	MSSQLServerColumnType alias;
	alias.type_name = "Name";
	alias.type_schema = "dbo";
	alias.system_type = "nvarchar";
	alias.user_defined = true;
	alias.max_length = 100;
	alias.collation = "Latin1_General_CI_AS";
	ASSERT_EQ(MSSQLDDLTranslator::ColumnDeclaration(alias), "[dbo].[Name]");
	// A user CLR type: its case and schema kept, no collation.
	MSSQLServerColumnType clr;
	clr.type_name = "Point";
	clr.type_schema = "geo";
	clr.user_defined = true;
	clr.assembly = true;
	ASSERT_EQ(MSSQLDDLTranslator::ColumnDeclaration(clr), "[geo].[Point]");
	// A typed xml, and a sparse column.
	MSSQLServerColumnType typed = MSSQLServerColumnType();
	typed.type_name = "xml";
	typed.system_type = "xml";
	typed.max_length = -1;
	typed.xml_collection_id = 65536;
	typed.xml_collection = "Orders";
	typed.xml_collection_schema = "dbo";
	typed.xml_document = true;
	ASSERT_EQ(MSSQLDDLTranslator::ColumnDeclaration(typed), "xml(DOCUMENT [dbo].[Orders])");
	typed.xml_document = false;
	ASSERT_EQ(MSSQLDDLTranslator::ColumnDeclaration(typed), "xml(CONTENT [dbo].[Orders])");
	MSSQLServerColumnType sparse;
	sparse.type_name = "int";
	sparse.system_type = "int";
	sparse.sparse = true;
	ASSERT_EQ(MSSQLDDLTranslator::ColumnDeclaration(sparse), "int SPARSE");

	// The server's row (NULL as empty).
	auto parsed = MSSQLDDLTranslator::ParseServerColumnType(
		{"Name", "dbo", "1", "0", "nvarchar", "100", "0", "0", "Latin1_General_CI_AS", "0", "0", "0", "0"});
	ASSERT_EQ(MSSQLDDLTranslator::ColumnDeclaration(parsed), "[dbo].[Name]");
	ASSERT_EQ(parsed.nullable ? "nullable" : "not null", std::string("not null"));

	// A type change keeps a character column's collation where the new type names none.
	ASSERT_EQ(MSSQLDDLTranslator::TranslateAlterColumnType("dbo", "t", "c", LogicalType::VARCHAR, false,
														   "Latin1_General_CS_AS"),
			  "ALTER TABLE [dbo].[t] ALTER COLUMN [c] " + MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::VARCHAR) +
				  " COLLATE Latin1_General_CS_AS NOT NULL;");
	ASSERT_EQ(MSSQLDDLTranslator::TranslateAlterColumnType("dbo", "t", "c", LogicalType::BIGINT, true,
														   "Latin1_General_CS_AS"),
			  "ALTER TABLE [dbo].[t] ALTER COLUMN [c] " + MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::BIGINT) +
				  " NULL;");
	// A sparse column stays sparse through a type change.
	ASSERT_EQ(MSSQLDDLTranslator::TranslateAlterColumnType("dbo", "t", "c", LogicalType::BIGINT, true, "", true),
			  "ALTER TABLE [dbo].[t] ALTER COLUMN [c] " + MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::BIGINT) +
				  " SPARSE NULL;");
	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: DDLOperation to String
//==============================================================================
void test_ddl_operation_to_string() {
	std::cout << "\n=== Test: DDLOperation to String ===" << std::endl;

	ASSERT_EQ(std::string(DDLOperationToString(DDLOperation::CREATE_SCHEMA)), "CREATE_SCHEMA");
	ASSERT_EQ(std::string(DDLOperationToString(DDLOperation::DROP_SCHEMA)), "DROP_SCHEMA");
	ASSERT_EQ(std::string(DDLOperationToString(DDLOperation::CREATE_TABLE)), "CREATE_TABLE");
	ASSERT_EQ(std::string(DDLOperationToString(DDLOperation::DROP_TABLE)), "DROP_TABLE");
	ASSERT_EQ(std::string(DDLOperationToString(DDLOperation::RENAME_TABLE)), "RENAME_TABLE");
	ASSERT_EQ(std::string(DDLOperationToString(DDLOperation::ADD_COLUMN)), "ADD_COLUMN");
	ASSERT_EQ(std::string(DDLOperationToString(DDLOperation::DROP_COLUMN)), "DROP_COLUMN");
	ASSERT_EQ(std::string(DDLOperationToString(DDLOperation::RENAME_COLUMN)), "RENAME_COLUMN");
	ASSERT_EQ(std::string(DDLOperationToString(DDLOperation::ALTER_COLUMN_TYPE)), "ALTER_COLUMN_TYPE");
	ASSERT_EQ(std::string(DDLOperationToString(DDLOperation::ALTER_COLUMN_NULL)), "ALTER_COLUMN_NULL");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Main
//==============================================================================
int main() {
	std::cout << "========================================" << std::endl;
	std::cout << "MSSQLDDLTranslator Unit Tests" << std::endl;
	std::cout << "========================================" << std::endl;

	try {
		// QuoteIdentifier tests
		test_quote_identifier_basic();
		test_quote_identifier_special();

		// MapTypeToSQLServer tests
		test_map_type_integers();
		test_map_type_floats();
		test_map_type_strings();
		test_map_type_datetime();
		test_map_type_uuid();

		// DDL Translation tests
		test_schema_ddl();
		test_drop_table();
		test_rename_table();
		test_drop_column();
		test_rename_column();
		test_alter_column_type();
		test_alter_column_nullability();
		test_column_declaration();

		// DDLOperation enum tests
		test_ddl_operation_to_string();

		std::cout << "\n========================================" << std::endl;
		std::cout << "ALL TESTS PASSED!" << std::endl;
		std::cout << "========================================" << std::endl;

		return 0;
	} catch (const std::exception &e) {
		std::cerr << "\n========================================" << std::endl;
		std::cerr << "TEST FAILED: " << e.what() << std::endl;
		std::cerr << "========================================" << std::endl;
		return 1;
	}
}
