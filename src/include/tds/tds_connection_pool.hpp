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
	// Times a connection was acquired. A blocking Acquire counts its ATTEMPT
	// here (so a timeout is counted); an optional TryAcquire counts only when it
	// actually got one, because an extra bulk-load writer probes once per chunk
	// and those attempts are not acquisitions (review of 0e12914).
	size_t acquire_count = 0;
	size_t acquire_timeout_count = 0;  // blocking Acquire only; a probe never waits
	size_t creation_failures = 0;	   // factory threw or returned nothing (issue #302)
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

	// Acquire a connection from the pool. Blocks up to acquire_timeout (or
	// timeout_ms) while others are active and a Release may serve the request;
	// with nothing active a creation that fails is reported at once.
	// Returns nullptr on failure and, if `failure` is given, says why THIS call
	// failed: "pool 'x' could not create a connection: <reason>" or
	// "pool 'x' timed out (N active of M, limit L)". Per call, not pool state:
	// a timeout on a healthy pool is never rendered with a creation failure some
	// other thread hit earlier (issue #302, review 1538).
	std::shared_ptr<TdsConnection> Acquire(int timeout_ms = -1, std::string *failure = nullptr);

	//! Never waits: an idle connection, a new one while the pool is below its
	//! limit, or null. For an OPTIONAL connection -- an extra bulk-load writer
	//! -- whose absence is the normal outcome on a pool the statement is already
	//! using, so a null here is not counted in acquire_timeout_count: every
	//! parallel load would otherwise report "timeouts" that delayed nothing
	//! (review of #382).
	//! `creation_failed` (optional) is set when the null came from a connection
	//! this call tried to CREATE and could not -- a login the server refuses,
	//! an expired token, a refused dial -- rather than from a pool with nothing
	//! free. The first does not clear on a later try; the second does (review
	//! of #382). Neither form counts in acquire_count or acquire_timeout_count.
	std::shared_ptr<TdsConnection> TryAcquire(std::string *failure = nullptr, bool *creation_failed = nullptr);

	//! An IDLE connection or null -- never a login. For work that is worth
	//! doing only if it costs no dial: the cache warm-up after a transaction
	//! (issue #383), which with `mssql_connection_cache = false`, or after the
	//! pinned connection was closed, finds nothing idle and is skipped rather
	//! than paying a TCP+TLS+LOGIN7 on COMMIT's path (review of #386). A
	//! connection it returns is counted in acquire_count like any checkout; a
	//! null is counted nowhere.
	std::shared_ptr<TdsConnection> TryAcquireIdleOnly();

	// Release a connection back to the pool
	void Release(std::shared_ptr<TdsConnection> conn);

	//! Issue #324: open connections up front until the pool holds `target` of
	//! them (capped at the limit), their logins run CONCURRENTLY -- one thread
	//! per connection -- and land idle. A TDS login is several round trips
	//! (~175 ms on loopback), so N of them cost about one instead of N.
	//! Blocks until every login has finished. Returns how many were opened;
	//! a failure does not throw -- the pool opens the rest on demand as before
	//! -- and its reason is recorded like any creation failure and, if
	//! `failure` is given, handed back.
	size_t Prewarm(size_t target, std::string *failure = nullptr);

	//! Issue #324: take a connection logged in elsewhere -- the one ATTACH's
	//! validation opened with the factory's own parameters -- as an idle
	//! connection of this pool. Returns false (and closes it) when the pool is
	//! full, shut down, or the connection is not Idle.
	bool Adopt(std::shared_ptr<TdsConnection> conn);

	// Get current pool statistics
	PoolStatistics GetStats() const;

	//! Why the last attempt to create a connection failed -- the factory's
	//! exception text, or a fixed line if it returned nullptr silently. Empty
	//! until a creation has failed, and cleared again by the next success, so
	//! `mssql_pool_stats` shows a live problem rather than history. Issue #302:
	//! before this, an expired Azure AD token, a wrong password and an
	//! unreachable host all reached the caller as "Failed to acquire connection
	//! from pool (timeout)".
	std::string GetLastCreateError() const;

	// Pin counter — tracks connections currently pinned to active DuckDB
	// transactions (spec 047 FR-005). Migrated from the deleted
	// MssqlPoolManager::pinned_counts_ map. Lock-free; safe to call from any
	// thread.
	void IncrementPinned();
	void DecrementPinned();
	int64_t GetPinnedCount() const noexcept;

	// Shutdown the pool (closes all connections)
	void Shutdown();

	//! Milliseconds since the recorded creation error, or -1 when there is
	//! none (never failed, or cleared by a creation success).
	int64_t GetLastCreateErrorAgeMs() const;

	// Get context name
	const std::string &GetContextName() const {
		return context_name_;
	}

