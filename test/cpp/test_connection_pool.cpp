// test/cpp/test_connection_pool.cpp
// Integration test for the real TDS connection pool implementation
//
// This test requires a running SQL Server instance.
// Set environment variables:
//   MSSQL_TEST_HOST:    SQL Server hostname (default: localhost)
//   MSSQL_TEST_PORT:    SQL Server port (default: 1433)
//   MSSQL_TEST_USER:    SQL Server username (default: sa)
//   MSSQL_TEST_PASS:    SQL Server password (required)
//   MSSQL_TEST_DB:      Database name (default: master)
//   MSSQL_TEST_ENCRYPT: Enable TLS encryption (default: false)
//                       Set to "true", "1", or "yes" to enable
//
// Compile (from project root):
//   g++ -std=c++17 -I src/include -I duckdb/src/include \
//       test/cpp/test_connection_pool.cpp \
//       src/tds/tds_connection.cpp src/tds/tds_socket.cpp \
//       src/tds/tds_protocol.cpp src/tds/tds_packet.cpp \
//       src/tds/tds_tls_context.cpp src/tds/connection_pool.cpp \
//       -o test_pool -pthread -lmbedtls -lmbedx509 -lmbedcrypto
//
// Run: ./test_pool

#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <cstdlib>

#include "tds/tds_connection.hpp"
#include "tds/tds_connection_pool.hpp"

using namespace duckdb::tds;

// Get environment variable with default
std::string getenv_or(const char* name, const char* default_value) {
    const char* value = std::getenv(name);
    return value ? value : default_value;
}

// Connection parameters from environment
struct TestConfig {
    std::string host;
    uint16_t port;
    std::string user;
    std::string pass;
    std::string database;
    bool use_encrypt;  // Enable TLS encryption

    static TestConfig FromEnv() {
        TestConfig config;
        config.host = getenv_or("MSSQL_TEST_HOST", "localhost");
        config.port = static_cast<uint16_t>(std::stoi(getenv_or("MSSQL_TEST_PORT", "1433")));
        config.user = getenv_or("MSSQL_TEST_USER", "sa");
        config.pass = getenv_or("MSSQL_TEST_PASS", "");
        config.database = getenv_or("MSSQL_TEST_DB", "master");
        // Default to false for backward compatibility
        std::string encrypt_str = getenv_or("MSSQL_TEST_ENCRYPT", "false");
        config.use_encrypt = (encrypt_str == "true" || encrypt_str == "1" || encrypt_str == "yes");
        return config;
    }

    bool IsValid() const {
        return !pass.empty();
    }
};

// Create a connection factory for the pool
ConnectionFactory createFactory(const TestConfig& config) {
    return [config]() -> std::shared_ptr<TdsConnection> {
        auto conn = std::make_shared<TdsConnection>();
        if (!conn->Connect(config.host, config.port)) {
            std::cerr << "[Factory] Connection failed: " << conn->GetLastError() << std::endl;
            return nullptr;
        }
        if (!conn->Authenticate(config.user, config.pass, config.database, config.use_encrypt)) {
            std::cerr << "[Factory] Authentication failed: " << conn->GetLastError() << std::endl;
            return nullptr;
        }
        return conn;
    };
}

// Test functions
void test_basic_acquire_release(const TestConfig& config) {
    std::cout << "\n=== Test: Basic Acquire/Release ===" << std::endl;

    PoolConfiguration pool_config;
    pool_config.connection_limit = 2;
    pool_config.acquire_timeout = 30;

    ConnectionPool pool("test_basic", pool_config, createFactory(config));

    // Acquire first connection
    auto conn1 = pool.Acquire();
    assert(conn1 != nullptr);
    std::cout << "Acquired first connection" << std::endl;

    auto stats = pool.GetStats();
    assert(stats.connections_created == 1);
    assert(stats.active_connections == 1);
    assert(stats.idle_connections == 0);

    // Verify connection works
    assert(conn1->IsAlive());
    std::cout << "Connection is alive" << std::endl;

    // Release connection
    pool.Release(conn1);
    conn1.reset();

    stats = pool.GetStats();
    assert(stats.active_connections == 0);
    assert(stats.idle_connections == 1);
    std::cout << "Connection released to idle pool" << std::endl;

    std::cout << "PASSED!" << std::endl;
}

