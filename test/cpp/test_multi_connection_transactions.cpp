// test/cpp/test_multi_connection_transactions.cpp
// Integration test for multi-connection transaction isolation
//
// This test verifies:
// 1. Two DuckDB connections can work independently
// 2. Transaction in one connection doesn't block the other
// 3. Transaction isolation is maintained (uncommitted changes not visible)
// 4. Two parallel transactions can work without deadlocks
//
// REQUIRES: Running SQL Server instance and built extension
//
// Environment variables:
//   MSSQL_TEST_HOST:    SQL Server hostname (default: localhost)
//   MSSQL_TEST_PORT:    SQL Server port (default: 1433)
//   MSSQL_TEST_USER:    SQL Server username (default: sa)
//   MSSQL_TEST_PASS:    SQL Server password (required)
//   MSSQL_TEST_DB:      Database name (default: test_db)
//
// Compile (from project root, after building extension):
//   # The test links against DuckDB and loads the extension dynamically
//   g++ -std=c++17 -I duckdb/src/include \
//       test/cpp/test_multi_connection_transactions.cpp \
//       -L build/release -lduckdb -pthread -o test_multi_conn_txn
//
// Run:
//   # Make sure extension is in DuckDB's extension path or use unsigned mode
//   MSSQL_TEST_PASS=YourPassword ./test_multi_conn_txn

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "duckdb.hpp"

using namespace duckdb;

// Get environment variable with default
std::string getenv_or(const char *name, const char *default_value) {
	const char *value = std::getenv(name);
	return value ? value : default_value;
}

// Test configuration from environment
struct TestConfig {
	std::string host;
	std::string port;
	std::string user;
	std::string pass;
	std::string database;
	std::string extension_path;

	static TestConfig FromEnv() {
		TestConfig config;
		config.host = getenv_or("MSSQL_TEST_HOST", "localhost");
		config.port = getenv_or("MSSQL_TEST_PORT", "1433");
		config.user = getenv_or("MSSQL_TEST_USER", "sa");
		config.pass = getenv_or("MSSQL_TEST_PASS", "");
		config.database = getenv_or("MSSQL_TEST_DB", "test_db");
		config.extension_path = getenv_or("MSSQL_EXTENSION_PATH", "");
		return config;
	}

	bool IsValid() const {
		return !pass.empty();
	}

	std::string GetAttachString() const {
		std::ostringstream oss;
		oss << "host=" << host << ";port=" << port << ";user=" << user << ";password=" << pass << ";database="
		    << database;
		return oss.str();
	}
};

// Helper to execute query and return success
bool ExecuteQuery(Connection &conn, const std::string &sql, std::string *error_out = nullptr) {
	try {
		auto result = conn.Query(sql);
		if (result->HasError()) {
			if (error_out) {
				*error_out = result->GetError();
			}
			return false;
		}
		return true;
	} catch (const std::exception &e) {
		if (error_out) {
			*error_out = e.what();
		}
		return false;
	}
}

// Helper to execute query and get single integer result
int64_t QuerySingleInt(Connection &conn, const std::string &sql) {
	auto result = conn.Query(sql);
	if (result->HasError()) {
		throw std::runtime_error("Query failed: " + result->GetError());
	}
	if (result->RowCount() == 0) {
		throw std::runtime_error("Query returned no rows");
	}
	return result->GetValue(0, 0).GetValue<int64_t>();
}

// Test: Two connections can work independently
void test_independent_connections(DuckDB &db, const TestConfig &config) {
	std::cout << "\n=== Test: Independent Connections ===" << std::endl;

	// Create two connections
	Connection conn1(db);
	Connection conn2(db);

	std::string error;

	// Both connections should be able to query
	auto count1 = QuerySingleInt(conn1, "SELECT COUNT(*) FROM db.dbo.tx_test");
	std::cout << "Connection 1 count: " << count1 << std::endl;

	auto count2 = QuerySingleInt(conn2, "SELECT COUNT(*) FROM db.dbo.tx_test");
	std::cout << "Connection 2 count: " << count2 << std::endl;

	assert(count1 == count2);
	std::cout << "Both connections see same data" << std::endl;

	std::cout << "PASSED!" << std::endl;
}

