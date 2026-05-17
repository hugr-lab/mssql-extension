#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "connection/mssql_settings.hpp"
#include "tds/tds_connection_pool.hpp"

namespace duckdb {

// Manages connection pools per attached database context
// Singleton pattern for global access
class MssqlPoolManager {
public:
	static MssqlPoolManager &Instance();

	// Get or create a pool for a context (SQL authentication)
	// Parameters:
	//   use_encrypt   - if true, enables TLS encryption for all connections in the pool
	//   instance_name - Spec 046: when non-empty, each connection's LOGIN7 ServerName
	//                   field is set to "host\instance_name" (matching every other
	//                   SQL Server client). Empty means default instance (pre-spec-045
	//                   behaviour, ServerName == host).
	tds::ConnectionPool *GetOrCreatePool(const std::string &context_name, const MSSQLPoolConfig &config,
										 const std::string &host, uint16_t port, const std::string &username,
										 const std::string &password, const std::string &database,
										 bool use_encrypt = false, const std::string &instance_name = "");

	// Get or create a pool for a context (Azure AD authentication)
	// Parameters:
	//   fedauth_token_utf16le - pre-encoded UTF-16LE access token
	//   use_encrypt - TLS encryption (required for Azure)
	//   instance_name - Spec 046: see GetOrCreatePool. Azure SQL doesn't use named
	//                   instances in practice but the parameter is accepted for
	//                   symmetry with the SQL-auth factory.
	// Note: For Azure auth, token refresh is handled separately
	tds::ConnectionPool *GetOrCreatePoolWithAzureAuth(const std::string &context_name, const MSSQLPoolConfig &config,
													  const std::string &host, uint16_t port,
													  const std::string &database,
													  const std::vector<uint8_t> &fedauth_token_utf16le,
													  bool use_encrypt = true, const std::string &instance_name = "");

	// Get or create a pool for a context (Integrated Authentication: Kerberos / SSPI).
	// Spec 042.
	//
	// Each new connection in the pool builds a fresh IAuthenticator instance via
	// AuthStrategyFactory::Create(info), so credential refresh happens naturally on
	// every connection acquisition (kinit ticket refresh, etc.). The MSSQLConnectionInfo
	// is captured by value into the factory closure.
	tds::ConnectionPool *GetOrCreatePoolWithIntegratedAuth(const std::string &context_name,
														   const MSSQLPoolConfig &config,
														   const struct MSSQLConnectionInfo &info);

	// Get an existing pool (returns nullptr if not found)
	tds::ConnectionPool *GetPool(const std::string &context_name);

	// Remove a pool (called on DETACH)
	void RemovePool(const std::string &context_name);

	// Get pool statistics for a context
	tds::PoolStatistics GetPoolStats(const std::string &context_name);

	// Check if a pool exists
	bool HasPool(const std::string &context_name);

	// Get all pool names
	std::vector<std::string> GetAllPoolNames();

	// Pinned connection tracking (called by MSSQLTransaction)
	void IncrementPinnedCount(const std::string &context_name);
	void DecrementPinnedCount(const std::string &context_name);

	// Get pinned count for a context
	size_t GetPinnedCount(const std::string &context_name) const;

private:
	MssqlPoolManager() = default;

	mutable std::mutex manager_mutex_;
	std::unordered_map<std::string, std::unique_ptr<tds::ConnectionPool>> pools_;

	// Pinned connection counters per context (atomic for thread safety)
	mutable std::mutex pinned_mutex_;
	std::unordered_map<std::string, std::atomic<size_t>> pinned_counts_;
};

}  // namespace duckdb
