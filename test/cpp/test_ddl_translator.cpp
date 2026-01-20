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
	ASSERT_EQ(MSSQLDDLTranslator::QuoteIdentifier("foo"), "[foo]");

	// With spaces
	ASSERT_EQ(MSSQLDDLTranslator::QuoteIdentifier("my table"), "[my table]");

	// With numbers
	ASSERT_EQ(MSSQLDDLTranslator::QuoteIdentifier("table123"), "[table123]");

	// Empty identifier
	ASSERT_EQ(MSSQLDDLTranslator::QuoteIdentifier(""), "[]");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: QuoteIdentifier - Special characters
//==============================================================================
void test_quote_identifier_special() {
	std::cout << "\n=== Test: QuoteIdentifier - Special Characters ===" << std::endl;

	// Contains closing bracket - must be escaped as ]]
	ASSERT_EQ(MSSQLDDLTranslator::QuoteIdentifier("foo]bar"), "[foo]]bar]");

	// Multiple closing brackets
	ASSERT_EQ(MSSQLDDLTranslator::QuoteIdentifier("a]b]c"), "[a]]b]]c]");

	// Closing bracket at start
	ASSERT_EQ(MSSQLDDLTranslator::QuoteIdentifier("]foo"), "[]]foo]");

	// Closing bracket at end
	ASSERT_EQ(MSSQLDDLTranslator::QuoteIdentifier("foo]"), "[foo]]]");

	// Only closing bracket
	ASSERT_EQ(MSSQLDDLTranslator::QuoteIdentifier("]"), "[]]]]");

	// Opening bracket (no escaping needed for [)
	ASSERT_EQ(MSSQLDDLTranslator::QuoteIdentifier("foo[bar"), "[foo[bar]");

	// Unicode characters
	ASSERT_EQ(MSSQLDDLTranslator::QuoteIdentifier("tbl_name"), "[tbl_name]");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: MapTypeToSQLServer - Integer types
//==============================================================================
void test_map_type_integers() {
	std::cout << "\n=== Test: MapTypeToSQLServer - Integers ===" << std::endl;

	// Signed integers
	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::BOOLEAN), "BIT");
	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::TINYINT), "TINYINT");
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
	ASSERT_EQ(MSSQLDDLTranslator::MapTypeToSQLServer(LogicalType::TIME), "TIME(7)");
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

	// Should use sp_rename
	ASSERT_CONTAINS(sql, "sp_rename");
	ASSERT_CONTAINS(sql, "dbo.old_name");
	ASSERT_CONTAINS(sql, "'new_name'");

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

	// Should use sp_rename with COLUMN parameter
	ASSERT_CONTAINS(sql, "sp_rename");
	ASSERT_CONTAINS(sql, "dbo.users.old_col");
	ASSERT_CONTAINS(sql, "'new_col'");
	ASSERT_CONTAINS(sql, "COLUMN");

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

	// Set NOT NULL
	std::string sql =
		MSSQLDDLTranslator::TranslateAlterColumnNullability("dbo", "users", "email", LogicalType::VARCHAR, true);

	ASSERT_CONTAINS(sql, "ALTER TABLE [dbo].[users]");
	ASSERT_CONTAINS(sql, "ALTER COLUMN [email]");
	ASSERT_CONTAINS(sql, "NOT NULL");

	// Set NULL
	sql = MSSQLDDLTranslator::TranslateAlterColumnNullability("dbo", "users", "phone", LogicalType::VARCHAR, false);

	ASSERT_CONTAINS(sql, "ALTER COLUMN [phone]");
	// Should have NULL but NOT "NOT NULL"
	ASSERT_CONTAINS(sql, " NULL");

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