void test_connection_reuse(const TestConfig& config) {
    std::cout << "\n=== Test: Connection Reuse ===" << std::endl;

    PoolConfiguration pool_config;
    pool_config.connection_limit = 2;

    ConnectionPool pool("test_reuse", pool_config, createFactory(config));

    // Acquire and release 5 times - should reuse same connection
    for (int i = 0; i < 5; i++) {
        auto conn = pool.Acquire();
        assert(conn != nullptr);
        assert(conn->IsAlive());
        std::cout << "Iteration " << (i+1) << ": Got connection (alive)" << std::endl;
        pool.Release(conn);
    }

    auto stats = pool.GetStats();
    std::cout << "Connections created: " << stats.connections_created << std::endl;
    std::cout << "Acquire count: " << stats.acquire_count << std::endl;

    // Should have created only 1 connection but acquired 5 times
    assert(stats.connections_created == 1);
    assert(stats.acquire_count == 5);

    std::cout << "PASSED!" << std::endl;
}

void test_pool_limit(const TestConfig& config) {
    std::cout << "\n=== Test: Pool Limit ===" << std::endl;

    PoolConfiguration pool_config;
    pool_config.connection_limit = 2;
    pool_config.acquire_timeout = 1;  // 1 second timeout

    ConnectionPool pool("test_limit", pool_config, createFactory(config));

    // Acquire 2 connections (at limit)
    auto conn1 = pool.Acquire();
    auto conn2 = pool.Acquire();
    assert(conn1 != nullptr);
    assert(conn2 != nullptr);
    std::cout << "Acquired 2 connections (at limit)" << std::endl;

    auto stats = pool.GetStats();
    assert(stats.active_connections == 2);

    // Try to acquire third - should timeout
    std::cout << "Trying to acquire 3rd connection (should timeout)..." << std::endl;
    auto conn3 = pool.Acquire(100);  // 100ms timeout
    assert(conn3 == nullptr);
    std::cout << "3rd acquire timed out as expected" << std::endl;

    stats = pool.GetStats();
    assert(stats.acquire_timeout_count == 1);

    // Release one and try again
    pool.Release(conn1);
    conn3 = pool.Acquire();
    assert(conn3 != nullptr);
    std::cout << "After release, acquired connection successfully" << std::endl;

    // Cleanup
    pool.Release(conn2);
    pool.Release(conn3);

    std::cout << "PASSED!" << std::endl;
}

