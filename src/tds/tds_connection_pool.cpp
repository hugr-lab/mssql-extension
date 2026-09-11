#include "tds/tds_connection_pool.hpp"
#include <algorithm>
#include "duckdb/common/error_data.hpp"

#include "duckdb/common/assert.hpp"

#include <cstdlib>

namespace duckdb {
namespace tds {

// Debug logging infrastructure (T005-T006)
static int GetPoolDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
	return level;
}

#define MSSQL_POOL_DEBUG_LOG(lvl, fmt, ...)                           \
	do {                                                              \
		if (GetPoolDebugLevel() >= lvl)                               \
			fprintf(stderr, "[MSSQL POOL] " fmt "\n", ##__VA_ARGS__); \
	} while (0)

ConnectionPool::ConnectionPool(const std::string &context_name, PoolConfiguration config, ConnectionFactory factory)
	: context_name_(context_name),
	  config_(std::move(config)),
	  factory_(std::move(factory)),
	  next_connection_id_(1),
	  shutdown_flag_(false) {
	// Start background cleanup thread
	cleanup_thread_ = std::thread(&ConnectionPool::CleanupThreadFunc, this);
}

ConnectionPool::~ConnectionPool() noexcept {
	// Spec 047 T046k: explicit noexcept + try/catch — a throw from teardown
	// during `~AttachedDatabase` unwind would invoke std::terminate. Failures
	// here are best-effort (socket close errors, log emission failures) and
	// we have no caller to report them to anyway.
	try {
		Shutdown();
	} catch (const std::exception &e) {
		// PR #118 review M1: debug-gated stderr so silent destructor swallow
		// doesn't hide pool-teardown errors in production diagnosis paths.
		MSSQL_POOL_DEBUG_LOG(1, "~ConnectionPool: swallowed exception during Shutdown: %s", e.what());
	} catch (...) {
		MSSQL_POOL_DEBUG_LOG(1, "~ConnectionPool: swallowed unknown exception during Shutdown");
	}
}

void ConnectionPool::Shutdown() {
	// Thread affinity (PR #179 review): with shared_ptr pool ownership this can
	// run on WHATEVER thread drops the last strong reference — including a
	// DuckDB worker thread inside ~MSSQLResultStream when its weak_ptr::lock()
	// wins a race against DETACH. That is safe: joining cleanup_thread_ is
	// legal from any thread (the cleanup thread holds no pool references, so a
	// self-join is impossible), and everything below is self-contained.
	//
	// Spec 047 T046l: surface DuckDB quiescence-contract violations in debug
	// builds via D_ASSERT. PR #118 review M2: also emit a release-mode warning
	// — silent UB on a real quiescence violation is worse for operators than
	// one stderr line.
	//
	// Spec 052 fix (PR #127): D_ASSERT in DuckDB-debug **throws**
	// InternalException (it doesn't call abort() like libc assert). If the
	// throw fires while cleanup_thread_ is still joinable, the unwind
	// skipped the join, ~ConnectionPool catches the exception but then
	// ~std::thread on the joinable cleanup_thread_ called std::terminate().
	// Reorder: signal+join cleanup thread first, close connections, THEN
	// emit the warning and run the assert at the very end — by which point
	// the unwind path can't strand any owned resource. The warning is the
	// operator-visibility signal; if it printed, the violation IS reported,
	// even if the trailing assert later throws.
	const size_t leaked = active_connections_.size();

	// Signal shutdown. The flag is set under pool_mutex_ so the cleanup
	// thread cannot check it (false) and then enter its cv wait AFTER the
	// notify below — that missed-wakeup race would put the join back on the
	// 1-second wait_for timeout.
	{
		std::lock_guard<std::mutex> signal_lock(pool_mutex_);
		shutdown_flag_.store(true);
	}

	// Wake up cleanup thread (its own CV) and any Acquire() waiters.
	cleanup_cv_.notify_all();
	available_cv_.notify_all();

	// Wait for cleanup thread to finish (do this BEFORE the assert that may
	// throw, otherwise the noexcept catch upstairs followed by ~std::thread
	// on a still-joinable thread = std::terminate).
	if (cleanup_thread_.joinable()) {
		cleanup_thread_.join();
	}

	// Close all connections
	std::lock_guard<std::mutex> lock(pool_mutex_);

	// Close idle connections
	while (!idle_connections_.empty()) {
		auto &meta = idle_connections_.front();
		if (meta.connection) {
			meta.connection->Close();
		}
		idle_connections_.pop();
		stats_.connections_closed++;
	}

	// Close active connections.
	// Spec 052 PR #127: dump per-connection diagnostics for every leaked
	// active conn (id, SPID, state, in-flight ref count). This is what
	// tells us WHERE the leak originated — without it the leak warning
	// only says "1 connection" with no clue which call path stranded it.
	for (auto &pair : active_connections_) {
		if (pair.second) {
			fprintf(stderr, "[MSSQL POOL] LEAKED active conn id=%llu spid=%u state=%d use_count=%ld pool='%s'\n",
					(unsigned long long)pair.first, (unsigned)pair.second->GetSpid(), (int)pair.second->GetState(),
					(long)pair.second.use_count(), context_name_.c_str());
			pair.second->Close();
		}
		stats_.connections_closed++;
	}
	active_connections_.clear();

	stats_.total_connections = 0;
	stats_.idle_connections = 0;
	stats_.active_connections = 0;

	// Spec 052 fix (PR #127): emit the warning + assert AFTER cleanup so that
	// if D_ASSERT throws (DuckDB-debug behaviour), no owned resource (cleanup
	// thread, sockets) is stranded by the unwind. `leaked` was captured at
	// entry — by this point active_connections_ is empty regardless, but the
	// quiescence-contract violation is what the operator needs to see.
	if (leaked > 0) {
		fprintf(stderr,
				"[MSSQL POOL] WARNING: ~ConnectionPool '%s' shut down with %zu connection(s) still "
				"checked out — DuckDB quiescence contract violated. Held connections were force-"
				"closed; any thread that still held a reference will see EBADF / connection reset "
				"on its next socket read. On Windows the closesocket-vs-recv race is undefined.\n",
				context_name_.c_str(), leaked);
	}
	D_ASSERT(leaked == 0);
}

std::shared_ptr<TdsConnection> ConnectionPool::Acquire(int timeout_ms, std::string *failure) {
	MSSQL_POOL_DEBUG_LOG(1, "Acquire called on pool '%s'", context_name_.c_str());
	if (shutdown_flag_.load()) {
		if (failure) {
			*failure = "pool '" + context_name_ + "' is shut down";
		}
		return nullptr;
	}

	// Use configured timeout if not specified
	if (timeout_ms < 0) {
		timeout_ms = config_.acquire_timeout * 1000;
	}

	auto start = std::chrono::steady_clock::now();

	// Why THIS call's own creation attempt failed, if it made one and then went
	// on to wait for a Release. Per call, not pool state (review 1538): a
	// timeout on a healthy pool must not be rendered with a reason some other
	// thread hit hours ago.
	std::string own_create_error;

	std::unique_lock<std::mutex> lock(pool_mutex_);
	stats_.acquire_count++;

	while (true) {
		// Try to get an idle connection
		auto conn = TryAcquireIdle();
		if (conn) {
			auto elapsed =
				std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
			stats_.acquire_wait_total_ms += elapsed;
			return conn;
		}

		// Try to create a new connection if under the limit. The backoff after
		// a failed creation applies only while others are active: then a
		// Release may serve this caller, and the pool should not dial on every
		// wakeup. With nothing active there is nothing to wait for, so every
		// Acquire dials once and reports its own result -- the first draft
		// applied the backoff there too, and back-to-back failing statements on
		// an empty pool stalled 250 ms .. 4 s each before the same error
		// (issue #302, review 1538).
		if (stats_.total_connections < config_.connection_limit &&
			(stats_.active_connections == 0 || std::chrono::steady_clock::now() >= next_create_allowed_)) {
			std::string error;
			lock.unlock();
			conn = CreateNewConnection(error);
			lock.lock();

			if (conn) {
				// A success resets the backoff so a transient failure does not
				// slow the next refill, and clears the recorded error: a pool
				// that has recovered has nothing to report. The first draft kept
				// it forever, and every later exhaustion timeout was rendered as
				// that stale creation failure (review 1538).
				create_backoff_ms_ = CREATE_BACKOFF_INITIAL_MS;
				next_create_allowed_ = std::chrono::steady_clock::time_point{};
				last_create_error_.clear();
				uint64_t id = next_connection_id_++;
				active_connections_[id] = conn;
				stats_.total_connections++;
				stats_.active_connections++;
				stats_.connections_created++;

				auto elapsed =
					std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
						.count();
				stats_.acquire_wait_total_ms += elapsed;
				return conn;
			}

			// Creation failed. Issue #302: this used to fall through to the same
			// wait an exhausted pool does -- for a Release that, with nothing
			// active, cannot come -- and report "(timeout)" after the full
			// acquire_timeout, with the reason discarded. Measured: an expired
			// Azure AD token cost 600 s and two login attempts, and read
			// identically to a wrong password or an unreachable host.
			stats_.creation_failures++;
			last_create_error_ = error;
			own_create_error = error;
			next_create_allowed_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(create_backoff_ms_);
			// Not std::min: it binds CREATE_BACKOFF_MAX_MS by reference, which
			// odr-uses an in-class constexpr with no out-of-line definition
			// before C++17 (review 1538).
			create_backoff_ms_ =
				create_backoff_ms_ * 2 > CREATE_BACKOFF_MAX_MS ? CREATE_BACKOFF_MAX_MS : create_backoff_ms_ * 2;
			if (stats_.active_connections == 0) {
				// Nothing can be released, so waiting would only run out the
				// clock. Fail now, with this call's reason.
				if (failure) {
					*failure = "pool '" + context_name_ + "' could not create a connection: " + error;
				}
				auto elapsed =
					std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
						.count();
				stats_.acquire_wait_total_ms += elapsed;
				return nullptr;
			}
			// Others are active: a Release may still serve this request, so
			// keep waiting -- but retry creation on the backoff schedule, not on
			// every wakeup.
		}

		// Pool exhausted, wait for a connection to be released
		if (timeout_ms == 0) {
			stats_.acquire_timeout_count++;
			if (failure) {
				*failure = DescribeTimeoutLocked(own_create_error);
			}
			return nullptr;
		}

		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();

		if (elapsed >= timeout_ms) {
			stats_.acquire_timeout_count++;
			stats_.acquire_wait_total_ms += elapsed;
			if (failure) {
				*failure = DescribeTimeoutLocked(own_create_error);
			}
			return nullptr;
		}

		int remaining = timeout_ms - static_cast<int>(elapsed);
		// Wake for the next creation attempt if that comes first (issue #302).
		if (next_create_allowed_ > now && stats_.total_connections < config_.connection_limit) {
			auto until_create =
				std::chrono::duration_cast<std::chrono::milliseconds>(next_create_allowed_ - now).count();
			remaining = std::min<int>(remaining, static_cast<int>(until_create) + 1);
		}
		available_cv_.wait_for(lock, std::chrono::milliseconds(remaining));

		if (shutdown_flag_.load()) {
			if (failure) {
				*failure = "pool '" + context_name_ + "' is shut down";
			}
			return nullptr;
		}
	}
}

void ConnectionPool::Release(std::shared_ptr<TdsConnection> conn) {
	// Post-Shutdown contract (PR #179 review): drop, never enqueue. Under
	// shared_ptr ownership this is belt-and-suspenders — Shutdown() only runs
	// from ~ConnectionPool, which cannot execute while a Release caller still
	// holds a strong reference (the ~MSSQLResultStream path holds one from
	// weak_ptr::lock() for the duration of this call).
	if (!conn || shutdown_flag_.load()) {
		return;
	}

	std::lock_guard<std::mutex> lock(pool_mutex_);

	// Re-check under the mutex: if a shutdown raced past the unlocked check
	// above, close instead of enqueueing into a quiesced pool.
	if (shutdown_flag_.load()) {
		conn->Close();
		return;
	}

	// Find and remove from active connections
	uint64_t found_id = 0;
	for (auto it = active_connections_.begin(); it != active_connections_.end(); ++it) {
		if (it->second.get() == conn.get()) {
			found_id = it->first;
			active_connections_.erase(it);
			stats_.active_connections--;
			break;
		}
	}

	// T010: Validate connection state before returning to pool (FR-002)
	// Connections must be in Idle state to be safely reused
	if (conn->GetState() != ConnectionState::Idle) {
		MSSQL_POOL_DEBUG_LOG(1, "Closing connection in non-Idle state: %d (pool '%s')",
							 static_cast<int>(conn->GetState()), context_name_.c_str());
		conn->Close();
		stats_.connections_closed++;
		stats_.total_connections--;
		available_cv_.notify_one();
		return;
	}

	// If caching disabled or connection is dead, close it
	if (!config_.connection_cache || !conn->IsAlive()) {
		conn->Close();
		stats_.connections_closed++;
		stats_.total_connections--;
		available_cv_.notify_one();
		return;
	}

	// Return to idle pool
	ConnectionMetadata meta;
	meta.connection = std::move(conn);
	meta.connection_id = found_id ? found_id : next_connection_id_++;
	meta.last_released = std::chrono::steady_clock::now();

	idle_connections_.push(std::move(meta));
	stats_.idle_connections++;

	available_cv_.notify_one();
}

PoolStatistics ConnectionPool::GetStats() const {
	std::lock_guard<std::mutex> lock(pool_mutex_);
	PoolStatistics result = stats_;
	result.pinned_count = pinned_count_.load(std::memory_order_relaxed);
	return result;
}

void ConnectionPool::IncrementPinned() {
	pinned_count_.fetch_add(1, std::memory_order_relaxed);
}

void ConnectionPool::DecrementPinned() {
	pinned_count_.fetch_sub(1, std::memory_order_relaxed);
}

int64_t ConnectionPool::GetPinnedCount() const noexcept {
	return pinned_count_.load(std::memory_order_relaxed);
}

std::shared_ptr<TdsConnection> ConnectionPool::TryAcquireIdle() {
	// pool_mutex_ must be held

	while (!idle_connections_.empty()) {
		auto meta = std::move(idle_connections_.front());
		idle_connections_.pop();
		stats_.idle_connections--;

		if (ValidateConnection(meta.connection)) {
			uint64_t id = meta.connection_id;
			active_connections_[id] = meta.connection;
			stats_.active_connections++;
			meta.connection->UpdateLastUsed();
			return meta.connection;
		}

		// Connection is dead, close it
		meta.connection->Close();
		stats_.connections_closed++;
		stats_.total_connections--;
	}

	return nullptr;
}

std::shared_ptr<TdsConnection> ConnectionPool::CreateNewConnection(std::string &error) {
	// pool_mutex_ must NOT be held (blocking I/O)
	std::shared_ptr<TdsConnection> conn;
	error.clear();
	try {
		conn = factory_();
		if (!conn) {
			error = "connection factory returned no connection and no reason";
		}
	} catch (const std::exception &e) {
		// The factory says why -- an expired token, the server's login error,
		// a refused dial. Kept for the caller; the pool itself only needs the
		// nullptr (issue #302). ErrorData, not what(): a DuckDB exception's
		// what() is its JSON serialization on the 2.0 line, and the first
		// version of this pasted `{"exception_type":"Connection",...}` into the
		// user's error.
		error = ErrorData(e).RawMessage();
	}
	if (!error.empty()) {
		MSSQL_POOL_DEBUG_LOG(1, "CreateNewConnection failed on pool '%s': %s", context_name_.c_str(), error.c_str());
	}
	return conn;
}

std::string ConnectionPool::GetLastCreateError() const {
	std::lock_guard<std::mutex> lock(pool_mutex_);
	return last_create_error_;
}

std::string ConnectionPool::DescribeTimeoutLocked(const std::string &own_create_error) const {
	// Two different things end in a nullptr from Acquire and used to read the
	// same: the pool is full and nobody released in time, or the pool tried to
	// create a connection and could not -- an expired Azure AD token, the
	// server's login error, a refused dial (issue #302). This is the first;
	// when the caller also tried to create on the way and failed, that reason
	// rides along, because with others active it is the one that explains why
	// the pool did not simply grow.
	std::string message = "pool '" + context_name_ + "' timed out (" + std::to_string(stats_.active_connections) +
						  " active of " + std::to_string(stats_.total_connections) + ", limit " +
						  std::to_string(config_.connection_limit) + ")";
	if (!own_create_error.empty()) {
		message += "; this call's own attempt to create a connection failed: " + own_create_error;
	}
	return message;
}

bool ConnectionPool::ValidateConnection(std::shared_ptr<TdsConnection> &conn) {
	if (!conn || !conn->IsAlive()) {
		return false;
	}

	// For long-idle connections, perform TDS ping
	if (conn->IsLongIdle()) {
		return conn->ValidateWithPing();
	}

	return true;
}

void ConnectionPool::CleanupThreadFunc() {
	while (!shutdown_flag_.load()) {
		// Wait up to 1 second between cleanup cycles. Waits on cleanup_cv_
		// (which Shutdown() notifies) instead of a blind sleep: ~ConnectionPool
		// joins this thread, so a plain sleep_for(1s) here made every DETACH /
		// catalog teardown — and with it every short-lived CLI session — pay up
		// to a full second before the pool could shut down. Must NOT wait on
		// available_cv_: a predicated wait there consumes Release()'s
		// notify_one meant for a pool-exhausted Acquire() waiter.
		{
			std::unique_lock<std::mutex> wait_lock(pool_mutex_);
			cleanup_cv_.wait_for(wait_lock, std::chrono::seconds(1), [this] { return shutdown_flag_.load(); });
		}

		if (shutdown_flag_.load()) {
			break;
		}

		std::lock_guard<std::mutex> lock(pool_mutex_);

		if (config_.idle_timeout <= 0) {
			continue;  // No idle timeout configured
		}

		auto now = std::chrono::steady_clock::now();
		size_t to_keep = config_.min_connections > stats_.active_connections
							 ? config_.min_connections - stats_.active_connections
							 : 0;

		// Check each idle connection
		std::queue<ConnectionMetadata> remaining;
		while (!idle_connections_.empty()) {
			auto meta = std::move(idle_connections_.front());
			idle_connections_.pop();

			auto idle_duration = std::chrono::duration_cast<std::chrono::seconds>(now - meta.last_released).count();

			bool should_close = idle_duration > config_.idle_timeout && remaining.size() >= to_keep;

			if (should_close) {
				meta.connection->Close();
				stats_.connections_closed++;
				stats_.total_connections--;
				stats_.idle_connections--;
			} else {
				remaining.push(std::move(meta));
			}
		}

		idle_connections_ = std::move(remaining);
	}
}

}  // namespace tds
}  // namespace duckdb
