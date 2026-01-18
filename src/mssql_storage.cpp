#include "mssql_storage.hpp"
#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_transaction.hpp"
#include "connection/mssql_pool_manager.hpp"
#include "connection/mssql_settings.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLConnectionInfo implementation
//===----------------------------------------------------------------------===//

shared_ptr<MSSQLConnectionInfo> MSSQLConnectionInfo::FromSecret(ClientContext &context, const string &secret_name) {
	auto &secret_manager = SecretManager::Get(context);

	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto secret_entry = secret_manager.GetSecretByName(transaction, secret_name);
	if (!secret_entry) {
		throw BinderException("MSSQL Error: Secret '%s' not found. Create it first with: CREATE SECRET %s (TYPE "
		                      "mssql, host '...', port ..., database '...', user '...', password '...')",
		                      secret_name, secret_name);
	}

	auto &secret = secret_entry->secret;
	if (secret->GetType() != "mssql") {
		throw BinderException("MSSQL Error: Secret '%s' is not of type 'mssql'. Got type: '%s'", secret_name,
		                      secret->GetType());
	}

	auto &kv_secret = dynamic_cast<const KeyValueSecret &>(*secret);

	auto result = make_shared_ptr<MSSQLConnectionInfo>();
	result->host = kv_secret.TryGetValue("host").ToString();
	result->port = static_cast<uint16_t>(kv_secret.TryGetValue("port").GetValue<int32_t>());
	result->database = kv_secret.TryGetValue("database").ToString();
	result->user = kv_secret.TryGetValue("user").ToString();
	result->password = kv_secret.TryGetValue("password").ToString();

	// Read optional use_encrypt (defaults to false)
	// Enables TLS encryption for the connection
	auto use_encrypt_val = kv_secret.TryGetValue("use_encrypt");
	if (!use_encrypt_val.IsNull()) {
		result->use_encrypt = use_encrypt_val.GetValue<bool>();
	} else {
		result->use_encrypt = false;
	}

	result->connected = false;
	return result;
}

//===----------------------------------------------------------------------===//
// Connection String Parsing
//===----------------------------------------------------------------------===//

// Check if string is a URI format (mssql://...)
static bool IsUriFormatImpl(const string &str) {
	return StringUtil::StartsWith(StringUtil::Lower(str), "mssql://");
}

bool MSSQLConnectionInfo::IsUriFormat(const string &str) {
	return IsUriFormatImpl(str);
}

// Check if string is an ADO.NET connection string (contains key=value pairs)
static bool IsConnectionStringImpl(const string &str) {
	// Connection strings have format like "Server=...;Database=..."
	return str.find('=') != string::npos;
}

bool MSSQLConnectionInfo::IsConnectionString(const string &str) {
	return IsConnectionStringImpl(str);
}

// URL decode a string (handles %XX encoding)
static string UrlDecode(const string &str) {
	string result;
	result.reserve(str.size());
	for (size_t i = 0; i < str.size(); i++) {
		if (str[i] == '%' && i + 2 < str.size()) {
			int hex_val = 0;
			if (sscanf(str.substr(i + 1, 2).c_str(), "%x", &hex_val) == 1) {
				result += static_cast<char>(hex_val);
				i += 2;
				continue;
			}
		}
		result += str[i];
	}
	return result;
}