// Test: Transaction in one connection doesn't block queries in other
void test_transaction_doesnt_block_other(DuckDB &db, const TestConfig &config) {
	std::cout << "\n=== Test: Transaction Doesn't Block Other Connection ===" << std::endl;

	Connection conn1(db);
	Connection conn2(db);

	std::string error;

	// Get initial count
	auto initial_count = QuerySingleInt(conn1, "SELECT COUNT(*) FROM db.dbo.tx_test");
	std::cout << "Initial count: " << initial_count << std::endl;

	// Start transaction on conn1
	assert(ExecuteQuery(conn1, "BEGIN TRANSACTION", &error));
	std::cout << "Connection 1: BEGIN TRANSACTION" << std::endl;

	// Insert a row in conn1's transaction
	assert(ExecuteQuery(conn1, "INSERT INTO db.dbo.tx_test (name, value) VALUES ('txn_test', 100)", &error));
	std::cout << "Connection 1: INSERT done" << std::endl;

	// Conn2 should still be able to query (not blocked)
	auto count_conn2 = QuerySingleInt(conn2, "SELECT COUNT(*) FROM db.dbo.tx_test");
	std::cout << "Connection 2 count (during conn1 txn): " << count_conn2 << std::endl;

	// Conn2 should NOT see the uncommitted row (isolation)
	assert(count_conn2 == initial_count);
	std::cout << "Connection 2 doesn't see uncommitted data (correct isolation)" << std::endl;

	// Conn2 can also do DML independently
	assert(ExecuteQuery(conn2, "INSERT INTO db.dbo.tx_test (name, value) VALUES ('conn2_insert', 200)", &error));
	std::cout << "Connection 2: INSERT done (autocommit)" << std::endl;

	// Verify conn2's insert is visible (autocommit)
	auto count_after_conn2 = QuerySingleInt(conn2, "SELECT COUNT(*) FROM db.dbo.tx_test WHERE name = 'conn2_insert'");
	assert(count_after_conn2 >= 1);
	std::cout << "Connection 2 insert committed immediately" << std::endl;

	// Rollback conn1's transaction
	assert(ExecuteQuery(conn1, "ROLLBACK", &error));
	std::cout << "Connection 1: ROLLBACK" << std::endl;

	// Clean up conn2's test row
	ExecuteQuery(conn2, "DELETE FROM db.dbo.tx_test WHERE name = 'conn2_insert'");

	std::cout << "PASSED!" << std::endl;
}

// Test: Commit makes changes visible to other connection
void test_commit_visibility(DuckDB &db, const TestConfig &config) {
	std::cout << "\n=== Test: Commit Makes Changes Visible ===" << std::endl;

	Connection conn1(db);
	Connection conn2(db);

	std::string error;

	// Get initial count
	auto initial_count = QuerySingleInt(conn1, "SELECT COUNT(*) FROM db.dbo.tx_test WHERE name = 'commit_test'");
	std::cout << "Initial matching rows: " << initial_count << std::endl;

	// Start transaction and insert
	assert(ExecuteQuery(conn1, "BEGIN TRANSACTION", &error));
	assert(ExecuteQuery(conn1, "INSERT INTO db.dbo.tx_test (name, value) VALUES ('commit_test', 300)", &error));
	std::cout << "Connection 1: INSERT in transaction" << std::endl;

	// Conn2 shouldn't see it yet
	auto count_before = QuerySingleInt(conn2, "SELECT COUNT(*) FROM db.dbo.tx_test WHERE name = 'commit_test'");
	assert(count_before == initial_count);
	std::cout << "Connection 2: doesn't see uncommitted row" << std::endl;

	// Commit
	assert(ExecuteQuery(conn1, "COMMIT", &error));
	std::cout << "Connection 1: COMMIT" << std::endl;

	// Now conn2 should see it
	auto count_after = QuerySingleInt(conn2, "SELECT COUNT(*) FROM db.dbo.tx_test WHERE name = 'commit_test'");
	assert(count_after == initial_count + 1);
	std::cout << "Connection 2: now sees committed row" << std::endl;

	// Clean up
	ExecuteQuery(conn1, "DELETE FROM db.dbo.tx_test WHERE name = 'commit_test'");

	std::cout << "PASSED!" << std::endl;
}

