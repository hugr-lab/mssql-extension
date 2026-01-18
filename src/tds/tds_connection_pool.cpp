#include "tds/tds_connection_pool.hpp"

namespace duckdb {
namespace tds {

ConnectionPool::ConnectionPool(const std::string& context_name, PoolConfiguration config, ConnectionFactory factory)
    : context_name_(context_name),
      config_(std::move(config)),
      factory_(std::move(factory)),
      next_connection_id_(1),
      shutdown_flag_(false) {

	// Start background cleanup thread
	cleanup_thread_ = std::thread(&ConnectionPool::CleanupThreadFunc, this);
}

ConnectionPool::~ConnectionPool() {
	Shutdown();
}

void ConnectionPool::Shutdown() {
	// Signal shutdown
	shutdown_flag_.store(true);

	// Wake up cleanup thread
	available_cv_.notify_all();

	// Wait for cleanup thread to finish
	if (cleanup_thread_.joinable()) {
		cleanup_thread_.join();
	}

	// Close all connections
	std::lock_guard<std::mutex> lock(pool_mutex_);

	// Close idle connections
	while (!idle_connections_.empty()) {
		auto& meta = idle_connections_.front();
		if (meta.connection) {
			meta.connection->Close();
		}
		idle_connections_.pop();
		stats_.connections_closed++;
	}

	// Close active connections
	for (auto& pair : active_connections_) {
		if (pair.second) {
			pair.second->Close();
		}
		stats_.connections_closed++;
	}
	active_connections_.clear();

	stats_.total_connections = 0;
	stats_.idle_connections = 0;
	stats_.active_connections = 0;
}

std::shared_ptr<TdsConnection> ConnectionPool::Acquire(int timeout_ms) {
	if (shutdown_flag_.load()) {
		return nullptr;
	}

	// Use configured timeout if not specified
	if (timeout_ms < 0) {
		timeout_ms = config_.acquire_timeout * 1000;
	}

	auto start = std::chrono::steady_clock::now();

	std::unique_lock<std::mutex> lock(pool_mutex_);
	stats_.acquire_count++;

	while (true) {
		// Try to get an idle connection
		auto conn = TryAcquireIdle();
		if (conn) {
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			    std::chrono::steady_clock::now() - start).count();
			stats_.acquire_wait_total_ms += elapsed;
			return conn;
		}

		// Try to create a new connection if under limit
		if (stats_.total_connections < config_.connection_limit) {
			lock.unlock();
			conn = CreateNewConnection();
			lock.lock();

			if (conn) {
				uint64_t id = next_connection_id_++;
				active_connections_[id] = conn;
				stats_.total_connections++;
				stats_.active_connections++;
				stats_.connections_created++;

				auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				    std::chrono::steady_clock::now() - start).count();
				stats_.acquire_wait_total_ms += elapsed;
				return conn;
			}
		}

		// Pool exhausted, wait for a connection to be released
		if (timeout_ms == 0) {
			stats_.acquire_timeout_count++;
			return nullptr;
		}

		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();

		if (elapsed >= timeout_ms) {
			stats_.acquire_timeout_count++;
			stats_.acquire_wait_total_ms += elapsed;
			return nullptr;
		}

		int remaining = timeout_ms - static_cast<int>(elapsed);
		available_cv_.wait_for(lock, std::chrono::milliseconds(remaining));

		if (shutdown_flag_.load()) {
			return nullptr;
		}
	}
}

void ConnectionPool::Release(std::shared_ptr<TdsConnection> conn) {
	if (!conn || shutdown_flag_.load()) {
		return;
	}

	std::lock_guard<std::mutex> lock(pool_mutex_);

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
	return stats_;
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

std::shared_ptr<TdsConnection> ConnectionPool::CreateNewConnection() {
	// pool_mutex_ must NOT be held (blocking I/O)
	return factory_();
}

bool ConnectionPool::ValidateConnection(std::shared_ptr<TdsConnection>& conn) {
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
		// Sleep for 1 second between cleanup cycles
		std::this_thread::sleep_for(std::chrono::seconds(1));

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

			auto idle_duration = std::chrono::duration_cast<std::chrono::seconds>(
			    now - meta.last_released).count();

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
