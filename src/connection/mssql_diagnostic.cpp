#include "connection/mssql_diagnostic.hpp"
#include "catalog/mssql_catalog.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// mssql_pool_stats table function
//===----------------------------------------------------------------------===//

TableFunctionSet MSSQLPoolStatsFunction::GetFunctionSet() {
	TableFunctionSet set("mssql_pool_stats");

	// Overload 1: no arguments (all pools)
	TableFunction no_args("mssql_pool_stats", {}, Execute, Bind, InitGlobal);
	set.AddFunction(no_args);

	// Overload 2: positional VARCHAR argument: mssql_pool_stats('db')
	TableFunction with_arg("mssql_pool_stats", {LogicalType::VARCHAR}, Execute, Bind, InitGlobal);
	set.AddFunction(with_arg);

	return set;
}

unique_ptr<FunctionData> MSSQLPoolStatsFunction::Bind(ClientContext &context, TableFunctionBindInput &input,
													  vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto bind_data = make_uniq<MSSQLPoolStatsBindData>();

	// Check for positional argument
	if (!input.inputs.empty() && !input.inputs[0].IsNull()) {
		bind_data->context_name = input.inputs[0].GetValue<string>();
		bind_data->all_pools = false;
	} else {
		bind_data->context_name = "";
		bind_data->all_pools = true;
	}

	// Define output columns - db first, then stats
	names.emplace_back("db");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("total_connections");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("idle_connections");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("active_connections");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("connections_created");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("connections_closed");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("acquire_count");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("acquire_timeout_count");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("pinned_count");
	return_types.emplace_back(LogicalType::BIGINT);

	return std::move(bind_data);
}

unique_ptr<GlobalTableFunctionState> MSSQLPoolStatsFunction::InitGlobal(ClientContext &context,
																		TableFunctionInitInput &input) {
	// Spec 047 T019: enumerate via DuckDB catalog list instead of the
	// (deleted) MssqlPoolManager singleton. Per-catalog pool ownership means
	// the authoritative list of MSSQL pools IS the list of attached MSSQL
	// catalogs in this DuckDB instance.
	auto gstate = make_uniq<MssqlPoolStatsGlobalState>();
	auto &bind_data = input.bind_data->Cast<MSSQLPoolStatsBindData>();

	if (bind_data.all_pools) {
		auto &db_manager = DatabaseManager::Get(context);
		auto attached_dbs = db_manager.GetDatabases(context);
		for (auto &db : attached_dbs) {
			if (!db) {
				continue;
			}
			auto &catalog = db->GetCatalog();
			if (catalog.GetCatalogType() == "mssql") {
				gstate->pool_names.push_back(db->GetName().GetIdentifierName());
			}
		}
	} else {
		// Single catalog lookup
		try {
			auto &catalog = Catalog::GetCatalog(context, Identifier(bind_data.context_name));
			if (catalog.GetCatalogType() == "mssql") {
				gstate->pool_names.push_back(bind_data.context_name);
			}
		} catch (...) {
			// Not attached / not an MSSQL catalog — empty result
		}
	}

	return std::move(gstate);
}

void MSSQLPoolStatsFunction::Execute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &gstate = input.global_state->Cast<MssqlPoolStatsGlobalState>();

	if (gstate.current_index >= gstate.pool_names.size()) {
		output.SetChildCardinality(0);
		return;
	}

	idx_t count = 0;
	idx_t max_count = STANDARD_VECTOR_SIZE;

	while (gstate.current_index < gstate.pool_names.size() && count < max_count) {
		const auto &pool_name = gstate.pool_names[gstate.current_index];
		try {
			auto &catalog = Catalog::GetCatalog(context, Identifier(pool_name));
			auto &mssql_catalog = catalog.Cast<MSSQLCatalog>();
			auto &pool = mssql_catalog.GetConnectionPool();
			auto stats = pool.GetStats();

			output.data[0].SetValue(count, Value(pool_name));  // db
			output.data[1].SetValue(count, Value::BIGINT(static_cast<int64_t>(stats.total_connections)));
			output.data[2].SetValue(count, Value::BIGINT(static_cast<int64_t>(stats.idle_connections)));
			output.data[3].SetValue(count, Value::BIGINT(static_cast<int64_t>(stats.active_connections)));
			output.data[4].SetValue(count, Value::BIGINT(static_cast<int64_t>(stats.connections_created)));
			output.data[5].SetValue(count, Value::BIGINT(static_cast<int64_t>(stats.connections_closed)));
			output.data[6].SetValue(count, Value::BIGINT(static_cast<int64_t>(stats.acquire_count)));
			output.data[7].SetValue(count, Value::BIGINT(static_cast<int64_t>(stats.acquire_timeout_count)));
			output.data[8].SetValue(count, Value::BIGINT(stats.pinned_count));

			count++;
		} catch (...) {
			// Catalog detached between InitGlobal and Execute — skip silently.
		}
		gstate.current_index++;
	}

	output.SetChildCardinality(count);
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

void RegisterMSSQLDiagnosticFunctions(ExtensionLoader &loader) {
	// mssql_pool_stats([context_name] VARCHAR) -> TABLE
	loader.RegisterFunction(MSSQLPoolStatsFunction::GetFunctionSet());
}

}  // namespace duckdb