// Parse URI format: mssql://user:password@host:port/database?param=value
static case_insensitive_map_t<string> ParseUri(const string &uri) {
	case_insensitive_map_t<string> result;

	// Skip "mssql://"
	string rest = uri.substr(8);

	// Extract query parameters first (after ?)
	string query_string;
	auto query_pos = rest.find('?');
	if (query_pos != string::npos) {
		query_string = rest.substr(query_pos + 1);
		rest = rest.substr(0, query_pos);
	}

	// Extract user:password (before @)
	auto at_pos = rest.find('@');
	if (at_pos != string::npos) {
		string user_pass = rest.substr(0, at_pos);
		rest = rest.substr(at_pos + 1);

		auto colon_pos = user_pass.find(':');
		if (colon_pos != string::npos) {
			result["user"] = UrlDecode(user_pass.substr(0, colon_pos));
			result["password"] = UrlDecode(user_pass.substr(colon_pos + 1));
		} else {
			result["user"] = UrlDecode(user_pass);
		}
	}

	// Extract host:port/database
	auto slash_pos = rest.find('/');
	string host_port;
	if (slash_pos != string::npos) {
		host_port = rest.substr(0, slash_pos);
		result["database"] = UrlDecode(rest.substr(slash_pos + 1));
	} else {
		host_port = rest;
	}

	// Parse host:port
	auto colon_pos = host_port.rfind(':');
	if (colon_pos != string::npos) {
		result["server"] = host_port.substr(0, colon_pos) + "," + host_port.substr(colon_pos + 1);
	} else {
		result["server"] = host_port;
	}

	// Parse query parameters
	if (!query_string.empty()) {
		auto params = StringUtil::Split(query_string, '&');
		for (auto &param : params) {
			auto eq_pos = param.find('=');
			if (eq_pos != string::npos) {
				string key = UrlDecode(param.substr(0, eq_pos));
				string value = UrlDecode(param.substr(eq_pos + 1));
				auto lower_key = StringUtil::Lower(key);
				if (lower_key == "encrypt" || lower_key == "ssl" || lower_key == "use_ssl") {
					result["encrypt"] = value;
				} else {
					result[key] = value;
				}
			}
		}
	}

	return result;
}

// Parse key=value pairs from connection string
// Format: "Server=host,port;Database=db;User Id=user;Password=pass;Encrypt=yes/no"
static case_insensitive_map_t<string> ParseConnectionString(const string &connection_string) {
	case_insensitive_map_t<string> result;

	// Split by semicolon
	auto parts = StringUtil::Split(connection_string, ';');
	for (auto &part : parts) {
		// Trim whitespace (Trim modifies in place)
		string trimmed = part;
		StringUtil::Trim(trimmed);
		if (trimmed.empty()) {
			continue;
		}

		// Split by first '='
		auto eq_pos = trimmed.find('=');
		if (eq_pos == string::npos) {
			continue;  // Skip invalid parts
		}

		string key = trimmed.substr(0, eq_pos);
		string value = trimmed.substr(eq_pos + 1);
		StringUtil::Trim(key);
		StringUtil::Trim(value);

		// Normalize key names
		auto lower_key = StringUtil::Lower(key);
		if (lower_key == "server" || lower_key == "data source") {
			result["server"] = value;
		} else if (lower_key == "database" || lower_key == "initial catalog") {
			result["database"] = value;
		} else if (lower_key == "user id" || lower_key == "uid" || lower_key == "user") {
			result["user"] = value;
		} else if (lower_key == "password" || lower_key == "pwd") {
			result["password"] = value;
		} else if (lower_key == "encrypt" || lower_key == "use encryption for data") {
			result["encrypt"] = value;
		} else {
			result[key] = value;
		}
	}

	return result;
}

string MSSQLConnectionInfo::ValidateConnectionString(const string &connection_string) {
	if (connection_string.empty()) {
		return "Connection string cannot be empty.";
	}

	// Parse based on format
	case_insensitive_map_t<string> params;
	if (IsUriFormatImpl(connection_string)) {
		params = ParseUri(connection_string);
	} else {
		params = ParseConnectionString(connection_string);
	}

	// Check required fields
	if (params.find("server") == params.end()) {
		return "Missing 'Server' in connection string. Format: Server=host,port;Database=...;User Id=...;Password=...";
	}
	if (params.find("database") == params.end()) {
		return "Missing 'Database' in connection string.";
	}
	if (params.find("user") == params.end()) {
		return "Missing 'User Id' in connection string.";
	}
	if (params.find("password") == params.end()) {
		return "Missing 'Password' in connection string.";
	}

	// Validate server format (host or host,port)
	auto server = params["server"];
	auto comma_pos = server.find(',');
	if (comma_pos != string::npos) {
		auto port_str = server.substr(comma_pos + 1);
		try {
			int port = std::stoi(port_str);
			if (port < 1 || port > 65535) {
				return StringUtil::Format("Port must be between 1 and 65535. Got: %d", port);
			}
		} catch (...) {
			return StringUtil::Format("Invalid port in Server parameter: '%s'", port_str);
		}
	}

	return "";  // Valid
}

