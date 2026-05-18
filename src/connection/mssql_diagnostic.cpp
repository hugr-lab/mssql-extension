#include "connection/mssql_diagnostic.hpp"
#include "catalog/mssql_catalog.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "mssql_secret.hpp"
#include "mssql_storage.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// ConnectionHandleManager
//===----------------------------------------------------------------------===//

MSSQLConnectionHandleManager &MSSQLConnectionHandleManager::Instance() {
	static MSSQLConnectionHandleManager instance;
	return instance;
}

int64_t MSSQLConnectionHandleManager::AddConnection(std::shared_ptr<tds::TdsConnection> conn) {
	std::lock_guard<std::mutex> lock(mutex_);
	int64_t handle = next_handle_++;
	connections_[handle] = std::move(conn);
	return handle;
}

std::shared_ptr<tds::TdsConnection> MSSQLConnectionHandleManager::GetConnection(int64_t handle) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = connections_.find(handle);
	if (it != connections_.end()) {
		return it->second;
	}
	return nullptr;
}

std::shared_ptr<tds::TdsConnection> MSSQLConnectionHandleManager::RemoveConnection(int64_t handle) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = connections_.find(handle);
	if (it != connections_.end()) {
		auto conn = std::move(it->second);
		connections_.erase(it);
		return conn;
	}
	return nullptr;
}

bool MSSQLConnectionHandleManager::HasConnection(int64_t handle) {
	std::lock_guard<std::mutex> lock(mutex_);
	return connections_.find(handle) != connections_.end();
}

int64_t MSSQLConnectionHandleManager::CloseAll() {
	std::lock_guard<std::mutex> lock(mutex_);
	int64_t closed = 0;
	for (auto &entry : connections_) {
		if (!entry.second) {
			continue;
		}
		try {
			entry.second->Close();
		} catch (...) {
			// Swallow — best-effort shutdown; counting only sockets we actually owned.
		}
		++closed;
	}
	connections_.clear();
	return closed;
}

//===----------------------------------------------------------------------===//
// mssql_open
//===----------------------------------------------------------------------===//

void MSSQLOpenFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &input_vector = args.data[0];

	UnaryExecutor::Execute<string_t, int64_t>(input_vector, result, args.size(), [&](string_t input_str) {
		std::string input = input_str.GetString();
		std::string host;
		uint16_t port;
		std::string database;
		std::string user;
		std::string password;

		// Check if input is a connection string (URI or ADO.NET format)
		if (MSSQLConnectionInfo::IsUriFormat(input) || MSSQLConnectionInfo::IsConnectionString(input)) {
			// Parse connection string directly
			auto conn_info = MSSQLConnectionInfo::FromConnectionString(input);
			host = conn_info->host;
			port = conn_info->port;
			database = conn_info->database;
			user = conn_info->user;
			password = conn_info->password;
		} else {
			// Treat as secret name (backward compatibility)
			auto &context = state.GetContext();
			auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
			auto &secret_manager = SecretManager::Get(context);

			auto secret_entry = secret_manager.GetSecretByName(transaction, input);
			if (!secret_entry) {
				throw InvalidInputException(
					"Secret '%s' not found. You can also pass a connection string:\n"
					"  mssql://user:password@host:port/database\n"
					"  Server=host,port;Database=db;User Id=user;Password=pass",
					input);
			}

			auto &secret = *secret_entry->secret;
			if (secret.GetType() != "mssql") {
				throw InvalidInputException("Secret '%s' is not an MSSQL secret", input);
			}

			auto &kv_secret = dynamic_cast<const KeyValueSecret &>(secret);

			auto host_val = kv_secret.TryGetValue(MSSQL_SECRET_HOST);
			auto port_val = kv_secret.TryGetValue(MSSQL_SECRET_PORT);
			auto database_val = kv_secret.TryGetValue(MSSQL_SECRET_DATABASE);
			auto user_val = kv_secret.TryGetValue(MSSQL_SECRET_USER);
			auto password_val = kv_secret.TryGetValue(MSSQL_SECRET_PASSWORD);

			if (host_val.IsNull() || database_val.IsNull() || user_val.IsNull() || password_val.IsNull()) {
				throw InvalidInputException("Secret '%s' is missing required fields", input);
			}

			host = host_val.ToString();
			port = port_val.IsNull() ? 1433 : static_cast<uint16_t>(port_val.GetValue<int64_t>());
			database = database_val.ToString();
			user = user_val.ToString();
			password = password_val.ToString();
		}

		// Create and connect
		auto conn = std::make_shared<tds::TdsConnection>();

		if (!conn->Connect(host, port)) {
			throw IOException("Failed to connect to %s:%d: %s", host, port, conn->GetLastError());
		}

		if (!conn->Authenticate(user, password, database)) {
			throw InvalidInputException("Login failed: %s", conn->GetLastError());
		}

		// Add to handle manager and return handle
		return MSSQLConnectionHandleManager::Instance().AddConnection(std::move(conn));
	});
}

