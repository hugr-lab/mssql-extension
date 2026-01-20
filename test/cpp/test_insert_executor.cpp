// test/cpp/test_insert_executor.cpp
// Unit tests for MSSQLInsertExecutor
//
// NOTE: These tests verify the executor's internal logic WITHOUT a real connection.
// Full end-to-end tests require a running SQL Server instance (see integration tests).
//
// Tests cover:
// - Executor construction
// - Statistics tracking
// - Batch builder initialization
// - Error handling
//
// Compile:
//   See Makefile or CMakeLists.txt
//
// Run:
//   ./test_insert_executor

#include <cassert>
#include <cstring>
#include <iostream>

#include "duckdb/common/types.hpp"
#include "insert/mssql_insert_config.hpp"
#include "insert/mssql_insert_error.hpp"
#include "insert/mssql_insert_executor.hpp"
#include "insert/mssql_insert_target.hpp"

using namespace duckdb;

//==============================================================================
// Helper macros for assertions
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

#define ASSERT_TRUE(expr)                                                                    \
	do {                                                                                     \
		if (!(expr)) {                                                                       \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  Expression was false: " << #expr << std::endl;                   \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

#define ASSERT_NEAR(actual, expected, tolerance)                                              \
	do {                                                                                      \
		if (std::abs((actual) - (expected)) > (tolerance)) {                                  \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl;  \
			std::cerr << "  Expected: " << (expected) << " +/- " << (tolerance) << std::endl; \
			std::cerr << "  Actual:   " << (actual) << std::endl;                             \
			assert(false);                                                                    \
		}                                                                                     \
	} while (0)

//==============================================================================
// Test Helpers
//==============================================================================
MSSQLInsertTarget CreateTestTarget() {
	MSSQLInsertTarget target;
	target.catalog_name = "test_catalog";
	target.schema_name = "dbo";
	target.table_name = "test_table";

	MSSQLInsertColumn col1;
	col1.name = "id";
	col1.duckdb_type = LogicalType::INTEGER;
	col1.mssql_type = "INT";
	col1.is_identity = false;
	target.columns.push_back(col1);

	MSSQLInsertColumn col2;
	col2.name = "name";
	col2.duckdb_type = LogicalType::VARCHAR;
	col2.mssql_type = "NVARCHAR(100)";
	col2.is_identity = false;
	target.columns.push_back(col2);

	target.insert_column_indices = {0, 1};
	return target;
}

MSSQLInsertConfig CreateTestConfig() {
	MSSQLInsertConfig config;
	config.batch_size = 100;
	config.max_rows_per_statement = 100;
	config.max_sql_bytes = 8 * 1024 * 1024;	 // 8MB
	config.use_returning_output = false;
	return config;
}

//==============================================================================
// Test: MSSQLInsertError formatting
//==============================================================================
void test_error_formatting() {
	std::cout << "\n=== Test: MSSQLInsertError Formatting ===" << std::endl;

	MSSQLInsertError error;
	error.statement_index = 5;
	error.row_offset_start = 100;
	error.row_offset_end = 150;
	error.sql_error_number = 2627;
	error.sql_error_message = "Violation of PRIMARY KEY constraint";

	auto message = error.FormatMessage();

	// Verify message contains key information
	ASSERT_TRUE(message.find("statement 5") != std::string::npos);
	ASSERT_TRUE(message.find("rows 100") != std::string::npos);
	ASSERT_TRUE(message.find("[2627]") != std::string::npos);
	ASSERT_TRUE(message.find("PRIMARY KEY") != std::string::npos);

	std::cout << "  Error message: " << message << std::endl;
	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: MSSQLInsertResult construction
//==============================================================================
void test_result_construction() {
	std::cout << "\n=== Test: MSSQLInsertResult Construction ===" << std::endl;

	// Success case
	MSSQLInsertResult success(100);
	ASSERT_TRUE(success.success);
	ASSERT_EQ(success.rows_affected, 100u);

	// Failure case
	MSSQLInsertError error;
	error.sql_error_number = 547;
	error.sql_error_message = "FOREIGN KEY constraint violation";
	MSSQLInsertResult failure(error);
	ASSERT_TRUE(!failure.success);
	ASSERT_EQ(failure.error.sql_error_number, 547);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: MSSQLInsertStatistics initial state
//==============================================================================
void test_statistics_initial() {
	std::cout << "\n=== Test: MSSQLInsertStatistics Initial State ===" << std::endl;

	MSSQLInsertStatistics stats;

	ASSERT_EQ(stats.total_rows_inserted, 0u);
	ASSERT_EQ(stats.total_batches_executed, 0u);
	ASSERT_EQ(stats.total_execution_time_us, 0u);
	ASSERT_EQ(stats.min_batch_size, 0u);
	ASSERT_EQ(stats.max_batch_size, 0u);
	ASSERT_EQ(stats.avg_batch_size, 0u);
	ASSERT_NEAR(stats.GetRowsPerSecond(), 0.0, 0.001);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: MSSQLInsertStatistics recording
//==============================================================================
void test_statistics_recording() {
	std::cout << "\n=== Test: MSSQLInsertStatistics Recording ===" << std::endl;

	MSSQLInsertStatistics stats;

	// Record first batch
	stats.RecordBatch(100, 5000, 10000);  // 100 rows, 5KB, 10ms

	ASSERT_EQ(stats.total_rows_inserted, 100u);
	ASSERT_EQ(stats.total_batches_executed, 1u);
	ASSERT_EQ(stats.total_execution_time_us, 10000u);
	ASSERT_EQ(stats.min_batch_size, 100u);
	ASSERT_EQ(stats.max_batch_size, 100u);
	ASSERT_EQ(stats.min_sql_bytes, 5000u);
	ASSERT_EQ(stats.max_sql_bytes, 5000u);

	// Record second batch (smaller)
	stats.RecordBatch(50, 2500, 5000);

	ASSERT_EQ(stats.total_rows_inserted, 150u);
	ASSERT_EQ(stats.total_batches_executed, 2u);
	ASSERT_EQ(stats.total_execution_time_us, 15000u);
	ASSERT_EQ(stats.min_batch_size, 50u);
	ASSERT_EQ(stats.max_batch_size, 100u);
	ASSERT_EQ(stats.min_sql_bytes, 2500u);
	ASSERT_EQ(stats.max_sql_bytes, 5000u);
	ASSERT_EQ(stats.avg_batch_size, 75u);  // 150/2

	// Record third batch (larger)
	stats.RecordBatch(200, 10000, 20000);

	ASSERT_EQ(stats.total_rows_inserted, 350u);
	ASSERT_EQ(stats.total_batches_executed, 3u);
	ASSERT_EQ(stats.min_batch_size, 50u);
	ASSERT_EQ(stats.max_batch_size, 200u);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: MSSQLInsertStatistics rows per second
//==============================================================================
void test_statistics_rows_per_second() {
	std::cout << "\n=== Test: MSSQLInsertStatistics Rows/Second ===" << std::endl;

	MSSQLInsertStatistics stats;

	// 1000 rows in 1 second (1,000,000 microseconds)
	stats.RecordBatch(1000, 50000, 1000000);

	double rps = stats.GetRowsPerSecond();
	ASSERT_NEAR(rps, 1000.0, 1.0);

	std::cout << "  Rows per second: " << rps << std::endl;

	// Add more: 2000 rows in 0.5 seconds
	stats.RecordBatch(2000, 100000, 500000);

	// Total: 3000 rows in 1.5 seconds = 2000 rows/sec
	rps = stats.GetRowsPerSecond();
	ASSERT_NEAR(rps, 2000.0, 1.0);

	std::cout << "  Rows per second (cumulative): " << rps << std::endl;
	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: MSSQLInsertException construction
//==============================================================================
void test_exception_construction() {
	std::cout << "\n=== Test: MSSQLInsertException Construction ===" << std::endl;

	MSSQLInsertError error;
	error.statement_index = 0;
	error.row_offset_start = 0;
	error.row_offset_end = 10;
	error.sql_error_number = 2627;
	error.sql_error_message = "Duplicate key";

	try {
		throw MSSQLInsertException(error);
	} catch (const MSSQLInsertException &e) {
		ASSERT_EQ(e.GetError().sql_error_number, 2627);
		ASSERT_TRUE(std::string(e.what()).find("2627") != std::string::npos);
		std::cout << "  Exception message: " << e.what() << std::endl;
	}

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Config validation
//==============================================================================
void test_config_validation() {
	std::cout << "\n=== Test: Config Validation ===" << std::endl;

	// Valid config
	MSSQLInsertConfig config;
	config.batch_size = 1000;
	config.max_rows_per_statement = 1000;
	config.max_sql_bytes = 8 * 1024 * 1024;
	config.Validate();	// Should not throw

	// Test effective rows calculation
	ASSERT_EQ(config.EffectiveRowsPerStatement(), 1000u);

	// With different batch_size and max_rows
	config.batch_size = 500;
	config.max_rows_per_statement = 1000;
	ASSERT_EQ(config.EffectiveRowsPerStatement(), 500u);  // min of the two

	config.batch_size = 1000;
	config.max_rows_per_statement = 500;
	ASSERT_EQ(config.EffectiveRowsPerStatement(), 500u);  // min of the two

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Config validation errors
//==============================================================================
void test_config_validation_errors() {
	std::cout << "\n=== Test: Config Validation Errors ===" << std::endl;

	// batch_size < 1
	MSSQLInsertConfig config;
	config.batch_size = 0;
	config.max_rows_per_statement = 1000;
	config.max_sql_bytes = 8192;

	bool threw = false;
	try {
		config.Validate();
	} catch (const InvalidInputException &) {
		threw = true;
	}
	ASSERT_TRUE(threw);

	// max_sql_bytes too small
	config.batch_size = 1000;
	config.max_sql_bytes = 512;	 // Less than minimum (1024)

	threw = false;
	try {
		config.Validate();
	} catch (const InvalidInputException &) {
		threw = true;
	}
	ASSERT_TRUE(threw);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Main
//==============================================================================
int main() {
	std::cout << "==========================================" << std::endl;
	std::cout << "MSSQLInsertExecutor Unit Tests" << std::endl;
	std::cout << "==========================================" << std::endl;

	try {
		// Error/Result tests
		test_error_formatting();
		test_result_construction();

		// Statistics tests
		test_statistics_initial();
		test_statistics_recording();
		test_statistics_rows_per_second();

		// Exception tests
		test_exception_construction();

		// Config tests
		test_config_validation();
		test_config_validation_errors();

		std::cout << "\n==========================================" << std::endl;
		std::cout << "ALL TESTS PASSED!" << std::endl;
		std::cout << "==========================================" << std::endl;
		return 0;

	} catch (const std::exception &e) {
		std::cerr << "\n==========================================" << std::endl;
		std::cerr << "TEST FAILED WITH EXCEPTION: " << e.what() << std::endl;
		std::cerr << "==========================================" << std::endl;
		return 1;
	}
}
