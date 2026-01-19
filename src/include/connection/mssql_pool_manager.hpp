#pragma once

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

	// Get or create a pool for a context
	// Parameters:
	//   use_encrypt - if true, enables TLS encryption for all connections in the pool
	tds::ConnectionPool *GetOrCreatePool(const std::string &context_name, const MSSQLPoolConfig &config,
										 const std::string &host, uint16_t port, const std::string &username,
										 const std::string &password, const std::string &database,
										 bool use_encrypt = false);

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

private:
	MssqlPoolManager() = default;

	std::mutex manager_mutex_;
	std::unordered_map<std::string, std::unique_ptr<tds::ConnectionPool>> pools_;
};

}  // namespace duckdb
