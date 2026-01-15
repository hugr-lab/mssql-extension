#include "connection/mssql_diagnostic.hpp"
#include "connection/mssql_pool_manager.hpp"
#include "mssql_secret.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// ConnectionHandleManager
//===----------------------------------------------------------------------===//

ConnectionHandleManager& ConnectionHandleManager::Instance() {
	static ConnectionHandleManager instance;
	return instance;
}

int64_t ConnectionHandleManager::AddConnection(std::shared_ptr<tds::TdsConnection> conn) {
	std::lock_guard<std::mutex> lock(mutex_);
	int64_t handle = next_handle_++;
	connections_[handle] = std::move(conn);
	return handle;
}

std::shared_ptr<tds::TdsConnection> ConnectionHandleManager::GetConnection(int64_t handle) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = connections_.find(handle);
	if (it != connections_.end()) {
		return it->second;
	}
	return nullptr;
}

std::shared_ptr<tds::TdsConnection> ConnectionHandleManager::RemoveConnection(int64_t handle) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = connections_.find(handle);
	if (it != connections_.end()) {
		auto conn = std::move(it->second);
		connections_.erase(it);
		return conn;
	}
	return nullptr;
}

bool ConnectionHandleManager::HasConnection(int64_t handle) {
	std::lock_guard<std::mutex> lock(mutex_);
	return connections_.find(handle) != connections_.end();
}

//===----------------------------------------------------------------------===//
// mssql_open
//===----------------------------------------------------------------------===//

void MssqlOpenFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &secret_name_vector = args.data[0];

	UnaryExecutor::Execute<string_t, int64_t>(secret_name_vector, result, args.size(), [&](string_t secret_name) {
		// Get the client context from expression state
		auto &context = state.GetContext();

		// Look up the secret
		auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
		auto &secret_manager = SecretManager::Get(context);

		auto secret_entry = secret_manager.GetSecretByName(transaction, secret_name.GetString());
		if (!secret_entry) {
			throw InvalidInputException("Secret '%s' not found", secret_name.GetString());
		}

		auto &secret = *secret_entry->secret;
		if (secret.GetType() != "mssql") {
			throw InvalidInputException("Secret '%s' is not an MSSQL secret", secret_name.GetString());
		}

		// Extract connection parameters from secret
		auto &kv_secret = dynamic_cast<const KeyValueSecret &>(secret);

		auto host_val = kv_secret.TryGetValue(MSSQL_SECRET_HOST);
		auto port_val = kv_secret.TryGetValue(MSSQL_SECRET_PORT);
		auto database_val = kv_secret.TryGetValue(MSSQL_SECRET_DATABASE);
		auto user_val = kv_secret.TryGetValue(MSSQL_SECRET_USER);
		auto password_val = kv_secret.TryGetValue(MSSQL_SECRET_PASSWORD);

		if (host_val.IsNull() || port_val.IsNull() || database_val.IsNull() ||
		    user_val.IsNull() || password_val.IsNull()) {
			throw InvalidInputException("Secret '%s' is missing required fields", secret_name.GetString());
		}

		std::string host = host_val.ToString();
		uint16_t port = static_cast<uint16_t>(port_val.GetValue<int64_t>());
		std::string database = database_val.ToString();
		std::string user = user_val.ToString();
		std::string password = password_val.ToString();

		// Create and connect
		auto conn = std::make_shared<tds::TdsConnection>();

		if (!conn->Connect(host, port)) {
			throw IOException("Failed to connect to %s:%d: %s", host, port, conn->GetLastError());
		}

		if (!conn->Authenticate(user, password, database)) {
			throw InvalidInputException("Login failed: %s", conn->GetLastError());
		}

		// Add to handle manager and return handle
		return ConnectionHandleManager::Instance().AddConnection(std::move(conn));
	});
}

//===----------------------------------------------------------------------===//
// mssql_close
//===----------------------------------------------------------------------===//

void MssqlCloseFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &handle_vector = args.data[0];

	UnaryExecutor::Execute<int64_t, bool>(handle_vector, result, args.size(), [&](int64_t handle) {
		auto conn = ConnectionHandleManager::Instance().RemoveConnection(handle);
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

void MssqlPingFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &handle_vector = args.data[0];

	UnaryExecutor::Execute<int64_t, bool>(handle_vector, result, args.size(), [&](int64_t handle) {
		auto conn = ConnectionHandleManager::Instance().GetConnection(handle);
		if (!conn) {
			throw InvalidInputException("Invalid connection handle: %lld", handle);
		}

		return conn->Ping();
	});
}

//===----------------------------------------------------------------------===//
// mssql_pool_stats table function
//===----------------------------------------------------------------------===//

TableFunction MssqlPoolStatsFunction::GetFunction() {
	// Create function with optional VARCHAR parameter
	TableFunction func("mssql_pool_stats", {}, Execute, Bind, InitGlobal);
	// Add overload with optional context_name parameter
	func.named_parameters["context_name"] = LogicalType::VARCHAR;
	return func;
}

unique_ptr<FunctionData> MssqlPoolStatsFunction::Bind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<MssqlPoolStatsBindData>();

	// Check if context_name parameter was provided
	auto it = input.named_parameters.find("context_name");
	if (it != input.named_parameters.end() && !it->second.IsNull()) {
		bind_data->context_name = it->second.GetValue<string>();
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

	return bind_data;
}

unique_ptr<GlobalTableFunctionState> MssqlPoolStatsFunction::InitGlobal(ClientContext &context,
                                                                         TableFunctionInitInput &input) {
	auto gstate = make_uniq<MssqlPoolStatsGlobalState>();
	auto &bind_data = input.bind_data->Cast<MssqlPoolStatsBindData>();

	if (bind_data.all_pools) {
		// Get all pool names
		gstate->pool_names = MssqlPoolManager::Instance().GetAllPoolNames();
	} else {
		// Single pool
		if (MssqlPoolManager::Instance().HasPool(bind_data.context_name)) {
			gstate->pool_names.push_back(bind_data.context_name);
		}
		// If pool doesn't exist, pool_names remains empty (no rows returned)
	}

	return gstate;
}

void MssqlPoolStatsFunction::Execute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &gstate = input.global_state->Cast<MssqlPoolStatsGlobalState>();

	if (gstate.current_index >= gstate.pool_names.size()) {
		output.SetCardinality(0);
		return;
	}

	// Calculate how many rows we can output in this batch
	idx_t count = 0;
	idx_t max_count = STANDARD_VECTOR_SIZE;

	while (gstate.current_index < gstate.pool_names.size() && count < max_count) {
		const auto& pool_name = gstate.pool_names[gstate.current_index];
		auto stats = MssqlPoolManager::Instance().GetPoolStats(pool_name);

		output.data[0].SetValue(count, Value(pool_name));  // db
		output.data[1].SetValue(count, Value::BIGINT(static_cast<int64_t>(stats.total_connections)));
		output.data[2].SetValue(count, Value::BIGINT(static_cast<int64_t>(stats.idle_connections)));
		output.data[3].SetValue(count, Value::BIGINT(static_cast<int64_t>(stats.active_connections)));
		output.data[4].SetValue(count, Value::BIGINT(static_cast<int64_t>(stats.connections_created)));
		output.data[5].SetValue(count, Value::BIGINT(static_cast<int64_t>(stats.connections_closed)));
		output.data[6].SetValue(count, Value::BIGINT(static_cast<int64_t>(stats.acquire_count)));
		output.data[7].SetValue(count, Value::BIGINT(static_cast<int64_t>(stats.acquire_timeout_count)));

		count++;
		gstate.current_index++;
	}

	output.SetCardinality(count);
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

void RegisterMSSQLDiagnosticFunctions(ExtensionLoader &loader) {
	// mssql_open(secret_name VARCHAR) -> BIGINT
	ScalarFunctionSet open_func("mssql_open");
	open_func.AddFunction(ScalarFunction({LogicalType::VARCHAR}, LogicalType::BIGINT, MssqlOpenFunction));
	loader.RegisterFunction(open_func);

	// mssql_close(handle BIGINT) -> BOOLEAN
	ScalarFunctionSet close_func("mssql_close");
	close_func.AddFunction(ScalarFunction({LogicalType::BIGINT}, LogicalType::BOOLEAN, MssqlCloseFunction));
	loader.RegisterFunction(close_func);

	// mssql_ping(handle BIGINT) -> BOOLEAN
	ScalarFunctionSet ping_func("mssql_ping");
	ping_func.AddFunction(ScalarFunction({LogicalType::BIGINT}, LogicalType::BOOLEAN, MssqlPingFunction));
	loader.RegisterFunction(ping_func);

	// mssql_pool_stats(context_name VARCHAR) -> TABLE
	loader.RegisterFunction(MssqlPoolStatsFunction::GetFunction());
}

}  // namespace duckdb
