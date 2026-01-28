// test/cpp/test_ctas_type_mapping.cpp
// Unit tests for CTAS-specific type mapping in MSSQLDDLTranslator
//
// These tests do NOT require a running SQL Server instance.
// They test the MapLogicalTypeToCTAS method and CTAS DDL generation in isolation.
//
// Run:
//   ./build/release/test/unittest "*test_ctas_type_mapping*"

#include <cassert>
#include <cstring>
#include <iostream>

#include "catalog/mssql_ddl_translator.hpp"
#include "dml/ctas/mssql_ctas_config.hpp"
#include "dml/ctas/mssql_ctas_types.hpp"
#include "duckdb/common/types.hpp"

using namespace duckdb;
using namespace duckdb::mssql;

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

#define ASSERT_THROWS(expr, expected_msg)                                                    \
	do {                                                                                     \
		bool threw = false;                                                                  \
		std::string error_msg;                                                               \
		try {                                                                                \
			(expr);                                                                          \
		} catch (const std::exception &e) {                                                  \
			threw = true;                                                                    \
			error_msg = e.what();                                                            \
		}                                                                                    \
		if (!threw) {                                                                        \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  Expected exception with: " << (expected_msg) << std::endl;       \
			std::cerr << "  But no exception was thrown" << std::endl;                       \
			assert(false);                                                                   \
		}                                                                                    \
		if (error_msg.find(expected_msg) == std::string::npos) {                             \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  Expected message containing: " << (expected_msg) << std::endl;   \
			std::cerr << "  Actual message: " << error_msg << std::endl;                     \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

//==============================================================================
// Test: CTAS Integer Type Mapping
//==============================================================================
void test_ctas_integers() {
	std::cout << "\n=== Test: CTAS Integer Type Mapping ===" << std::endl;

	CTASConfig config;

	// Signed integers
	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::BOOLEAN, config), "BIT");
	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::TINYINT, config), "TINYINT");
	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::SMALLINT, config), "SMALLINT");
	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::INTEGER, config), "INT");
	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::BIGINT, config), "BIGINT");

	// Unsigned integers - map to appropriate SQL Server types
	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::UTINYINT, config), "TINYINT");
	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::USMALLINT, config), "INT");
	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::UINTEGER, config), "BIGINT");
	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::UBIGINT, config), "DECIMAL(20,0)");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: CTAS Float/Decimal Type Mapping
//==============================================================================
void test_ctas_floats() {
	std::cout << "\n=== Test: CTAS Float/Decimal Type Mapping ===" << std::endl;

	CTASConfig config;

	// Floats
	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::FLOAT, config), "REAL");
	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::DOUBLE, config), "FLOAT");

	// Decimals - various precision/scale
	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::DECIMAL(10, 2), config), "DECIMAL(10,2)");
	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::DECIMAL(18, 6), config), "DECIMAL(18,6)");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: CTAS Decimal Precision Clamping (FR-017)
//==============================================================================
void test_ctas_decimal_clamping() {
	std::cout << "\n=== Test: CTAS Decimal Precision Clamping ===" << std::endl;

	CTASConfig config;

	// SQL Server max precision is 38
	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::DECIMAL(38, 5), config), "DECIMAL(38,5)");
	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::DECIMAL(38, 38), config), "DECIMAL(38,38)");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: CTAS String Type Mapping with Text Policy
//==============================================================================
void test_ctas_strings_nvarchar() {
	std::cout << "\n=== Test: CTAS String Type Mapping - NVARCHAR ===" << std::endl;

	CTASConfig config;
	config.text_type = CTASTextType::NVARCHAR;	// Default

	// VARCHAR maps to NVARCHAR(MAX) by default
	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::VARCHAR, config), "NVARCHAR(MAX)");

	std::cout << "PASSED!" << std::endl;
}

void test_ctas_strings_varchar() {
	std::cout << "\n=== Test: CTAS String Type Mapping - VARCHAR ===" << std::endl;

	CTASConfig config;
	config.text_type = CTASTextType::VARCHAR;  // User-set to VARCHAR

	// VARCHAR maps to VARCHAR(MAX) when setting is VARCHAR
	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::VARCHAR, config), "VARCHAR(MAX)");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: CTAS Binary Type Mapping
//==============================================================================
void test_ctas_binary() {
	std::cout << "\n=== Test: CTAS Binary Type Mapping ===" << std::endl;

	CTASConfig config;

	// BLOB maps to VARBINARY(MAX)
	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::BLOB, config), "VARBINARY(MAX)");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: CTAS Date/Time Type Mapping
//==============================================================================
void test_ctas_datetime() {
	std::cout << "\n=== Test: CTAS Date/Time Type Mapping ===" << std::endl;

	CTASConfig config;

	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::DATE, config), "DATE");
	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::TIME, config), "TIME(7)");
	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::TIMESTAMP, config), "DATETIME2(7)");
	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::TIMESTAMP_TZ, config), "DATETIMEOFFSET(7)");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: CTAS UUID Type Mapping
