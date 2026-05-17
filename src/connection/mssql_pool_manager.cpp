#include "connection/mssql_pool_manager.hpp"

#include "mssql_storage.hpp"
#include "tds/auth/auth_strategy_factory.hpp"

#include <cstdio>

namespace duckdb {

MssqlPoolManager &MssqlPoolManager::Instance() {
	static MssqlPoolManager instance;
	return instance;
}

tds::ConnectionPool *MssqlPoolManager::GetOrCreatePool(const std::string &context_name, const MSSQLPoolConfig &config,
													   const std::string &host, uint16_t port,
													   const std::string &username, const std::string &password,
													   const std::string &database, bool use_encrypt,
													   const std::string &instance_name) {
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

	// Create connection factory that captures use_encrypt for TLS support.
	// Spec 046: when instance_name is non-empty, LOGIN7 ServerName is set
	// to "host\instance" so SQL Server logs and DMVs see the same form the
	// user typed in their connection string.
	auto factory = [host, port, username, password, database, use_encrypt,
					instance_name]() -> std::shared_ptr<tds::TdsConnection> {
		auto conn = std::make_shared<tds::TdsConnection>();
		if (!conn->Connect(host, port)) {
			return nullptr;
		}
		if (!instance_name.empty()) {
			conn->SetTdsServerName(host + "\\" + instance_name);
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

tds::ConnectionPool *MssqlPoolManager::GetOrCreatePoolWithAzureAuth(
	const std::string &context_name, const MSSQLPoolConfig &config, const std::string &host, uint16_t port,
	const std::string &database, const std::vector<uint8_t> &fedauth_token_utf16le, bool use_encrypt,
	const std::string &instance_name) {
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
	auto factory = [host, port, database, fedauth_token_utf16le, use_encrypt,
					instance_name]() -> std::shared_ptr<tds::TdsConnection> {
		auto conn = std::make_shared<tds::TdsConnection>();
		if (!conn->Connect(host, port)) {
			return nullptr;
		}
		if (!instance_name.empty()) {
			conn->SetTdsServerName(host + "\\" + instance_name);
		}
		// Use FEDAUTH authentication with pre-acquired token
		if (!conn->AuthenticateWithFedAuth(database, fedauth_token_utf16le, use_encrypt)) {
			return nullptr;
		}

		// Note: Warm-up query disabled - Fabric seems to have timing issues with pool connections
		// The connection is returned in Idle state after authentication

		return conn;
	};

	// Create the pool
	std::unique_ptr<tds::ConnectionPool> pool(new tds::ConnectionPool(context_name, pool_config, factory));
	auto *ptr = pool.get();
	pools_[context_name] = std::move(pool);

	return ptr;
}

tds::ConnectionPool *MssqlPoolManager::GetOrCreatePoolWithIntegratedAuth(const std::string &context_name,
																		 const MSSQLPoolConfig &config,
																		 const MSSQLConnectionInfo &info) {
	std::lock_guard<std::mutex> lock(manager_mutex_);

	auto it = pools_.find(context_name);
	if (it != pools_.end()) {
		return it->second.get();
	}

	tds::PoolConfiguration pool_config;
	pool_config.connection_limit = config.connection_limit;
	pool_config.connection_cache = config.connection_cache;
	pool_config.connection_timeout = config.connection_timeout;
	pool_config.idle_timeout = config.idle_timeout;
	pool_config.min_connections = config.min_connections;
	pool_config.acquire_timeout = config.acquire_timeout;

	// Copy info into the factory closure -- each new connection re-acquires Kerberos
	// credentials, so a ticket renewed via kinit is picked up on the next pool fill.
	//
	// spec 042 ultrareview bug_001: when the authenticator throws or the
	// LOGIN7 round trip fails (expired TGT, KDC outage, keytab rotated mid-
	// session, etc.), the verbatim GSSAPI status + actionable hint from
	// Krb5Authenticator::HintForMinor is the most useful thing we can show
	// the user. The previous implementation discarded those errors and the
	// pool surfaced only a generic "failed to acquire connection". We now
	// log them via the existing MSSQL_CONN_DEBUG_LOG and stderr so users
	// running with MSSQL_DEBUG=1 (or any time, on stderr) see the real
	// reason a pool refill failed.
	MSSQLConnectionInfo info_copy = info;
	auto factory = [info_copy]() -> std::shared_ptr<tds::TdsConnection> {
		auto conn = std::make_shared<tds::TdsConnection>();
		if (!conn->Connect(info_copy.host, info_copy.port)) {
			fprintf(stderr, "[MSSQL POOL] integrated-auth: TCP connect to %s:%u failed: %s\n", info_copy.host.c_str(),
					static_cast<unsigned>(info_copy.port), conn->GetLastError().c_str());
			return nullptr;
		}
		// Spec 046: LOGIN7 ServerName carries host\instance for named instances.
		if (!info_copy.instance_name.empty()) {
			conn->SetTdsServerName(info_copy.host + "\\" + info_copy.instance_name);
		}
		// Build a fresh strategy / authenticator for this connection so
		// gss_init_sec_context state is independent across pool connections.
		std::shared_ptr<tds::AuthenticationStrategy> strategy;
		try {
			strategy = tds::AuthStrategyFactory::Create(info_copy);
		} catch (const std::exception &e) {
			fprintf(stderr, "[MSSQL POOL] integrated-auth: AuthStrategyFactory::Create failed: %s\n", e.what());
			return nullptr;
		}
		if (!strategy) {
			fprintf(stderr, "[MSSQL POOL] integrated-auth: AuthStrategyFactory returned null strategy\n");
			return nullptr;
		}
		auto authenticator = strategy->GetAuthenticator();
		if (!authenticator) {
			fprintf(stderr, "[MSSQL POOL] integrated-auth: strategy provided no authenticator\n");
			return nullptr;
		}
		if (!conn->AuthenticateIntegrated(info_copy.database, authenticator, info_copy.use_encrypt)) {
			fprintf(stderr, "[MSSQL POOL] integrated-auth: %s\n", conn->GetLastError().c_str());
			return nullptr;
		}
		return conn;
	};

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