private:
	std::string context_name_;
	PoolConfiguration config_;
	ConnectionFactory factory_;

	// Creation-failure state, guarded by pool_mutex_ (issue #302). While others
	// are active a factory that has just failed is not retried on every wakeup:
	// the next attempt waits create_backoff_ms_, doubling to
	// CREATE_BACKOFF_MAX_MS, since each attempt against a server that drops the
	// connection costs a full login read. With nothing active there is nothing
	// to wait for, and every Acquire dials once.
	static constexpr int CREATE_BACKOFF_INITIAL_MS = 250;
	static constexpr int CREATE_BACKOFF_MAX_MS = 4000;
	std::string last_create_error_;
	// When last_create_error_ was recorded; exposed as an age by pool stats so
	// a reader can tell a live failure from one a healthy pool has outlived.
	std::chrono::steady_clock::time_point last_create_error_at_{};
	std::chrono::steady_clock::time_point next_create_allowed_{};
	int create_backoff_ms_ = CREATE_BACKOFF_INITIAL_MS;

	// Connection storage
	std::queue<ConnectionMetadata> idle_connections_;
	std::unordered_map<uint64_t, std::shared_ptr<TdsConnection>> active_connections_;

	// Synchronization. available_cv_ is reserved for Acquire() waiters —
	// Release()'s notify_one must always reach a thread blocked on pool
	// exhaustion. The cleanup thread parks on its own cleanup_cv_ (notified
	// only by Shutdown()) so it can never consume that wakeup.
	mutable std::mutex pool_mutex_;
	std::condition_variable available_cv_;
	std::condition_variable cleanup_cv_;

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
	std::shared_ptr<TdsConnection> AcquireImpl(int timeout_ms, std::string *failure, bool optional,
											   bool *creation_failed);
	std::shared_ptr<TdsConnection> TryAcquireIdle();
	// The one way a connection enters idle_connections_ -- Release, Adopt and
	// Prewarm alike (review of #386: the two copies had dropped Release's
	// guards). Pools `conn` when the pool is up, caching is on and the
	// connection is Idle and alive; otherwise closes it. pool_mutex_ held.
	// Returns whether it was pooled; the caller owns the stats either way.
	bool PoolIfReusableLocked(std::shared_ptr<TdsConnection> &conn, uint64_t connection_id);
	// The bookkeeping of a creation that succeeded / failed, shared by
	// AcquireImpl and Prewarm (issue #302's backoff and recorded reason).
	// pool_mutex_ held. Neither counts creation_failures: the caller does.
	void RecordCreateSuccessLocked();
	void RecordCreateFailureLocked(const std::string &error);
	// Runs the factory with pool_mutex_ RELEASED (blocking I/O). On failure
	// returns nullptr and puts the reason in `error`; the caller records it.
	std::shared_ptr<TdsConnection> CreateNewConnection(std::string &error);

	// "pool 'x' timed out (N active of M, limit L)", plus this call's own
	// creation failure when it made one. pool_mutex_ must be held.
	std::string DescribeTimeoutLocked(const std::string &own_create_error) const;
	bool ValidateConnection(std::shared_ptr<TdsConnection> &conn);
};

}  // namespace tds
}  // namespace duckdb