shared_ptr<MSSQLConnectionInfo> MSSQLConnectionInfo::FromConnectionString(const string &connection_string) {
	// Validate first
	string error = ValidateConnectionString(connection_string);
	if (!error.empty()) {
		throw InvalidInputException("MSSQL Error: %s", error);
	}

	// Parse based on format
	case_insensitive_map_t<string> params;
	if (IsUriFormatImpl(connection_string)) {
		params = ParseUri(connection_string);
	} else {
		params = ParseConnectionString(connection_string);
	}

	auto result = make_shared_ptr<MSSQLConnectionInfo>();

	// Parse server (host,port or just host)
	auto server = params["server"];
	auto comma_pos = server.find(',');
	if (comma_pos != string::npos) {
		result->host = server.substr(0, comma_pos);
		result->port = static_cast<uint16_t>(std::stoi(server.substr(comma_pos + 1)));
	} else {
		result->host = server;
		result->port = 1433;  // Default MSSQL port
	}

	result->database = params["database"];
	result->user = params["user"];
	result->password = params["password"];

	// Parse optional encrypt parameter
	// Enables TLS encryption for the connection
	if (params.find("encrypt") != params.end()) {
		auto encrypt_val = StringUtil::Lower(params["encrypt"]);
		result->use_encrypt = (encrypt_val == "yes" || encrypt_val == "true" || encrypt_val == "1");
	} else {
		result->use_encrypt = false;
	}

	result->connected = false;
	return result;
}

//===----------------------------------------------------------------------===//
// MSSQLContext implementation
//===----------------------------------------------------------------------===//

MSSQLContext::MSSQLContext(const string &name, const string &secret_name) : name(name), secret_name(secret_name) {
}

//===----------------------------------------------------------------------===//
// MSSQLContextManager implementation
//===----------------------------------------------------------------------===//

// Static storage for context managers - keyed by DatabaseInstance pointer
static case_insensitive_map_t<unique_ptr<MSSQLContextManager>> g_context_managers;
static mutex g_context_managers_lock;

MSSQLContextManager &MSSQLContextManager::Get(DatabaseInstance &db) {
	lock_guard<mutex> guard(g_context_managers_lock);

	// Use pointer address as unique key - cast to size_t for formatting
	string db_key = StringUtil::Format("%llu", (unsigned long long)(uintptr_t)&db);

	auto it = g_context_managers.find(db_key);
	if (it == g_context_managers.end()) {
		auto manager = make_uniq<MSSQLContextManager>();
		auto &manager_ref = *manager;
		g_context_managers[db_key] = std::move(manager);
		return manager_ref;
	}
	return *it->second;
}

void MSSQLContextManager::RegisterContext(const string &name, shared_ptr<MSSQLContext> ctx) {
	lock_guard<mutex> guard(lock);
	if (contexts.find(name) != contexts.end()) {
		throw CatalogException("MSSQL Error: Context '%s' already exists. Use a different name or DETACH first.", name);
	}
	contexts[name] = std::move(ctx);
}

void MSSQLContextManager::UnregisterContext(const string &name) {
	lock_guard<mutex> guard(lock);
	auto it = contexts.find(name);
	if (it != contexts.end()) {
		// Clean up: abort any in-progress queries, close connection
		// For stub implementation, just remove from map
		contexts.erase(it);
	}
}