void test_parallel_acquire(const TestConfig& config) {
    std::cout << "\n=== Test: Parallel Acquire ===" << std::endl;

    PoolConfiguration pool_config;
    pool_config.connection_limit = 4;
    pool_config.acquire_timeout = 30;

    ConnectionPool pool("test_parallel", pool_config, createFactory(config));

    std::atomic<int> successful{0};
    std::atomic<int> failed{0};
    std::vector<std::thread> threads;

    // Launch 4 threads to acquire connections simultaneously
    for (int i = 0; i < 4; i++) {
        threads.emplace_back([&pool, &successful, &failed, i]() {
            auto conn = pool.Acquire(5000);
            if (conn && conn->IsAlive()) {
                std::cout << "Thread " << i << " acquired connection" << std::endl;
                successful++;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                pool.Release(conn);
            } else {
                std::cout << "Thread " << i << " failed to acquire" << std::endl;
                failed++;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "Successful: " << successful << ", Failed: " << failed << std::endl;
    assert(successful == 4);
    assert(failed == 0);

    auto stats = pool.GetStats();
    std::cout << "Total connections created: " << stats.connections_created << std::endl;
    assert(stats.connections_created <= 4);
    assert(stats.acquire_timeout_count == 0);

    std::cout << "PASSED!" << std::endl;
}

void test_sequential_reuse(const TestConfig& config) {
    std::cout << "\n=== Test: Sequential Reuse Efficiency ===" << std::endl;

    PoolConfiguration pool_config;
    pool_config.connection_limit = 4;

    ConnectionPool pool("test_sequential", pool_config, createFactory(config));

    // Run 20 sequential operations - should reuse 1 connection
    for (int i = 0; i < 20; i++) {
        auto conn = pool.Acquire();
        assert(conn != nullptr);
        assert(conn->IsAlive());
        // Simulate minimal work
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        pool.Release(conn);
    }

    auto stats = pool.GetStats();
    std::cout << "Acquire count: " << stats.acquire_count << std::endl;
    std::cout << "Connections created: " << stats.connections_created << std::endl;

    // Should have created only 1 connection for 20 operations
    assert(stats.connections_created == 1);
    assert(stats.acquire_count == 20);

    double efficiency = (double)stats.acquire_count / std::max(stats.connections_created, (size_t)1);
    std::cout << "Reuse efficiency: " << efficiency << "x" << std::endl;

    std::cout << "PASSED!" << std::endl;
}

void test_connection_validation(const TestConfig& config) {
    std::cout << "\n=== Test: Connection Validation ===" << std::endl;

    PoolConfiguration pool_config;
    pool_config.connection_limit = 2;

    ConnectionPool pool("test_validation", pool_config, createFactory(config));

    // Acquire a connection
    auto conn1 = pool.Acquire();
    assert(conn1 != nullptr);

    // Verify it can validate
    assert(conn1->IsAlive());
    std::cout << "Connection is alive after acquire" << std::endl;

    // Release and re-acquire (should get same connection)
    pool.Release(conn1);
    auto conn2 = pool.Acquire();
    assert(conn2 != nullptr);
    assert(conn2->IsAlive());
    std::cout << "Connection is alive after reuse from pool" << std::endl;

    pool.Release(conn2);

    std::cout << "PASSED!" << std::endl;
}

void test_backward_compatibility_plaintext(const TestConfig& config) {
    std::cout << "\n=== Test: Backward Compatibility (Plaintext Default) ===" << std::endl;

    // Create connection without specifying use_encrypt (defaults to false)
    auto conn = std::make_shared<TdsConnection>();
    assert(conn != nullptr);

    if (!conn->Connect(config.host, config.port)) {
        std::cerr << "Connection failed: " << conn->GetLastError() << std::endl;
        assert(false && "Connection should succeed");
    }

    // Call Authenticate WITHOUT the use_encrypt parameter
    // This tests the default parameter value (use_encrypt=false)
    if (!conn->Authenticate(config.user, config.pass, config.database)) {
        std::cerr << "Authentication failed: " << conn->GetLastError() << std::endl;
        assert(false && "Authentication should succeed without TLS");
    }

    // Verify TLS is NOT enabled (backward compatibility)
    assert(!conn->IsTlsEnabled());
    std::cout << "TLS enabled: " << (conn->IsTlsEnabled() ? "yes" : "no") << std::endl;
    std::cout << "Connection established in plaintext mode (backward compatible)" << std::endl;

    // Verify connection is functional
    assert(conn->IsAlive());
    std::cout << "Connection is alive" << std::endl;

    conn->Close();
    std::cout << "PASSED!" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "TDS Connection Pool Integration Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    auto config = TestConfig::FromEnv();

    if (!config.IsValid()) {
        std::cerr << "\nERROR: MSSQL_TEST_PASS environment variable is required!" << std::endl;
        std::cerr << "\nUsage:" << std::endl;
        std::cerr << "  export MSSQL_TEST_HOST=localhost" << std::endl;
        std::cerr << "  export MSSQL_TEST_PORT=1433" << std::endl;
        std::cerr << "  export MSSQL_TEST_USER=sa" << std::endl;
        std::cerr << "  export MSSQL_TEST_PASS=YourPassword" << std::endl;
        std::cerr << "  export MSSQL_TEST_DB=master" << std::endl;
        std::cerr << "  ./test_pool" << std::endl;
        return 1;
    }

    std::cout << "\nConnection settings:" << std::endl;
    std::cout << "  Host: " << config.host << std::endl;
    std::cout << "  Port: " << config.port << std::endl;
    std::cout << "  User: " << config.user << std::endl;
    std::cout << "  Database: " << config.database << std::endl;
    std::cout << "  Encrypt: " << (config.use_encrypt ? "yes" : "no") << std::endl;

    // Test basic connectivity first
    std::cout << "\n=== Verifying SQL Server connectivity ===" << std::endl;
    auto test_conn = std::make_shared<TdsConnection>();
    if (!test_conn->Connect(config.host, config.port)) {
        std::cerr << "ERROR: Cannot connect to SQL Server: " << test_conn->GetLastError() << std::endl;
        return 1;
    }
    if (!test_conn->Authenticate(config.user, config.pass, config.database, config.use_encrypt)) {
        std::cerr << "ERROR: Authentication failed: " << test_conn->GetLastError() << std::endl;
        return 1;
    }
    std::cout << "SQL Server connectivity verified!" << std::endl;
    std::cout << "TLS enabled: " << (test_conn->IsTlsEnabled() ? "yes" : "no") << std::endl;
    test_conn->Close();

    // Run all tests
    try {
        test_backward_compatibility_plaintext(config);  // T029: Backward compatibility test
        test_basic_acquire_release(config);
        test_connection_reuse(config);
        test_pool_limit(config);
        test_parallel_acquire(config);
        test_sequential_reuse(config);
        test_connection_validation(config);
    } catch (const std::exception& e) {
        std::cerr << "\nTEST FAILED with exception: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << "All tests PASSED!" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
