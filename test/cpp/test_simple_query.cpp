// test/cpp/test_simple_query.cpp
// Integration tests for MSSQLSimpleQuery
//
// This test requires a running SQL Server instance.
//
// Setup using Docker:
//   docker compose -f docker/docker-compose.yml up -d
//
// Set environment variables:
//   MSSQL_TEST_HOST:    SQL Server hostname (default: localhost)
//   MSSQL_TEST_PORT:    SQL Server port (default: 1433)
//   MSSQL_TEST_USER:    SQL Server username (default: sa)
//   MSSQL_TEST_PASS:    SQL Server password (required)
//   MSSQL_TEST_DB:      Database name (default: master)
//
// Compile:
//   See Makefile or CMakeLists.txt
//
// Run:
//   ./test_simple_query

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "query/mssql_simple_query.hpp"
#include "tds/tds_connection.hpp"

using namespace duckdb;
using namespace duckdb::tds;

// Get environment variable with default
std::string getenv_or(const char *name, const char *default_value) {
	const char *value = std::getenv(name);
	return value ? value : default_value;
}

// Connection parameters from environment
struct TestConfig {
	std::string host;
	uint16_t port;
	std::string user;
	std::string pass;
	std::string database;

	static TestConfig FromEnv() {
		TestConfig config;
		config.host = getenv_or("MSSQL_TEST_HOST", "localhost");
		config.port = static_cast<uint16_t>(std::stoi(getenv_or("MSSQL_TEST_PORT", "1433")));
		config.user = getenv_or("MSSQL_TEST_USER", "sa");
		config.pass = getenv_or("MSSQL_TEST_PASS", "");
		config.database = getenv_or("MSSQL_TEST_DB", "master");
		return config;
	}

	bool IsValid() const {
		return !pass.empty();
	}
};

// Helper to create and authenticate a connection
std::shared_ptr<TdsConnection> CreateConnection(const TestConfig &config, bool use_tls = false) {
	auto conn = std::make_shared<TdsConnection>();

	if (!conn->Connect(config.host, config.port)) {
		std::cerr << "Connect failed: " << conn->GetLastError() << std::endl;
		return nullptr;
	}

	if (!conn->Authenticate(config.user, config.pass, config.database, use_tls)) {
		std::cerr << "Auth failed: " << conn->GetLastError() << std::endl;
		return nullptr;
	}

	return conn;
}