//===----------------------------------------------------------------------===//
// mssql_close
//===----------------------------------------------------------------------===//

void MSSQLCloseFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &handle_vector = args.data[0];

	UnaryExecutor::Execute<int64_t, bool>(handle_vector, result, args.size(), [&](int64_t handle) {
		auto conn = MSSQLConnectionHandleManager::Instance().RemoveConnection(handle);
		if (conn) {
			conn->Close();
		}
		// Idempotent: always return true
		return true;
	});
}

//===----------------------------------------------------------------------===//
// mssql_ping
//===----------------------------------------------------------------------===//

void MSSQLPingFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &handle_vector = args.data[0];

	UnaryExecutor::Execute<int64_t, bool>(handle_vector, result, args.size(), [&](int64_t handle) {
		auto conn = MSSQLConnectionHandleManager::Instance().GetConnection(handle);
		if (!conn) {
			throw InvalidInputException("Invalid connection handle: %lld", handle);
		}

		return conn->Ping();
	});
}

//===----------------------------------------------------------------------===//
// mssql_close_all (spec 047 FR-013)
//===----------------------------------------------------------------------===//

// Closes every diagnostic handle in one shot. Computed once per call;
// broadcast as a constant across the result vector — `SELECT mssql_close_all()`
// returns one row, but a `SELECT mssql_close_all() FROM range(N)` still only
// triggers a single CloseAll().
void MSSQLCloseAllFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	int64_t closed = MSSQLConnectionHandleManager::Instance().CloseAll();
	auto closed_i32 = static_cast<int32_t>(closed);

	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	auto data = ConstantVector::GetData<int32_t>(result);
	data[0] = closed_i32;
}

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
													  vector<LogicalType> &return_types, vector<string> &names) {
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
				gstate->pool_names.push_back(db->GetName());
			}
		}
	} else {
		// Single catalog lookup
		try {
			auto &catalog = Catalog::GetCatalog(context, bind_data.context_name);
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
		output.SetCardinality(0);
		return;
	}

	idx_t count = 0;
	idx_t max_count = STANDARD_VECTOR_SIZE;

	while (gstate.current_index < gstate.pool_names.size() && count < max_count) {
		const auto &pool_name = gstate.pool_names[gstate.current_index];
		try {
			auto &catalog = Catalog::GetCatalog(context, pool_name);
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

	output.SetCardinality(count);
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

void RegisterMSSQLDiagnosticFunctions(ExtensionLoader &loader) {
	// mssql_open(connection_string VARCHAR) -> BIGINT
	// Accepts:
	//   - Connection string: "Server=host,port;Database=db;User Id=user;Password=pass"
	//   - URI format: "mssql://user:password@host:port/database"
	//   - Secret name (for backward compatibility)
	ScalarFunctionSet open_func("mssql_open");
	open_func.AddFunction(ScalarFunction({LogicalType::VARCHAR}, LogicalType::BIGINT, MSSQLOpenFunction));
	loader.RegisterFunction(open_func);

	// mssql_close(handle BIGINT) -> BOOLEAN
	ScalarFunctionSet close_func("mssql_close");
	close_func.AddFunction(ScalarFunction({LogicalType::BIGINT}, LogicalType::BOOLEAN, MSSQLCloseFunction));
	loader.RegisterFunction(close_func);

	// mssql_ping(handle BIGINT) -> BOOLEAN
	ScalarFunctionSet ping_func("mssql_ping");
	ping_func.AddFunction(ScalarFunction({LogicalType::BIGINT}, LogicalType::BOOLEAN, MSSQLPingFunction));
	loader.RegisterFunction(ping_func);

	// [DEPRECATED] mssql_close_all() -> INTEGER  (spec 047 FR-013)
	// Companion shutdown helper for the diagnostic handle API. Lives in the
	// same `[DEPRECATED]` group as mssql_open / mssql_close / mssql_ping
	// (FR-010) — the deprecation marker is in CLAUDE.md's Extension Functions
	// table; DuckDB's ScalarFunctionSet registration path does not surface a
	// per-function description string, so the marker stays in docs + this
	// comment until the diagnostic API is retired and the singleton
	// MSSQLConnectionHandleManager goes with it.
	ScalarFunctionSet close_all_func("mssql_close_all");
	close_all_func.AddFunction(ScalarFunction({}, LogicalType::INTEGER, MSSQLCloseAllFunction));
	loader.RegisterFunction(close_all_func);

	// mssql_pool_stats([context_name] VARCHAR) -> TABLE
	loader.RegisterFunction(MSSQLPoolStatsFunction::GetFunctionSet());
}

}  // namespace duckdb