// Test: Two parallel transactions
void test_parallel_transactions(DuckDB &db, const TestConfig &config) {
	std::cout << "\n=== Test: Two Parallel Transactions ===" << std::endl;

	Connection conn1(db);
	Connection conn2(db);

	std::string error;

	// Clean up any previous test data
	ExecuteQuery(conn1, "DELETE FROM db.dbo.tx_test WHERE name IN ('parallel_1', 'parallel_2')");

	// Get initial count
	auto initial_count = QuerySingleInt(conn1, "SELECT COUNT(*) FROM db.dbo.tx_test");
	std::cout << "Initial count: " << initial_count << std::endl;

	// Start transactions on both connections
	assert(ExecuteQuery(conn1, "BEGIN TRANSACTION", &error));
	std::cout << "Connection 1: BEGIN TRANSACTION" << std::endl;

	assert(ExecuteQuery(conn2, "BEGIN TRANSACTION", &error));
	std::cout << "Connection 2: BEGIN TRANSACTION" << std::endl;

	// Each connection inserts its own row
	assert(ExecuteQuery(conn1, "INSERT INTO db.dbo.tx_test (name, value) VALUES ('parallel_1', 111)", &error));
	std::cout << "Connection 1: INSERT 'parallel_1'" << std::endl;

	assert(ExecuteQuery(conn2, "INSERT INTO db.dbo.tx_test (name, value) VALUES ('parallel_2', 222)", &error));
	std::cout << "Connection 2: INSERT 'parallel_2'" << std::endl;

	// Each connection sees its own uncommitted changes
	auto count1_own = QuerySingleInt(conn1, "SELECT COUNT(*) FROM db.dbo.tx_test WHERE name = 'parallel_1'");
	auto count2_own = QuerySingleInt(conn2, "SELECT COUNT(*) FROM db.dbo.tx_test WHERE name = 'parallel_2'");
	std::cout << "Connection 1 sees own insert: " << count1_own << std::endl;
	std::cout << "Connection 2 sees own insert: " << count2_own << std::endl;
	assert(count1_own == 1);
	assert(count2_own == 1);

	// Neither sees the other's uncommitted changes (default isolation)
	auto count1_other = QuerySingleInt(conn1, "SELECT COUNT(*) FROM db.dbo.tx_test WHERE name = 'parallel_2'");
	auto count2_other = QuerySingleInt(conn2, "SELECT COUNT(*) FROM db.dbo.tx_test WHERE name = 'parallel_1'");
	std::cout << "Connection 1 sees conn2's uncommitted: " << count1_other << std::endl;
	std::cout << "Connection 2 sees conn1's uncommitted: " << count2_other << std::endl;
	assert(count1_other == 0);
	assert(count2_other == 0);

	// Commit both
	assert(ExecuteQuery(conn1, "COMMIT", &error));
	std::cout << "Connection 1: COMMIT" << std::endl;

	assert(ExecuteQuery(conn2, "COMMIT", &error));
	std::cout << "Connection 2: COMMIT" << std::endl;

	// Now both rows should be visible to everyone
	auto final_count = QuerySingleInt(conn1, "SELECT COUNT(*) FROM db.dbo.tx_test WHERE name IN ('parallel_1', 'parallel_2')");
	std::cout << "Final count of parallel rows: " << final_count << std::endl;
	assert(final_count == 2);

	// Clean up
	ExecuteQuery(conn1, "DELETE FROM db.dbo.tx_test WHERE name IN ('parallel_1', 'parallel_2')");

	std::cout << "PASSED!" << std::endl;
}