shared_ptr<MSSQLContext> MSSQLContextManager::GetContext(const string &name) {
	lock_guard<mutex> guard(lock);
	auto it = contexts.find(name);
	if (it == contexts.end()) {
		return nullptr;
	}
	return it->second;
}

bool MSSQLContextManager::HasContext(const string &name) {
	lock_guard<mutex> guard(lock);
	return contexts.find(name) != contexts.end();
}

vector<string> MSSQLContextManager::ListContexts() {
	lock_guard<mutex> guard(lock);
	vector<string> result;
	for (auto &entry : contexts) {
		result.push_back(entry.first);
	}
	return result;
}

//===----------------------------------------------------------------------===//
// Storage Extension callbacks
//===----------------------------------------------------------------------===//

unique_ptr<Catalog> MSSQLAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                AttachedDatabase &db, const string &name, AttachInfo &info, AttachOptions &options) {
	// Extract SECRET parameter (optional if connection string is provided)
	// Remove it from options so DuckDB's StorageOptions doesn't reject it as unrecognized
	string secret_name;
	for (auto it = options.options.begin(); it != options.options.end();) {
		auto lower_name = StringUtil::Lower(it->first);
		if (lower_name == "secret") {
			secret_name = it->second.ToString();
			it = options.options.erase(it);
		} else {
			++it;
		}
	}

	// Get connection string from info.path (the first argument to ATTACH)
	string connection_string = info.path;

	// Create context based on whether SECRET or connection string is provided
	auto ctx = make_shared_ptr<MSSQLContext>(name, secret_name);
	ctx->attached_db = &db;

	if (!secret_name.empty()) {
		// SECRET provided - use secret-based connection
		ctx->connection_info = MSSQLConnectionInfo::FromSecret(context, secret_name);
	} else if (!connection_string.empty()) {
		// Connection string provided - parse it
		ctx->connection_info = MSSQLConnectionInfo::FromConnectionString(connection_string);
	} else {
		// Neither SECRET nor connection string provided
		throw InvalidInputException(
		    "MSSQL Error: Either SECRET or connection string is required for ATTACH.\n"
		    "With secret: ATTACH '' AS %s (TYPE mssql, SECRET <secret_name>)\n"
		    "With connection string: ATTACH 'Server=host;Database=db;User Id=user;Password=pass' AS %s (TYPE mssql)",
		    name, name);
	}

	// Register context
	auto &manager = MSSQLContextManager::Get(*context.db);
	manager.RegisterContext(name, ctx);

	// Create connection pool for this context using current settings
	auto pool_config = LoadPoolConfig(context);
	MssqlPoolManager::Instance().GetOrCreatePool(
	    name,
	    pool_config,
	    ctx->connection_info->host,
	    ctx->connection_info->port,
	    ctx->connection_info->user,
	    ctx->connection_info->password,
	    ctx->connection_info->database,
	    ctx->connection_info->use_encrypt
	);

	// Create MSSQLCatalog with connection info and access mode from options
	// The catalog will use the connection pool to query SQL Server
	// options.access_mode is set by DuckDB based on the READ_ONLY option in ATTACH
	auto catalog = make_uniq<MSSQLCatalog>(db, name, ctx->connection_info, options.access_mode);
	catalog->Initialize(false);

	return std::move(catalog);
}

unique_ptr<TransactionManager> MSSQLCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                                             AttachedDatabase &db, Catalog &catalog) {
	// Use custom transaction manager for external MSSQL catalog
	auto &mssql_catalog = catalog.Cast<MSSQLCatalog>();
	return make_uniq<MSSQLTransactionManager>(db, mssql_catalog);
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

void RegisterMSSQLStorageExtension(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);

	auto storage_ext = make_uniq<StorageExtension>();
	storage_ext->attach = MSSQLAttach;
	storage_ext->create_transaction_manager = MSSQLCreateTransactionManager;

	config.storage_extensions["mssql"] = std::move(storage_ext);
}

}  // namespace duckdb
