#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "tds/tds_connection.hpp"

namespace duckdb {

// Global connection handle manager.
//
// Spec 047 design note (FR-010, T045):
//   This singleton is RETAINED on purpose. The diagnostic API trio —
//   mssql_open / mssql_close / mssql_ping — plus the mssql_close_all
//   shutdown helper (FR-013) take a connection string (not a catalog
//   name) by design, so there is no natural per-catalog owner to hold
//   the handle map. Unlike the three removed singletons (MssqlPoolManager,
//   MSSQLContextManager, MSSQLResultStreamRegistry) which had a clear
//   catalog discriminator and were folded into MSSQLCatalog, the
//   diagnostic handles are inherently process-scoped.
//
//   The four functions above are all marked [DEPRECATED] (FR-010 group);
//   when they are removed in a future major release, this singleton goes
//   with them — that is the planned cleanup for the last extension-
//   internal process-wide state. The mssql_close_all() function (FR-013,
//   added in spec 047) gives hosts a shutdown path so they can release
//   sockets cleanly before that removal.
//
//   See `state_inventory.md` in specs/047-process-state-cleanup/ for the
//   final classification of every process-wide static after spec 047:
//   zero "migrate" entries, this manager + OS/library scratch are the
//   legitimate remainder.
//
// Manages connections opened via mssql_open for diagnostic purposes.
class MSSQLConnectionHandleManager {
public:
	static MSSQLConnectionHandleManager &Instance();

	// Add a connection and return its handle
	int64_t AddConnection(std::shared_ptr<tds::TdsConnection> conn);

	// Get a connection by handle
	std::shared_ptr<tds::TdsConnection> GetConnection(int64_t handle);

	// Remove and return a connection
	std::shared_ptr<tds::TdsConnection> RemoveConnection(int64_t handle);

	// Check if handle exists
	bool HasConnection(int64_t handle);

	// Spec 047 FR-013: close every open handle in one shot. Returns the count
	// of connections closed. Idempotent — a second call after closing returns 0.
	// Used by hosts that want a clean shutdown path before tearing down the
	// process: `SELECT mssql_close_all()` releases every diagnostic-API socket
	// without forcing the user to track handles manually.
	int64_t CloseAll();

private:
	MSSQLConnectionHandleManager() : next_handle_(1) {}

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
void MSSQLOpenFunction(DataChunk &args, ExpressionState &state, Vector &result);

// mssql_close(handle BIGINT) -> BOOLEAN
// Closes a connection and releases resources
// Idempotent: closing already-closed handle returns true
void MSSQLCloseFunction(DataChunk &args, ExpressionState &state, Vector &result);

// mssql_ping(handle BIGINT) -> BOOLEAN
// Tests if a connection is alive by sending a minimal TDS packet
// Returns true if connection responds, false otherwise
void MSSQLPingFunction(DataChunk &args, ExpressionState &state, Vector &result);

// mssql_close_all() -> INTEGER
// [DEPRECATED] Closes every diagnostic connection opened via mssql_open and
// returns the count of closed handles. Idempotent. Companion to mssql_open /
// mssql_close / mssql_ping, all of which share the singleton
// MSSQLConnectionHandleManager and will be removed together in a future major
// release (see FR-010/FR-013, spec 047).
void MSSQLCloseAllFunction(DataChunk &args, ExpressionState &state, Vector &result);

// mssql_pool_stats table function
// Returns statistics for connection pools associated with attached databases
// If context_name is provided, returns stats for that specific pool
// If context_name is not provided, returns stats for all pools
struct MSSQLPoolStatsBindData : public FunctionData {
	std::string context_name;  // Empty string means all pools
	bool all_pools;			   // True if no parameter provided

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<MSSQLPoolStatsBindData>();
		result->context_name = context_name;
		result->all_pools = all_pools;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<MSSQLPoolStatsBindData>();
		return context_name == other.context_name && all_pools == other.all_pools;
	}
};

struct MssqlPoolStatsGlobalState : public GlobalTableFunctionState {
	std::vector<std::string> pool_names;  // Pools to iterate over
	idx_t current_index = 0;			  // Current position in pool_names
};

class MSSQLPoolStatsFunction {
public:
	static TableFunctionSet GetFunctionSet();

private:
	static unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input,
										 vector<LogicalType> &return_types, vector<string> &names);
	static unique_ptr<GlobalTableFunctionState> InitGlobal(ClientContext &context, TableFunctionInitInput &input);
	static void Execute(ClientContext &context, TableFunctionInput &input, DataChunk &output);
};

}  // namespace duckdb