// Test: Concurrent threads with transactions
void test_concurrent_threads(DuckDB &db, const TestConfig &config) {
	std::cout << "\n=== Test: Concurrent Threads with Transactions ===" << std::endl;

	std::atomic<int> success_count{0};
	std::atomic<int> error_count{0};
	const int num_threads = 4;
	const int rows_per_thread = 10;

	// Clean up any previous test data
	{
		Connection cleanup_conn(db);
		ExecuteQuery(cleanup_conn, "DELETE FROM db.dbo.tx_test WHERE name LIKE 'thread_%'");
	}

	std::vector<std::thread> threads;

	for (int t = 0; t < num_threads; t++) {
		threads.emplace_back([&db, t, rows_per_thread, &success_count, &error_count]() {
			try {
				Connection conn(db);
				std::string error;

				// Begin transaction
				if (!ExecuteQuery(conn, "BEGIN TRANSACTION", &error)) {
					std::cerr << "Thread " << t << " BEGIN failed: " << error << std::endl;
					error_count++;
					return;
				}

				// Insert multiple rows
				for (int i = 0; i < rows_per_thread; i++) {
					std::ostringstream sql;
					sql << "INSERT INTO db.dbo.tx_test (name, value) VALUES ('thread_" << t << "_row_" << i << "', "
					    << (t * 1000 + i) << ")";

					if (!ExecuteQuery(conn, sql.str(), &error)) {
						std::cerr << "Thread " << t << " INSERT failed: " << error << std::endl;
						ExecuteQuery(conn, "ROLLBACK");
						error_count++;
						return;
					}
				}

				// Commit
				if (!ExecuteQuery(conn, "COMMIT", &error)) {
					std::cerr << "Thread " << t << " COMMIT failed: " << error << std::endl;
					error_count++;
					return;
				}

				success_count++;
				std::cout << "Thread " << t << " completed successfully" << std::endl;

			} catch (const std::exception &e) {
				std::cerr << "Thread " << t << " exception: " << e.what() << std::endl;
				error_count++;
			}
		});
	}

	// Wait for all threads
	for (auto &t : threads) {
		t.join();
	}

	std::cout << "Successful threads: " << success_count << std::endl;
	std::cout << "Failed threads: " << error_count << std::endl;

	// Verify results
	Connection verify_conn(db);
	auto total_rows =
	    QuerySingleInt(verify_conn, "SELECT COUNT(*) FROM db.dbo.tx_test WHERE name LIKE 'thread_%'");
	std::cout << "Total rows inserted by threads: " << total_rows << std::endl;

	int expected_rows = success_count * rows_per_thread;
	assert(total_rows == expected_rows);

	// Clean up
	ExecuteQuery(verify_conn, "DELETE FROM db.dbo.tx_test WHERE name LIKE 'thread_%'");

	assert(error_count == 0);
	std::cout << "PASSED!" << std::endl;
}

// Test: UPDATE and DELETE in transactions work correctly
void test_update_delete_in_transaction(DuckDB &db, const TestConfig &config) {
	std::cout << "\n=== Test: UPDATE and DELETE in Transaction ===" << std::endl;

	Connection conn1(db);
	Connection conn2(db);

	std::string error;

	// Setup: Insert test rows
	ExecuteQuery(conn1, "DELETE FROM db.dbo.tx_test WHERE name LIKE 'upd_del_%'");
	ExecuteQuery(conn1, "INSERT INTO db.dbo.tx_test (name, value) VALUES ('upd_del_1', 100)");
	ExecuteQuery(conn1, "INSERT INTO db.dbo.tx_test (name, value) VALUES ('upd_del_2', 200)");
	ExecuteQuery(conn1, "INSERT INTO db.dbo.tx_test (name, value) VALUES ('upd_del_3', 300)");

	auto initial_sum = QuerySingleInt(conn1, "SELECT SUM(value) FROM db.dbo.tx_test WHERE name LIKE 'upd_del_%'");
	std::cout << "Initial sum: " << initial_sum << std::endl;
	assert(initial_sum == 600);

	// Begin transaction
	assert(ExecuteQuery(conn1, "BEGIN TRANSACTION", &error));
	std::cout << "Connection 1: BEGIN TRANSACTION" << std::endl;

	// UPDATE within transaction
	assert(ExecuteQuery(conn1, "UPDATE db.dbo.tx_test SET value = 500 WHERE name = 'upd_del_1'", &error));
	std::cout << "Connection 1: UPDATE upd_del_1 to 500" << std::endl;

	// DELETE within transaction
	assert(ExecuteQuery(conn1, "DELETE FROM db.dbo.tx_test WHERE name = 'upd_del_2'", &error));
	std::cout << "Connection 1: DELETE upd_del_2" << std::endl;

	// Conn1 sees its changes
	auto sum_conn1 = QuerySingleInt(conn1, "SELECT SUM(value) FROM db.dbo.tx_test WHERE name LIKE 'upd_del_%'");
	std::cout << "Connection 1 sum (in txn): " << sum_conn1 << std::endl;
	assert(sum_conn1 == 800);  // 500 + 300

	// Conn2 still sees original data
	auto sum_conn2 = QuerySingleInt(conn2, "SELECT SUM(value) FROM db.dbo.tx_test WHERE name LIKE 'upd_del_%'");
	std::cout << "Connection 2 sum (during conn1 txn): " << sum_conn2 << std::endl;
	assert(sum_conn2 == 600);  // Original

	// Rollback
	assert(ExecuteQuery(conn1, "ROLLBACK", &error));
	std::cout << "Connection 1: ROLLBACK" << std::endl;

	// Verify rollback worked
	auto sum_after_rollback =
	    QuerySingleInt(conn1, "SELECT SUM(value) FROM db.dbo.tx_test WHERE name LIKE 'upd_del_%'");
	std::cout << "Sum after rollback: " << sum_after_rollback << std::endl;
	assert(sum_after_rollback == 600);

	// Clean up
	ExecuteQuery(conn1, "DELETE FROM db.dbo.tx_test WHERE name LIKE 'upd_del_%'");

	std::cout << "PASSED!" << std::endl;
}