//==============================================================================
// Test: Execute Scalar - Simple SELECT
//==============================================================================
void test_execute_scalar_simple(const TestConfig &config) {
	std::cout << "\n=== Test: ExecuteScalar - Simple SELECT ===" << std::endl;

	auto conn = CreateConnection(config);
	assert(conn != nullptr);

	// Test simple integer scalar
	std::string result = MSSQLSimpleQuery::ExecuteScalar(*conn, "SELECT 42");
	std::cout << "SELECT 42 => '" << result << "'" << std::endl;
	assert(result == "42");

	conn->Close();
	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Execute Scalar - String Value
//==============================================================================
void test_execute_scalar_string(const TestConfig &config) {
	std::cout << "\n=== Test: ExecuteScalar - String Value ===" << std::endl;

	auto conn = CreateConnection(config);
	assert(conn != nullptr);

	// Test NVARCHAR string
	std::string result = MSSQLSimpleQuery::ExecuteScalar(*conn, "SELECT N'Hello, World!'");
	std::cout << "SELECT N'Hello, World!' => '" << result << "'" << std::endl;
	assert(result == "Hello, World!");

	conn->Close();
	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Execute Scalar - Database Collation Query
//==============================================================================
void test_execute_scalar_collation(const TestConfig &config) {
	std::cout << "\n=== Test: ExecuteScalar - Database Collation ===" << std::endl;

	auto conn = CreateConnection(config);
	assert(conn != nullptr);

	// This is the actual query used in catalog initialization
	std::string sql = "SELECT CAST(DATABASEPROPERTYEX(DB_NAME(), 'Collation') AS NVARCHAR(128))";
	std::string result = MSSQLSimpleQuery::ExecuteScalar(*conn, sql);

	std::cout << "Database collation: '" << result << "'" << std::endl;

	// Should return a valid collation name
	assert(!result.empty());
	assert(result.find("_") != std::string::npos);	// Collations contain underscores

	conn->Close();
	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Execute - Multiple Rows
//==============================================================================
void test_execute_multiple_rows(const TestConfig &config) {
	std::cout << "\n=== Test: Execute - Multiple Rows ===" << std::endl;

	auto conn = CreateConnection(config);
	assert(conn != nullptr);

	std::string sql = "SELECT TOP 5 name FROM sys.schemas ORDER BY name";
	auto result = MSSQLSimpleQuery::Execute(*conn, sql);

	std::cout << "Query: " << sql << std::endl;
	std::cout << "Success: " << (result.success ? "true" : "false") << std::endl;
	std::cout << "Row count: " << result.RowCount() << std::endl;

	if (!result.success) {
		std::cerr << "Error: " << result.error_message << std::endl;
	}

	assert(result.success);
	assert(result.RowCount() > 0);

	// Print column names
	std::cout << "Columns: ";
	for (const auto &col : result.column_names) {
		std::cout << col << " ";
	}
	std::cout << std::endl;

	// Print rows
	for (size_t i = 0; i < result.rows.size(); i++) {
		std::cout << "  Row " << i << ": ";
		for (const auto &val : result.rows[i]) {
			std::cout << "'" << val << "' ";
		}
		std::cout << std::endl;
	}

	conn->Close();
	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Execute - Schema Discovery Query
//==============================================================================
void test_execute_schema_discovery(const TestConfig &config) {
	std::cout << "\n=== Test: Execute - Schema Discovery ===" << std::endl;

	auto conn = CreateConnection(config);
	assert(conn != nullptr);

	// This is similar to the query used in metadata cache
	std::string sql = R"(
        SELECT s.name AS schema_name
        FROM sys.schemas s
        WHERE s.schema_id NOT IN (3, 4)
          AND EXISTS (
            SELECT 1 FROM sys.tables t WHERE t.schema_id = s.schema_id
            UNION ALL
            SELECT 1 FROM sys.views v WHERE v.schema_id = s.schema_id
          )
        ORDER BY s.name
    )";

	auto result = MSSQLSimpleQuery::Execute(*conn, sql);

	std::cout << "Query: Schema discovery" << std::endl;
	std::cout << "Success: " << (result.success ? "true" : "false") << std::endl;
	std::cout << "Row count: " << result.RowCount() << std::endl;

	if (!result.success) {
		std::cerr << "Error: " << result.error_message << std::endl;
	}

	assert(result.success);

	// Print schemas found
	std::cout << "Schemas with tables/views:" << std::endl;
	for (const auto &row : result.rows) {
		if (!row.empty()) {
			std::cout << "  - " << row[0] << std::endl;
		}
	}

	conn->Close();
	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Execute With Callback
//==============================================================================
void test_execute_with_callback(const TestConfig &config) {
	std::cout << "\n=== Test: ExecuteWithCallback ===" << std::endl;

	auto conn = CreateConnection(config);
	assert(conn != nullptr);

	std::string sql = "SELECT TOP 10 object_id, name, type FROM sys.objects ORDER BY name";

	int row_count = 0;
	auto result = MSSQLSimpleQuery::ExecuteWithCallback(*conn, sql, [&row_count](const std::vector<std::string> &row) {
		std::cout << "  Callback row " << row_count << ": ";
		for (const auto &val : row) {
			std::cout << "'" << val << "' ";
		}
		std::cout << std::endl;
		row_count++;
		return true;  // continue
	});

	std::cout << "Success: " << (result.success ? "true" : "false") << std::endl;
	std::cout << "Rows processed: " << row_count << std::endl;

	if (!result.success) {
		std::cerr << "Error: " << result.error_message << std::endl;
	}

	assert(result.success);
	assert(row_count > 0);

	conn->Close();
	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Execute With Callback - Early Stop
//==============================================================================
void test_execute_with_callback_early_stop(const TestConfig &config) {
	std::cout << "\n=== Test: ExecuteWithCallback - Early Stop ===" << std::endl;

	auto conn = CreateConnection(config);
	assert(conn != nullptr);

	// Query that would return many rows
	std::string sql = "SELECT TOP 100 object_id, name FROM sys.objects ORDER BY name";

	int row_count = 0;
	const int stop_after = 3;

	auto result = MSSQLSimpleQuery::ExecuteWithCallback(
		*conn, sql, [&row_count, stop_after](const std::vector<std::string> &row) {
			row_count++;
			std::cout << "  Row " << row_count << ": " << row[1] << std::endl;
			return row_count < stop_after;	// stop after 3 rows
		});

	std::cout << "Rows processed before stop: " << row_count << std::endl;

	// Result might have error due to cancellation, but we should have processed some rows
	assert(row_count == stop_after);

	// Note: After early stop with cancellation, connection state might need attention
	// The MSSQLSimpleQuery sends attention and waits for ack

	conn->Close();
	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Error Handling - Invalid SQL
//==============================================================================
void test_error_handling_invalid_sql(const TestConfig &config) {
	std::cout << "\n=== Test: Error Handling - Invalid SQL ===" << std::endl;

	auto conn = CreateConnection(config);
	assert(conn != nullptr);

	std::string sql = "SELECT * FROM nonexistent_table_xyz_12345";
	auto result = MSSQLSimpleQuery::Execute(*conn, sql);

	std::cout << "Success: " << (result.success ? "true" : "false") << std::endl;
	std::cout << "Error number: " << result.error_number << std::endl;
	std::cout << "Error message: " << result.error_message << std::endl;

	assert(!result.success);
	assert(result.error_number != 0);
	assert(!result.error_message.empty());

	conn->Close();
	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Connection Reuse After Query
//==============================================================================
void test_connection_reuse(const TestConfig &config) {
	std::cout << "\n=== Test: Connection Reuse After Query ===" << std::endl;

	auto conn = CreateConnection(config);
	assert(conn != nullptr);

	// Execute first query
	std::string result1 = MSSQLSimpleQuery::ExecuteScalar(*conn, "SELECT 1");
	std::cout << "Query 1 result: " << result1 << std::endl;
	assert(result1 == "1");

	// Connection should be back in Idle state
	std::cout << "Connection state after query 1: " << ConnectionStateToString(conn->GetState()) << std::endl;
	assert(conn->GetState() == ConnectionState::Idle);

	// Execute second query on same connection
	std::string result2 = MSSQLSimpleQuery::ExecuteScalar(*conn, "SELECT 2");
	std::cout << "Query 2 result: " << result2 << std::endl;
	assert(result2 == "2");

	// Connection should still be in Idle state
	std::cout << "Connection state after query 2: " << ConnectionStateToString(conn->GetState()) << std::endl;
	assert(conn->GetState() == ConnectionState::Idle);

	// Execute third query
	std::string result3 = MSSQLSimpleQuery::ExecuteScalar(*conn, "SELECT 3");
	std::cout << "Query 3 result: " << result3 << std::endl;
	assert(result3 == "3");

	conn->Close();
	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Various Data Types
//==============================================================================
void test_various_data_types(const TestConfig &config) {
	std::cout << "\n=== Test: Various Data Types ===" << std::endl;

	auto conn = CreateConnection(config);
	assert(conn != nullptr);

	std::string sql = R"(
        SELECT
            CAST(42 AS INT) as int_val,
            CAST(123456789012345 AS BIGINT) as bigint_val,
            CAST(3.14 AS FLOAT) as float_val,
            N'Unicode: こんにちは' as nvarchar_val,
            'ASCII text' as varchar_val,
            CAST(1 AS BIT) as bit_val
    )";

	auto result = MSSQLSimpleQuery::Execute(*conn, sql);

	std::cout << "Success: " << (result.success ? "true" : "false") << std::endl;

	if (!result.success) {
		std::cerr << "Error: " << result.error_message << std::endl;
		assert(false);
	}

	assert(result.RowCount() == 1);

	std::cout << "Columns: ";
	for (const auto &col : result.column_names) {
		std::cout << col << " | ";
	}
	std::cout << std::endl;

	std::cout << "Values: ";
	for (const auto &val : result.rows[0]) {
		std::cout << "'" << val << "' | ";
	}
	std::cout << std::endl;

	// Verify values
	assert(result.rows[0].size() >= 6);
	assert(result.rows[0][0] == "42");				 // INT
	assert(result.rows[0][1] == "123456789012345");	 // BIGINT
	// FLOAT might have precision variations, just check non-empty
	assert(!result.rows[0][2].empty());
	// NVARCHAR - should contain Unicode
	assert(result.rows[0][3].find("Unicode") != std::string::npos);
	assert(result.rows[0][4] == "ASCII text");	// VARCHAR
	assert(result.rows[0][5] == "1");			// BIT

	conn->Close();
	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Main
//==============================================================================
int main() {
	std::cout << "========================================" << std::endl;
	std::cout << "MSSQLSimpleQuery Integration Tests" << std::endl;
	std::cout << "========================================" << std::endl;

	auto config = TestConfig::FromEnv();

	if (!config.IsValid()) {
		std::cerr << "\nERROR: MSSQL_TEST_PASS environment variable is required!" << std::endl;
		std::cerr << "\nSetup:" << std::endl;
		std::cerr << "  1. Start SQL Server:" << std::endl;
		std::cerr << "     docker compose -f docker/docker-compose.yml up -d" << std::endl;
		std::cerr << "\n  2. Set environment variables:" << std::endl;
		std::cerr << "     export MSSQL_TEST_HOST=localhost" << std::endl;
		std::cerr << "     export MSSQL_TEST_PORT=1433" << std::endl;
		std::cerr << "     export MSSQL_TEST_USER=sa" << std::endl;
		std::cerr << "     export MSSQL_TEST_PASS=YourPassword" << std::endl;
		std::cerr << "     export MSSQL_TEST_DB=master" << std::endl;
		std::cerr << "\n  3. Run tests:" << std::endl;
		std::cerr << "     ./test_simple_query" << std::endl;
		return 1;
	}

	std::cout << "\nConnection settings:" << std::endl;
	std::cout << "  Host: " << config.host << std::endl;
	std::cout << "  Port: " << config.port << std::endl;
	std::cout << "  User: " << config.user << std::endl;
	std::cout << "  Database: " << config.database << std::endl;

	// Verify connectivity first
	std::cout << "\n=== Verifying SQL Server connectivity ===" << std::endl;
	{
		auto test_conn = CreateConnection(config);
		if (!test_conn) {
			std::cerr << "ERROR: Cannot connect to SQL Server" << std::endl;
			return 1;
		}
		std::cout << "Connection established! SPID=" << test_conn->GetSpid() << std::endl;
		test_conn->Close();
	}

	// Run all tests
	try {
		test_execute_scalar_simple(config);
		test_execute_scalar_string(config);
		test_execute_scalar_collation(config);
		test_execute_multiple_rows(config);
		test_execute_schema_discovery(config);
		test_execute_with_callback(config);
		test_execute_with_callback_early_stop(config);
		test_error_handling_invalid_sql(config);
		test_connection_reuse(config);
		test_various_data_types(config);
	} catch (const std::exception &e) {
		std::cerr << "\nTEST FAILED with exception: " << e.what() << std::endl;
		return 1;
	}

	std::cout << "\n========================================" << std::endl;
	std::cout << "All MSSQLSimpleQuery tests PASSED!" << std::endl;
	std::cout << "========================================" << std::endl;

	return 0;
}
