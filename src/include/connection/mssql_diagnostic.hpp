#pragma once

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

// Register the diagnostic table function (mssql_pool_stats) with the extension loader
void RegisterMSSQLDiagnosticFunctions(ExtensionLoader &loader);

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
										 vector<LogicalType> &return_types, vector<Identifier> &names);
	static unique_ptr<GlobalTableFunctionState> InitGlobal(ClientContext &context, TableFunctionInitInput &input);
	static void Execute(ClientContext &context, TableFunctionInput &input, DataChunk &output);
};

}  // namespace duckdb
