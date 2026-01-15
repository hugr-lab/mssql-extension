#pragma once

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "tds/tds_connection.hpp"
#include <memory>
#include <unordered_map>
#include <mutex>

namespace duckdb {

// Global connection handle manager
// Manages connections opened via mssql_open for diagnostic purposes
class ConnectionHandleManager {
public:
	static ConnectionHandleManager& Instance();

	// Add a connection and return its handle
	int64_t AddConnection(std::shared_ptr<tds::TdsConnection> conn);

	// Get a connection by handle
	std::shared_ptr<tds::TdsConnection> GetConnection(int64_t handle);

	// Remove and return a connection
	std::shared_ptr<tds::TdsConnection> RemoveConnection(int64_t handle);

	// Check if handle exists
	bool HasConnection(int64_t handle);

private:
	ConnectionHandleManager() : next_handle_(1) {}

	std::mutex mutex_;
	std::unordered_map<int64_t, std::shared_ptr<tds::TdsConnection>> connections_;
	int64_t next_handle_;
};

// Register all diagnostic functions with the extension loader
void RegisterMSSQLDiagnosticFunctions(ExtensionLoader &loader);

// Individual function implementations

// mssql_open(secret_name VARCHAR) -> BIGINT
// Opens a connection using credentials from the named mssql secret
// Returns connection handle for use with other diagnostic functions
void MssqlOpenFunction(DataChunk &args, ExpressionState &state, Vector &result);

// mssql_close(handle BIGINT) -> BOOLEAN
// Closes a connection and releases resources
// Idempotent: closing already-closed handle returns true
void MssqlCloseFunction(DataChunk &args, ExpressionState &state, Vector &result);

// mssql_ping(handle BIGINT) -> BOOLEAN
// Tests if a connection is alive by sending a minimal TDS packet
// Returns true if connection responds, false otherwise
void MssqlPingFunction(DataChunk &args, ExpressionState &state, Vector &result);

// mssql_pool_stats table function
// Returns statistics for connection pools associated with attached databases
// If context_name is provided, returns stats for that specific pool
// If context_name is not provided, returns stats for all pools
struct MssqlPoolStatsBindData : public FunctionData {
	std::string context_name;  // Empty string means all pools
	bool all_pools;            // True if no parameter provided

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<MssqlPoolStatsBindData>();
		result->context_name = context_name;
		result->all_pools = all_pools;
		return result;
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<MssqlPoolStatsBindData>();
		return context_name == other.context_name && all_pools == other.all_pools;
	}
};

struct MssqlPoolStatsGlobalState : public GlobalTableFunctionState {
	std::vector<std::string> pool_names;  // Pools to iterate over
	idx_t current_index = 0;              // Current position in pool_names
};

class MssqlPoolStatsFunction {
public:
	static TableFunction GetFunction();

private:
	static unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input,
	                                     vector<LogicalType> &return_types, vector<string> &names);
	static unique_ptr<GlobalTableFunctionState> InitGlobal(ClientContext &context, TableFunctionInitInput &input);
	static void Execute(ClientContext &context, TableFunctionInput &input, DataChunk &output);
};

}  // namespace duckdb
