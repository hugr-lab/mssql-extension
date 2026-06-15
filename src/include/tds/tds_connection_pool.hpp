#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include "tds_connection.hpp"
#include "tds_types.hpp"

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
	int64_t pinned_count = 0;  // Connections pinned to active transactions (spec 047 FR-005)
};

// Connection factory function type
using ConnectionFactory = std::function<std::shared_ptr<TdsConnection>()>;

// Thread-safe connection pool for a single database context.
//
// Spec 047 T046m — destruction contract:
//   Sockets close immediately on pool destruction. In-flight TDS requests
//   on other threads observe connection-reset on next read (EBADF / SSL
//   read error). Server-side rollback of any open transactions happens
//   via TCP FIN within seconds.
//
//   No graceful TDS ATTENTION cancel is sent during destruction — that
//   would require a cross-thread write to a connection's socket, racing
//   with the owning thread's read. The DuckDB extension contract
//   requires query quiescence before `~AttachedDatabase` runs (and
//   therefore before `~MSSQLCatalog` / `~ConnectionPool`); the debug
//   `D_ASSERT(active_connections_.empty())` in Shutdown() surfaces
//   any host that violates that contract.
//
//   Cooperative cancellation (atomic flag polled by the owner thread,
//   per spec 047 Constraints / non-goals) is tracked as a follow-up
//   spec — out of scope here.
class ConnectionPool {
public:
	ConnectionPool(const std::string &context_name, PoolConfiguration config, ConnectionFactory factory);
	// noexcept: destructor body wraps Shutdown() in try/catch — a throw from
	// teardown during `~AttachedDatabase` unwind would invoke std::terminate.
	~ConnectionPool() noexcept;

	// Non-copyable, non-movable
	ConnectionPool(const ConnectionPool &) = delete;
	ConnectionPool &operator=(const ConnectionPool &) = delete;
	ConnectionPool(ConnectionPool &&) = delete;
	ConnectionPool &operator=(ConnectionPool &&) = delete;

	// Acquire a connection from the pool
	// Blocks up to acquire_timeout if pool is exhausted
	// Returns nullptr on timeout
	std::shared_ptr<TdsConnection> Acquire(int timeout_ms = -1);

	// Release a connection back to the pool
	void Release(std::shared_ptr<TdsConnection> conn);

	// Get current pool statistics
	PoolStatistics GetStats() const;

	// Pin counter — tracks connections currently pinned to active DuckDB
	// transactions (spec 047 FR-005). Migrated from the deleted
	// MssqlPoolManager::pinned_counts_ map. Lock-free; safe to call from any
	// thread.
	void IncrementPinned();
	void DecrementPinned();
	int64_t GetPinnedCount() const noexcept;

	// Shutdown the pool (closes all connections)
	void Shutdown();

	// Get context name
	const std::string &GetContextName() const {
		return context_name_;
	}

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

	// Pin counter (spec 047 FR-005). Separate from pool_mutex_ — pin counting
	// is high-frequency on transaction begin/commit and should not contend
	// with Acquire/Release.
	std::atomic<int64_t> pinned_count_{0};

	// Internal methods
	void CleanupThreadFunc();
	std::shared_ptr<TdsConnection> TryAcquireIdle();
	std::shared_ptr<TdsConnection> CreateNewConnection();
	bool ValidateConnection(std::shared_ptr<TdsConnection> &conn);
};

}  // namespace tds
}  // namespace duckdb
