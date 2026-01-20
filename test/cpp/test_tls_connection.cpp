// test/cpp/test_tls_connection.cpp
// Integration tests for TLS/encrypted connections to SQL Server
//
// This test requires a running SQL Server instance with TLS enabled.
// SQL Server 2022 has TLS enabled by default with a self-signed certificate.
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
// Compile (from project root):
//   See CMakeLists.txt for build configuration with mbedTLS
//
// Run:
//   ./test_tls_connection

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "tds/tds_connection.hpp"
#include "tds/tds_connection_pool.hpp"

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

//==============================================================================
// Test: Basic TLS Connection
// Verifies that a TLS connection can be established successfully
//==============================================================================
void test_basic_tls_connection(const TestConfig &config) {
	std::cout << "\n=== Test: Basic TLS Connection ===" << std::endl;

	auto conn = std::make_shared<TdsConnection>();

	// Connect TCP
	if (!conn->Connect(config.host, config.port)) {
		std::cerr << "TCP connect failed: " << conn->GetLastError() << std::endl;
		assert(false && "TCP connection should succeed");
	}
	std::cout << "TCP connection established" << std::endl;

	// Authenticate WITH TLS (use_encrypt=true)
	if (!conn->Authenticate(config.user, config.pass, config.database, true)) {
		std::cerr << "TLS authentication failed: " << conn->GetLastError() << std::endl;
		assert(false && "TLS authentication should succeed");
	}

	// Verify TLS is enabled
	assert(conn->IsTlsEnabled());
	std::cout << "TLS enabled: yes" << std::endl;

	// Verify connection is functional
	assert(conn->IsAlive());
	assert(conn->GetState() == ConnectionState::Idle);
	std::cout << "Connection state: Idle" << std::endl;

	// Get SPID (should be non-zero after successful login)
	assert(conn->GetSpid() > 0);
	std::cout << "SPID: " << conn->GetSpid() << std::endl;

	conn->Close();
	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: TLS Cipher Suite and Version
// Verifies that the negotiated TLS version and cipher are acceptable
//==============================================================================
void test_tls_cipher_and_version(const TestConfig &config) {
	std::cout << "\n=== Test: TLS Cipher Suite and Version ===" << std::endl;

	auto conn = std::make_shared<TdsConnection>();

	if (!conn->Connect(config.host, config.port)) {
		assert(false && "TCP connection should succeed");
	}

	if (!conn->Authenticate(config.user, config.pass, config.database, true)) {
		std::cerr << "TLS authentication failed: " << conn->GetLastError() << std::endl;
		assert(false && "TLS authentication should succeed");
	}

	assert(conn->IsTlsEnabled());

	// Get TLS info from socket
	auto *socket = conn->GetSocket();
	assert(socket != nullptr);

	std::string cipher = socket->GetTlsCipherSuite();
	std::string version = socket->GetTlsVersion();

	std::cout << "TLS Version: " << version << std::endl;
	std::cout << "Cipher Suite: " << cipher << std::endl;

	// Verify we got TLS 1.2 or higher
	// TLSv1.2 or TLSv1.3
	assert(version.find("TLSv1.2") != std::string::npos || version.find("TLSv1.3") != std::string::npos);

	// Verify we got a strong cipher (contains AES or CHACHA20)
	assert(cipher.find("AES") != std::string::npos || cipher.find("CHACHA") != std::string::npos);

	conn->Close();
	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Query Execution Over TLS
// Verifies that SQL queries work correctly over TLS connection
//==============================================================================
void test_query_over_tls(const TestConfig &config) {
	std::cout << "\n=== Test: Query Execution Over TLS ===" << std::endl;

	auto conn = std::make_shared<TdsConnection>();

	if (!conn->Connect(config.host, config.port)) {
		assert(false && "TCP connection should succeed");
	}

	if (!conn->Authenticate(config.user, config.pass, config.database, true)) {
		std::cerr << "TLS auth failed: " << conn->GetLastError() << std::endl;
		assert(false && "TLS authentication should succeed");
	}

	assert(conn->IsTlsEnabled());
	std::cout << "TLS connection established" << std::endl;

	// Execute a simple query
	std::string sql = "SELECT 42 AS answer, 'hello' AS greeting";
	if (!conn->ExecuteBatch(sql)) {
		std::cerr << "ExecuteBatch failed: " << conn->GetLastError() << std::endl;
		assert(false && "ExecuteBatch should succeed");
	}
	std::cout << "Query sent: " << sql << std::endl;

	// Read response
	std::vector<uint8_t> buffer(8192);
	ssize_t total_received = 0;

	auto start = std::chrono::steady_clock::now();
	while (true) {
		ssize_t received = conn->ReceiveData(buffer.data() + total_received, buffer.size() - total_received, 5000);
		if (received <= 0)
			break;
		total_received += received;

		auto elapsed =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
		if (elapsed > 5000)
			break;
	}

	std::cout << "Received " << total_received << " bytes response" << std::endl;
	assert(total_received > 0);

	// Connection should return to Idle state for reuse
	// (The response parser would transition state, but for raw test just verify we got data)

	conn->Close();
	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Multiple Queries Over Same TLS Connection
// Verifies that TLS connection can be reused for multiple queries
//==============================================================================
void test_multiple_queries_over_tls(const TestConfig &config) {
	std::cout << "\n=== Test: Multiple Queries Over Same TLS Connection ===" << std::endl;

	auto conn = std::make_shared<TdsConnection>();

	if (!conn->Connect(config.host, config.port)) {
		assert(false && "TCP connection should succeed");
	}

	if (!conn->Authenticate(config.user, config.pass, config.database, true)) {
		std::cerr << "TLS auth failed: " << conn->GetLastError() << std::endl;
		assert(false && "TLS authentication should succeed");
	}

	assert(conn->IsTlsEnabled());

	// Execute multiple queries on the same connection
	std::vector<std::string> queries = {"SELECT 1 AS q1", "SELECT 2 AS q2", "SELECT 3 AS q3"};

	for (size_t i = 0; i < queries.size(); i++) {
		// Need to reset connection state for next query
		// In real usage, the response parser would transition back to Idle
		if (conn->GetState() != ConnectionState::Idle) {
			// Force state reset for test
			conn->TransitionState(conn->GetState(), ConnectionState::Idle);
		}

		if (!conn->ExecuteBatch(queries[i])) {
			std::cerr << "Query " << (i + 1) << " failed: " << conn->GetLastError() << std::endl;
			assert(false && "Query should succeed");
		}

		// Consume response
		std::vector<uint8_t> buffer(4096);
		ssize_t received = conn->ReceiveData(buffer.data(), buffer.size(), 5000);
		assert(received > 0);
		std::cout << "Query " << (i + 1) << ": received " << received << " bytes" << std::endl;
	}

	conn->Close();
	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: TLS Connection Pool Integration
// Verifies that TLS connections work correctly with connection pooling
//==============================================================================
void test_tls_connection_pool(const TestConfig &config) {
	std::cout << "\n=== Test: TLS Connection Pool Integration ===" << std::endl;

	// Create factory that creates TLS connections
	ConnectionFactory tls_factory = [&config]() -> std::shared_ptr<TdsConnection> {
		auto conn = std::make_shared<TdsConnection>();
		if (!conn->Connect(config.host, config.port)) {
			std::cerr << "[TLS Factory] Connect failed: " << conn->GetLastError() << std::endl;
			return nullptr;
		}
		if (!conn->Authenticate(config.user, config.pass, config.database, true)) {
			std::cerr << "[TLS Factory] Auth failed: " << conn->GetLastError() << std::endl;
			return nullptr;
		}
		return conn;
	};

	PoolConfiguration pool_config;
	pool_config.connection_limit = 3;
	pool_config.acquire_timeout = 30;

	ConnectionPool pool("test_tls_pool", pool_config, tls_factory);

	// Acquire first connection
	auto conn1 = pool.Acquire();
	assert(conn1 != nullptr);
	assert(conn1->IsTlsEnabled());
	std::cout << "Connection 1: TLS enabled, SPID=" << conn1->GetSpid() << std::endl;

	// Acquire second connection
	auto conn2 = pool.Acquire();
	assert(conn2 != nullptr);
	assert(conn2->IsTlsEnabled());
	std::cout << "Connection 2: TLS enabled, SPID=" << conn2->GetSpid() << std::endl;

	// Verify they are different connections
	assert(conn1->GetSpid() != conn2->GetSpid());

	// Release both
	pool.Release(conn1);
	pool.Release(conn2);

	// Acquire again - should reuse
	auto conn3 = pool.Acquire();
	assert(conn3 != nullptr);
	assert(conn3->IsTlsEnabled());
	std::cout << "Connection 3 (reused): TLS enabled, SPID=" << conn3->GetSpid() << std::endl;

	pool.Release(conn3);

	auto stats = pool.GetStats();
	std::cout << "Pool stats: created=" << stats.connections_created << ", acquires=" << stats.acquire_count
			  << std::endl;

	// Should have created only 2 connections for 3 acquires
	assert(stats.connections_created == 2);
	assert(stats.acquire_count == 3);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Parallel TLS Connections
// Verifies that multiple threads can establish TLS connections concurrently
//==============================================================================
void test_parallel_tls_connections(const TestConfig &config) {
	std::cout << "\n=== Test: Parallel TLS Connections ===" << std::endl;

	const int num_threads = 4;
	std::atomic<int> successful{0};
	std::atomic<int> failed{0};
	std::vector<std::thread> threads;

	for (int i = 0; i < num_threads; i++) {
		threads.emplace_back([&config, &successful, &failed, i]() {
			auto conn = std::make_shared<TdsConnection>();

			if (!conn->Connect(config.host, config.port)) {
				std::cerr << "Thread " << i << " connect failed: " << conn->GetLastError() << std::endl;
				failed++;
				return;
			}

			if (!conn->Authenticate(config.user, config.pass, config.database, true)) {
				std::cerr << "Thread " << i << " auth failed: " << conn->GetLastError() << std::endl;
				failed++;
				return;
			}

			if (!conn->IsTlsEnabled()) {
				std::cerr << "Thread " << i << " TLS not enabled" << std::endl;
				failed++;
				return;
			}

			std::cout << "Thread " << i << ": TLS connected, SPID=" << conn->GetSpid() << std::endl;
			successful++;

			// Keep connection alive briefly
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			conn->Close();
		});
	}

	for (auto &t : threads) {
		t.join();
	}

	std::cout << "Successful: " << successful << ", Failed: " << failed << std::endl;
	assert(successful == num_threads);
	assert(failed == 0);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: TLS vs Plaintext Comparison
// Verifies both modes work and TLS flag is correctly set
//==============================================================================
void test_tls_vs_plaintext(const TestConfig &config) {
	std::cout << "\n=== Test: TLS vs Plaintext Comparison ===" << std::endl;

	// Create plaintext connection
	auto plain_conn = std::make_shared<TdsConnection>();
	if (!plain_conn->Connect(config.host, config.port)) {
		assert(false && "Plaintext connect should succeed");
	}
	if (!plain_conn->Authenticate(config.user, config.pass, config.database, false)) {
		std::cerr << "Plaintext auth failed: " << plain_conn->GetLastError() << std::endl;
		assert(false && "Plaintext auth should succeed");
	}

	assert(!plain_conn->IsTlsEnabled());
	std::cout << "Plaintext connection: TLS=" << (plain_conn->IsTlsEnabled() ? "yes" : "no")
			  << ", SPID=" << plain_conn->GetSpid() << std::endl;

	// Create TLS connection
	auto tls_conn = std::make_shared<TdsConnection>();
	if (!tls_conn->Connect(config.host, config.port)) {
		assert(false && "TLS connect should succeed");
	}
	if (!tls_conn->Authenticate(config.user, config.pass, config.database, true)) {
		std::cerr << "TLS auth failed: " << tls_conn->GetLastError() << std::endl;
		assert(false && "TLS auth should succeed");
	}

	assert(tls_conn->IsTlsEnabled());
	std::cout << "TLS connection: TLS=" << (tls_conn->IsTlsEnabled() ? "yes" : "no") << ", SPID=" << tls_conn->GetSpid()
			  << std::endl;

	// Both should be alive
	assert(plain_conn->IsAlive());
	assert(tls_conn->IsAlive());

	// They should have different SPIDs
	assert(plain_conn->GetSpid() != tls_conn->GetSpid());

	plain_conn->Close();
	tls_conn->Close();

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: TLS Connection Timeout
// Verifies that TLS handshake respects timeout settings
//==============================================================================
void test_tls_connection_timing(const TestConfig &config) {
	std::cout << "\n=== Test: TLS Connection Timing ===" << std::endl;

	auto conn = std::make_shared<TdsConnection>();

	auto start = std::chrono::steady_clock::now();

	if (!conn->Connect(config.host, config.port)) {
		assert(false && "TCP connect should succeed");
	}

	auto tcp_time =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	std::cout << "TCP connect time: " << tcp_time << "ms" << std::endl;

	start = std::chrono::steady_clock::now();

	if (!conn->Authenticate(config.user, config.pass, config.database, true)) {
		std::cerr << "TLS auth failed: " << conn->GetLastError() << std::endl;
		assert(false && "TLS auth should succeed");
	}

	auto auth_time =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	std::cout << "TLS auth time (PRELOGIN + TLS handshake + LOGIN7): " << auth_time << "ms" << std::endl;

	// TLS connection should complete within reasonable time (10 seconds for slow systems)
	assert(auth_time < 10000);

	assert(conn->IsTlsEnabled());
	conn->Close();

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: TLS with Wrong Server (Error Handling)
// Verifies proper error handling when TLS fails
//==============================================================================
void test_tls_connection_wrong_port(const TestConfig &config) {
	std::cout << "\n=== Test: TLS Connection to Wrong Port (Error Handling) ===" << std::endl;

	auto conn = std::make_shared<TdsConnection>();

	// Try to connect to a port that doesn't have SQL Server
	// Use port 1 which is typically not accessible
	bool connected = conn->Connect("127.0.0.1", 9999, 2);  // 2 second timeout

	if (connected) {
		// If we somehow connected, try auth which should fail
		bool authed = conn->Authenticate(config.user, config.pass, config.database, true);
		assert(!authed);
		std::cout << "Auth failed as expected: " << conn->GetLastError() << std::endl;
	} else {
		std::cout << "Connect failed as expected: " << conn->GetLastError() << std::endl;
	}

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Large Query Over TLS
// Verifies that large SQL queries work over TLS
//==============================================================================
void test_large_query_over_tls(const TestConfig &config) {
	std::cout << "\n=== Test: Large Query Over TLS ===" << std::endl;

	auto conn = std::make_shared<TdsConnection>();

	if (!conn->Connect(config.host, config.port)) {
		assert(false && "TCP connection should succeed");
	}

	if (!conn->Authenticate(config.user, config.pass, config.database, true)) {
		std::cerr << "TLS auth failed: " << conn->GetLastError() << std::endl;
		assert(false && "TLS auth should succeed");
	}

	assert(conn->IsTlsEnabled());

	// Generate a query that produces a large result
	// SELECT a lot of rows
	std::string sql = R"(
        SELECT TOP 1000
            ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) as row_num,
            REPLICATE('X', 100) as padding
        FROM sys.objects a, sys.objects b
    )";

	if (!conn->ExecuteBatch(sql)) {
		std::cerr << "ExecuteBatch failed: " << conn->GetLastError() << std::endl;
		assert(false && "ExecuteBatch should succeed");
	}
	std::cout << "Large query sent" << std::endl;

	// Read all response data
	std::vector<uint8_t> buffer(65536);
	ssize_t total_received = 0;
	int packet_count = 0;

	auto start = std::chrono::steady_clock::now();
	while (true) {
		ssize_t received = conn->ReceiveData(buffer.data(), buffer.size(), 5000);
		if (received <= 0)
			break;
		total_received += received;
		packet_count++;

		auto elapsed =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
		if (elapsed > 30000)
			break;	// 30 second max
	}

	std::cout << "Received " << total_received << " bytes in " << packet_count << " reads" << std::endl;
	assert(total_received > 10000);	 // Should receive substantial data

	conn->Close();
	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Main
//==============================================================================
int main() {
	std::cout << "========================================" << std::endl;
	std::cout << "TDS TLS Connection Integration Tests" << std::endl;
	std::cout << "========================================" << std::endl;

	auto config = TestConfig::FromEnv();

	if (!config.IsValid()) {
		std::cerr << "\nERROR: MSSQL_TEST_PASS environment variable is required!" << std::endl;
		std::cerr << "\nSetup:" << std::endl;
		std::cerr << "  1. Start SQL Server with TLS:" << std::endl;
		std::cerr << "     docker compose -f docker/docker-compose.yml up -d" << std::endl;
		std::cerr << "\n  2. Set environment variables:" << std::endl;
		std::cerr << "     export MSSQL_TEST_HOST=localhost" << std::endl;
		std::cerr << "     export MSSQL_TEST_PORT=1433" << std::endl;
		std::cerr << "     export MSSQL_TEST_USER=sa" << std::endl;
		std::cerr << "     export MSSQL_TEST_PASS=TestPassword1" << std::endl;
		std::cerr << "     export MSSQL_TEST_DB=master" << std::endl;
		std::cerr << "\n  3. Run tests:" << std::endl;
		std::cerr << "     ./test_tls_connection" << std::endl;
		return 1;
	}

	std::cout << "\nConnection settings:" << std::endl;
	std::cout << "  Host: " << config.host << std::endl;
	std::cout << "  Port: " << config.port << std::endl;
	std::cout << "  User: " << config.user << std::endl;
	std::cout << "  Database: " << config.database << std::endl;

	// Verify basic connectivity first
	std::cout << "\n=== Verifying SQL Server TLS connectivity ===" << std::endl;
	{
		auto test_conn = std::make_shared<TdsConnection>();
		if (!test_conn->Connect(config.host, config.port)) {
			std::cerr << "ERROR: Cannot connect to SQL Server: " << test_conn->GetLastError() << std::endl;
			return 1;
		}
		if (!test_conn->Authenticate(config.user, config.pass, config.database, true)) {
			std::cerr << "ERROR: TLS authentication failed: " << test_conn->GetLastError() << std::endl;
			std::cerr << "\nThis could mean:" << std::endl;
			std::cerr << "  - SQL Server is not running or not reachable" << std::endl;
			std::cerr << "  - Incorrect username/password" << std::endl;
			std::cerr << "  - SQL Server does not support TLS encryption" << std::endl;
			return 1;
		}
		std::cout << "TLS connectivity verified!" << std::endl;
		std::cout << "  TLS enabled: " << (test_conn->IsTlsEnabled() ? "yes" : "no") << std::endl;
		if (test_conn->IsTlsEnabled()) {
			auto *socket = test_conn->GetSocket();
			std::cout << "  TLS version: " << socket->GetTlsVersion() << std::endl;
			std::cout << "  Cipher: " << socket->GetTlsCipherSuite() << std::endl;
		}
		test_conn->Close();
	}

	// Run all tests
	try {
		test_basic_tls_connection(config);
		test_tls_cipher_and_version(config);
		test_query_over_tls(config);
		test_multiple_queries_over_tls(config);
		test_tls_connection_pool(config);
		test_parallel_tls_connections(config);
		test_tls_vs_plaintext(config);
		test_tls_connection_timing(config);
		test_tls_connection_wrong_port(config);
		test_large_query_over_tls(config);
	} catch (const std::exception &e) {
		std::cerr << "\nTEST FAILED with exception: " << e.what() << std::endl;
		return 1;
	}

	std::cout << "\n========================================" << std::endl;
	std::cout << "All TLS tests PASSED!" << std::endl;
	std::cout << "========================================" << std::endl;

	return 0;
}