//==============================================================================
void test_ctas_uuid() {
	std::cout << "\n=== Test: CTAS UUID Type Mapping ===" << std::endl;

	CTASConfig config;

	ASSERT_EQ(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::UUID, config), "UNIQUEIDENTIFIER");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: CTAS Unsupported Types (FR-012)
//==============================================================================
void test_ctas_unsupported_hugeint() {
	std::cout << "\n=== Test: CTAS Unsupported HUGEINT ===" << std::endl;

	CTASConfig config;

	ASSERT_THROWS(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::HUGEINT, config), "HUGEINT");

	std::cout << "PASSED!" << std::endl;
}

void test_ctas_unsupported_interval() {
	std::cout << "\n=== Test: CTAS Unsupported INTERVAL ===" << std::endl;

	CTASConfig config;

	ASSERT_THROWS(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::INTERVAL, config), "INTERVAL");

	std::cout << "PASSED!" << std::endl;
}

void test_ctas_unsupported_list() {
	std::cout << "\n=== Test: CTAS Unsupported LIST ===" << std::endl;

	CTASConfig config;

	ASSERT_THROWS(MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::LIST(LogicalType::INTEGER), config), "LIST");

	std::cout << "PASSED!" << std::endl;
}

void test_ctas_unsupported_struct() {
	std::cout << "\n=== Test: CTAS Unsupported STRUCT ===" << std::endl;

	CTASConfig config;

	child_list_t<LogicalType> struct_children;
	struct_children.push_back({"a", LogicalType::INTEGER});
	struct_children.push_back({"b", LogicalType::VARCHAR});
	LogicalType struct_type = LogicalType::STRUCT(struct_children);

	ASSERT_THROWS(MSSQLDDLTranslator::MapLogicalTypeToCTAS(struct_type, config), "STRUCT");

	std::cout << "PASSED!" << std::endl;
}

void test_ctas_unsupported_map() {
	std::cout << "\n=== Test: CTAS Unsupported MAP ===" << std::endl;

	CTASConfig config;

	LogicalType map_type = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::INTEGER);

	ASSERT_THROWS(MSSQLDDLTranslator::MapLogicalTypeToCTAS(map_type, config), "MAP");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: TranslateCreateTableFromSchema
//==============================================================================
void test_ctas_create_table_ddl() {
	std::cout << "\n=== Test: CTAS CREATE TABLE DDL Generation ===" << std::endl;

	vector<CTASColumnDef> columns;

	CTASColumnDef col1;
	col1.name = "id";
	col1.duckdb_type = LogicalType::INTEGER;
	col1.mssql_type = "INT";
	col1.nullable = false;
	columns.push_back(col1);

	CTASColumnDef col2;
	col2.name = "name";
	col2.duckdb_type = LogicalType::VARCHAR;
	col2.mssql_type = "NVARCHAR(MAX)";
	col2.nullable = true;
	columns.push_back(col2);

	string ddl = MSSQLDDLTranslator::TranslateCreateTableFromSchema("dbo", "test_table", columns);

	ASSERT_CONTAINS(ddl, "CREATE TABLE [dbo].[test_table]");
	ASSERT_CONTAINS(ddl, "[id] INT NOT NULL");
	ASSERT_CONTAINS(ddl, "[name] NVARCHAR(MAX) NULL");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: CTAS DDL with special column names (FR-010)
//==============================================================================
void test_ctas_ddl_special_names() {
	std::cout << "\n=== Test: CTAS DDL with Special Column Names ===" << std::endl;

	vector<CTASColumnDef> columns;

	CTASColumnDef col1;
	col1.name = "column with spaces";
	col1.duckdb_type = LogicalType::INTEGER;
	col1.mssql_type = "INT";
	col1.nullable = true;
	columns.push_back(col1);

	CTASColumnDef col2;
	col2.name = "column]with]brackets";
	col2.duckdb_type = LogicalType::VARCHAR;
	col2.mssql_type = "NVARCHAR(MAX)";
	col2.nullable = true;
	columns.push_back(col2);

	string ddl = MSSQLDDLTranslator::TranslateCreateTableFromSchema("dbo", "test_table", columns);

	// Verify column names are properly bracket-escaped
	ASSERT_CONTAINS(ddl, "[column with spaces]");
	ASSERT_CONTAINS(ddl, "[column]]with]]brackets]");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Main
//==============================================================================
int main() {
	std::cout << "========================================" << std::endl;
	std::cout << "CTAS Type Mapping Unit Tests" << std::endl;
	std::cout << "========================================" << std::endl;

	try {
		// Supported type mapping
		test_ctas_integers();
		test_ctas_floats();
		test_ctas_decimal_clamping();
		test_ctas_strings_nvarchar();
		test_ctas_strings_varchar();
		test_ctas_binary();
		test_ctas_datetime();
		test_ctas_uuid();

		// Unsupported types
		test_ctas_unsupported_hugeint();
		test_ctas_unsupported_interval();
		test_ctas_unsupported_list();
		test_ctas_unsupported_struct();
		test_ctas_unsupported_map();

		// DDL generation
		test_ctas_create_table_ddl();
		test_ctas_ddl_special_names();

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