int main() {
	std::cout << "==========================================" << std::endl;
	std::cout << "Multi-Connection Transaction Tests" << std::endl;
	std::cout << "==========================================" << std::endl;

	auto config = TestConfig::FromEnv();

	if (!config.IsValid()) {
		std::cerr << "\nERROR: MSSQL_TEST_PASS environment variable is required!" << std::endl;
		std::cerr << "\nUsage:" << std::endl;
		std::cerr << "  export MSSQL_TEST_HOST=localhost" << std::endl;
		std::cerr << "  export MSSQL_TEST_PORT=1433" << std::endl;
		std::cerr << "  export MSSQL_TEST_USER=sa" << std::endl;
		std::cerr << "  export MSSQL_TEST_PASS=YourPassword" << std::endl;
		std::cerr << "  export MSSQL_TEST_DB=test_db" << std::endl;
		std::cerr << "  ./test_multi_conn_txn" << std::endl;
		return 1;
	}

	std::cout << "\nConnection settings:" << std::endl;
	std::cout << "  Host: " << config.host << std::endl;
	std::cout << "  Port: " << config.port << std::endl;
	std::cout << "  User: " << config.user << std::endl;
	std::cout << "  Database: " << config.database << std::endl;

	try {
		// Create DuckDB instance
		DuckDB db(nullptr);  // In-memory database
		Connection setup_conn(db);

		std::string error;

		// Load the MSSQL extension
		std::cout << "\nLoading MSSQL extension..." << std::endl;

		// Try loading from build directory first
		if (!ExecuteQuery(setup_conn, "LOAD 'build/release/extension/mssql/mssql.duckdb_extension'", &error)) {
			// Try loading by name (requires extension to be installed)
			if (!ExecuteQuery(setup_conn, "LOAD mssql", &error)) {
				std::cerr << "Failed to load MSSQL extension: " << error << std::endl;
				std::cerr << "Make sure the extension is built and in the DuckDB extension path" << std::endl;
				return 1;
			}
		}
		std::cout << "Extension loaded successfully" << std::endl;

		// Attach the MSSQL database
		std::cout << "\nAttaching MSSQL database..." << std::endl;
		std::ostringstream attach_sql;
		attach_sql << "ATTACH '" << config.GetAttachString() << "' AS db (TYPE mssql, READ_WRITE)";

		if (!ExecuteQuery(setup_conn, attach_sql.str(), &error)) {
			std::cerr << "Failed to attach MSSQL database: " << error << std::endl;
			return 1;
		}
		std::cout << "Database attached successfully" << std::endl;

		// Ensure test table exists
		std::cout << "\nSetting up test table..." << std::endl;
		// The table should be created by docker/init/init-transaction-tests.sql
		// Just verify it exists
		try {
			auto count = QuerySingleInt(setup_conn, "SELECT COUNT(*) FROM db.dbo.tx_test");
			std::cout << "Test table db.dbo.tx_test exists with " << count << " rows" << std::endl;
		} catch (const std::exception &e) {
			std::cerr << "Test table db.dbo.tx_test not found. Please run init-transaction-tests.sql first." << std::endl;
			std::cerr << "Error: " << e.what() << std::endl;
			return 1;
		}

		// Run all tests
		test_independent_connections(db, config);
		test_transaction_doesnt_block_other(db, config);
		test_commit_visibility(db, config);
		test_parallel_transactions(db, config);
		test_concurrent_threads(db, config);
		test_update_delete_in_transaction(db, config);

		std::cout << "\n==========================================" << std::endl;
		std::cout << "All tests PASSED!" << std::endl;
		std::cout << "==========================================" << std::endl;

	} catch (const std::exception &e) {
		std::cerr << "\nTEST FAILED with exception: " << e.what() << std::endl;
		return 1;
	}

	return 0;
}
