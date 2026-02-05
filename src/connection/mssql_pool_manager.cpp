#include "connection/mssql_pool_manager.hpp"

namespace duckdb {

MssqlPoolManager &MssqlPoolManager::Instance() {
	static MssqlPoolManager instance;
	return instance;
}

tds::ConnectionPool *MssqlPoolManager::GetOrCreatePool(const std::string &context_name, const MSSQLPoolConfig &config,
													   const std::string &host, uint16_t port,
													   const std::string &username, const std::string &password,
													   const std::string &database, bool use_encrypt) {
	std::lock_guard<std::mutex> lock(manager_mutex_);

	// Check if pool already exists
	auto it = pools_.find(context_name);
	if (it != pools_.end()) {
		return it->second.get();
	}

	// Create pool configuration
	tds::PoolConfiguration pool_config;
	pool_config.connection_limit = config.connection_limit;
	pool_config.connection_cache = config.connection_cache;
	pool_config.connection_timeout = config.connection_timeout;
	pool_config.idle_timeout = config.idle_timeout;
	pool_config.min_connections = config.min_connections;
	pool_config.acquire_timeout = config.acquire_timeout;

	// Create connection factory that captures use_encrypt for TLS support
	auto factory = [host, port, username, password, database, use_encrypt]() -> std::shared_ptr<tds::TdsConnection> {
		auto conn = std::make_shared<tds::TdsConnection>();
		if (!conn->Connect(host, port)) {
			return nullptr;
		}
		if (!conn->Authenticate(username, password, database, use_encrypt)) {
			return nullptr;
		}
		return conn;
	};

	// Create the pool
	std::unique_ptr<tds::ConnectionPool> pool(new tds::ConnectionPool(context_name, pool_config, factory));
	auto *ptr = pool.get();
	pools_[context_name] = std::move(pool);

	return ptr;
}

tds::ConnectionPool *MssqlPoolManager::GetOrCreatePoolWithAzureAuth(const std::string &context_name,
                                                                    const MSSQLPoolConfig &config,
                                                                    const std::string &host, uint16_t port,
                                                                    const std::string &database,
                                                                    const std::vector<uint8_t> &fedauth_token_utf16le,
                                                                    bool use_encrypt) {
	std::lock_guard<std::mutex> lock(manager_mutex_);

	// Check if pool already exists
	auto it = pools_.find(context_name);
	if (it != pools_.end()) {
		return it->second.get();
	}

	// Create pool configuration
	tds::PoolConfiguration pool_config;
	pool_config.connection_limit = config.connection_limit;
	pool_config.connection_cache = config.connection_cache;
	pool_config.connection_timeout = config.connection_timeout;
	pool_config.idle_timeout = config.idle_timeout;
	pool_config.min_connections = config.min_connections;
	pool_config.acquire_timeout = config.acquire_timeout;

	// Create connection factory for Azure AD authentication
	// Note: This captures the token by value. For token refresh, a separate mechanism is needed.
	auto factory = [host, port, database, fedauth_token_utf16le, use_encrypt]() -> std::shared_ptr<tds::TdsConnection> {
		auto conn = std::make_shared<tds::TdsConnection>();
		if (!conn->Connect(host, port)) {
			return nullptr;
		}
		// Use FEDAUTH authentication with pre-acquired token
		if (!conn->AuthenticateWithFedAuth(database, fedauth_token_utf16le, use_encrypt)) {
			return nullptr;
		}
		return conn;
	};

	// Create the pool
	std::unique_ptr<tds::ConnectionPool> pool(new tds::ConnectionPool(context_name, pool_config, factory));
	auto *ptr = pool.get();
	pools_[context_name] = std::move(pool);

	return ptr;
}

tds::ConnectionPool *MssqlPoolManager::GetPool(const std::string &context_name) {
	std::lock_guard<std::mutex> lock(manager_mutex_);
	auto it = pools_.find(context_name);
	if (it != pools_.end()) {
		return it->second.get();
	}
	return nullptr;
}

void MssqlPoolManager::RemovePool(const std::string &context_name) {
	std::lock_guard<std::mutex> lock(manager_mutex_);
	auto it = pools_.find(context_name);
	if (it != pools_.end()) {
		it->second->Shutdown();
		pools_.erase(it);
	}
}

tds::PoolStatistics MssqlPoolManager::GetPoolStats(const std::string &context_name) {
	std::lock_guard<std::mutex> lock(manager_mutex_);
	auto it = pools_.find(context_name);
	if (it != pools_.end()) {
		return it->second->GetStats();
	}
	return tds::PoolStatistics{};
}

bool MssqlPoolManager::HasPool(const std::string &context_name) {
	std::lock_guard<std::mutex> lock(manager_mutex_);
	return pools_.find(context_name) != pools_.end();
}

std::vector<std::string> MssqlPoolManager::GetAllPoolNames() {
	std::lock_guard<std::mutex> lock(manager_mutex_);
	std::vector<std::string> names;
	names.reserve(pools_.size());
	for (const auto &pair : pools_) {
		names.push_back(pair.first);
	}
	return names;
}

void MssqlPoolManager::IncrementPinnedCount(const std::string &context_name) {
	std::lock_guard<std::mutex> lock(pinned_mutex_);
	// Create entry if it doesn't exist (will be 0), then increment
	pinned_counts_[context_name]++;
}

void MssqlPoolManager::DecrementPinnedCount(const std::string &context_name) {
	std::lock_guard<std::mutex> lock(pinned_mutex_);
	auto it = pinned_counts_.find(context_name);
	if (it != pinned_counts_.end() && it->second > 0) {
		it->second--;
	}
}

size_t MssqlPoolManager::GetPinnedCount(const std::string &context_name) const {
	std::lock_guard<std::mutex> lock(pinned_mutex_);
	auto it = pinned_counts_.find(context_name);
	if (it != pinned_counts_.end()) {
		return it->second.load();
	}
	return 0;
}

}  // namespace duckdb
