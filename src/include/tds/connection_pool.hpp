#pragma once

#include "tds_types.hpp"
#include "tds_connection.hpp"
#include <memory>
#include <queue>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>

namespace duckdb {
namespace tds {

// Pool configuration (mirrors PoolConfig from mssql_settings.hpp)
struct PoolConfiguration {
	size_t connection_limit = DEFAULT_CONNECTION_LIMIT;
	bool connection_cache = DEFAULT_CONNECTION_CACHE;
	int connection_timeout = DEFAULT_CONNECTION_TIMEOUT;
	int idle_timeout = DEFAULT_IDLE_TIMEOUT;
	size_t min_connections = DEFAULT_MIN_CONNECTIONS;
	int acquire_timeout = DEFAULT_ACQUIRE_TIMEOUT;
};

// Connection metadata for tracking in pool
struct ConnectionMetadata {
	std::shared_ptr<TdsConnection> connection;
	uint64_t connection_id;
	std::chrono::steady_clock::time_point last_released;
};

// Pool statistics for monitoring
struct PoolStatistics {
	size_t total_connections = 0;
	size_t idle_connections = 0;
	size_t active_connections = 0;
	size_t connections_created = 0;
	size_t connections_closed = 0;
	size_t acquire_count = 0;
	size_t acquire_timeout_count = 0;
	uint64_t acquire_wait_total_ms = 0;
};

// Connection factory function type
using ConnectionFactory = std::function<std::shared_ptr<TdsConnection>()>;

// Thread-safe connection pool for a single database context
class ConnectionPool {
public:
	ConnectionPool(const std::string& context_name, PoolConfiguration config, ConnectionFactory factory);
	~ConnectionPool();

	// Non-copyable, non-movable
	ConnectionPool(const ConnectionPool&) = delete;
	ConnectionPool& operator=(const ConnectionPool&) = delete;
	ConnectionPool(ConnectionPool&&) = delete;
	ConnectionPool& operator=(ConnectionPool&&) = delete;

	// Acquire a connection from the pool
	// Blocks up to acquire_timeout if pool is exhausted
	// Returns nullptr on timeout
	std::shared_ptr<TdsConnection> Acquire(int timeout_ms = -1);

	// Release a connection back to the pool
	void Release(std::shared_ptr<TdsConnection> conn);

	// Get current pool statistics
	PoolStatistics GetStats() const;

	// Shutdown the pool (closes all connections)
	void Shutdown();

	// Get context name
	const std::string& GetContextName() const { return context_name_; }

private:
	std::string context_name_;
	PoolConfiguration config_;
	ConnectionFactory factory_;

	// Connection storage
	std::queue<ConnectionMetadata> idle_connections_;
	std::unordered_map<uint64_t, std::shared_ptr<TdsConnection>> active_connections_;

	// Synchronization
	mutable std::mutex pool_mutex_;
	std::condition_variable available_cv_;

	// Statistics
	PoolStatistics stats_;
	uint64_t next_connection_id_;

	// Background cleanup
	std::thread cleanup_thread_;
	std::atomic<bool> shutdown_flag_;

	// Internal methods
	void CleanupThreadFunc();
	std::shared_ptr<TdsConnection> TryAcquireIdle();
	std::shared_ptr<TdsConnection> CreateNewConnection();
	bool ValidateConnection(std::shared_ptr<TdsConnection>& conn);
};

}  // namespace tds
}  // namespace duckdb
