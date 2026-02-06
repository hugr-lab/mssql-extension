#include "mssql_storage.hpp"
#include "azure/azure_fedauth.hpp"
#include "azure/azure_token.hpp"
#include "tds/auth/auth_strategy_factory.hpp"
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
#include "mssql_platform.hpp"
#include "tds/tds_connection.hpp"

#include <cstdlib>

// Debug logging (same pattern as tds_socket.cpp)
static int GetMssqlStorageDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define MSSQL_STORAGE_DEBUG_LOG(lvl, fmt, ...)                           \
	do {                                                                 \
		if (GetMssqlStorageDebugLevel() >= lvl)                          \
			fprintf(stderr, "[MSSQL STORAGE] " fmt "\n", ##__VA_ARGS__); \
	} while (0)

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLConnectionInfo implementation
//===----------------------------------------------------------------------===//

shared_ptr<MSSQLConnectionInfo> MSSQLConnectionInfo::FromSecret(ClientContext &context, const string &secret_name) {
	auto &secret_manager = SecretManager::Get(context);

	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto secret_entry = secret_manager.GetSecretByName(transaction, secret_name);
	if (!secret_entry) {
		throw BinderException(
			"MSSQL Error: Secret '%s' not found. Create it first with: CREATE SECRET %s (TYPE "
			"mssql, host '...', port ..., database '...', user '...', password '...')",
			secret_name, secret_name);
	}

	auto &secret = secret_entry->secret;
	if (secret->GetType() != "mssql") {
		throw BinderException("MSSQL Error: Secret '%s' is not of type 'mssql'. Got type: '%s'", secret_name,
							  secret->GetType());
	}

	// Use static_cast - we've already verified it's an MSSQL secret which is always KeyValueSecret
	// This avoids dynamic_cast RTTI warnings when crossing extension boundaries on macOS
	auto &kv_secret = static_cast<const KeyValueSecret &>(*secret);

	auto result = make_shared_ptr<MSSQLConnectionInfo>();
	result->host = kv_secret.TryGetValue("host").ToString();
	result->port = static_cast<uint16_t>(kv_secret.TryGetValue("port").GetValue<int32_t>());
	result->database = kv_secret.TryGetValue("database").ToString();
	result->user = kv_secret.TryGetValue("user").ToString();
	result->password = kv_secret.TryGetValue("password").ToString();

	// Read optional use_encrypt (defaults to true for security)
	// Enables TLS encryption for the connection
	auto use_encrypt_val = kv_secret.TryGetValue("use_encrypt");
	if (!use_encrypt_val.IsNull()) {
		result->use_encrypt = use_encrypt_val.GetValue<bool>();
	}
	// Default is true (use_encrypt initialized to true in struct definition)

	// Read optional catalog (defaults to true)
	// When false, catalog integration is disabled (raw query mode only)
	auto catalog_val = kv_secret.TryGetValue("catalog");
	if (!catalog_val.IsNull()) {
		result->catalog_enabled = catalog_val.GetValue<bool>();
	}
	// Default is true (catalog_enabled initialized to true in struct definition)

	// Read optional access_token for direct token authentication (Spec 032)
	// Takes precedence over azure_secret
	auto access_token_val = kv_secret.TryGetValue("access_token");
	if (!access_token_val.IsNull()) {
		result->access_token = access_token_val.ToString();
		result->use_azure_auth = !result->access_token.empty();	 // Manual token uses FEDAUTH flow
	}

	// Read optional azure_secret for Azure AD authentication (T015)
	// Only applies if access_token is not set
	if (result->access_token.empty()) {
		auto azure_secret_val = kv_secret.TryGetValue("azure_secret");
		if (!azure_secret_val.IsNull()) {
			result->azure_secret_name = azure_secret_val.ToString();
			result->use_azure_auth = !result->azure_secret_name.empty();
		}
	}
	// Default: use_azure_auth = false (SQL auth)

	result->connected = false;
	return result;
}

//===----------------------------------------------------------------------===//
// Endpoint Detection Helpers (T007)
//===----------------------------------------------------------------------===//

bool MSSQLConnectionInfo::IsAzureEndpoint() const {
	return mssql::IsAzureEndpoint(host);
}

bool MSSQLConnectionInfo::IsFabricEndpoint() const {
	return mssql::IsFabricEndpoint(host);
}

bool MSSQLConnectionInfo::IsSynapseEndpoint() const {
	return mssql::IsSynapseEndpoint(host);
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
				} else if (lower_key == "trustservercertificate") {
					result["trustservercertificate"] = value;
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
		} else if (lower_key == "trustservercertificate") {
			result["trustservercertificate"] = value;
		} else {
			result[key] = value;
		}
	}

	return result;
}

string MSSQLConnectionInfo::ValidateConnectionString(const string &connection_string, bool azure_auth) {
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
	// User/password are only required for SQL authentication, not Azure AD
	if (!azure_auth) {
		if (params.find("user") == params.end()) {
			return "Missing 'User Id' in connection string.";
		}
		if (params.find("password") == params.end()) {
			return "Missing 'Password' in connection string.";
		}
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

	return "";	// Valid
}

shared_ptr<MSSQLConnectionInfo> MSSQLConnectionInfo::FromConnectionString(const string &connection_string,
																		  bool azure_auth) {
	// Validate first
	string error = ValidateConnectionString(connection_string, azure_auth);
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

	// Parse optional encrypt and trustservercertificate parameters
	// TrustServerCertificate is an alias for Encrypt (both enable TLS)
	// Default: TLS enabled for security (use_encrypt = true in struct definition)
	bool encrypt_specified = params.find("encrypt") != params.end();
	bool trust_cert_specified = params.find("trustservercertificate") != params.end();

	// Only override the default (true) if explicitly specified
	if (encrypt_specified || trust_cert_specified) {
		bool encrypt_value = true;	// Default when not specified
		bool trust_cert_value = true;

		if (encrypt_specified) {
			auto encrypt_val = StringUtil::Lower(params["encrypt"]);
			// "no" or "false" disables TLS; anything else enables it
			encrypt_value = !(encrypt_val == "no" || encrypt_val == "false" || encrypt_val == "0");
		}

		if (trust_cert_specified) {
			auto trust_val = StringUtil::Lower(params["trustservercertificate"]);
			trust_cert_value = !(trust_val == "no" || trust_val == "false" || trust_val == "0");
		}

		// Check for conflicting values
		if (encrypt_specified && trust_cert_specified && encrypt_value != trust_cert_value) {
			throw InvalidInputException(
				"MSSQL Error: Conflicting values for Encrypt (%s) and TrustServerCertificate (%s). "
				"These parameters must have the same value or only one should be specified.",
				encrypt_value ? "true" : "false", trust_cert_value ? "true" : "false");
		}

		// Apply: if any is specified, use their value (both must agree if both specified)
		result->use_encrypt = encrypt_specified ? encrypt_value : trust_cert_value;
	}
	// If neither specified, use_encrypt keeps its default value (true from struct definition)

	// Parse optional Catalog parameter (defaults to true)
	// When false, catalog integration is disabled (raw query mode only)
	bool catalog_specified = params.find("catalog") != params.end();
	if (catalog_specified) {
		auto catalog_val = StringUtil::Lower(params["catalog"]);
		result->catalog_enabled = (catalog_val == "yes" || catalog_val == "true" || catalog_val == "1");
	}
	// Default is true (catalog_enabled initialized to true in struct definition)

	result->connected = false;
	return result;
}

//===----------------------------------------------------------------------===//
// MSSQLContext implementation
//===----------------------------------------------------------------------===//

MSSQLContext::MSSQLContext(const string &name, const string &secret_name) : name(name), secret_name(secret_name) {}

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
// Connection Validation
//===----------------------------------------------------------------------===//

// Translate TDS error message to user-friendly message
static string TranslateConnectionError(const string &error, const string &host, uint16_t port, const string &user,
									   const string &database) {
	string lower_error = StringUtil::Lower(error);

	// Authentication failures
	if (lower_error.find("login failed") != string::npos || lower_error.find("authentication") != string::npos ||
		lower_error.find("18456") != string::npos) {
		return StringUtil::Format("Authentication failed for user '%s' - check username and password", user);
	}

	// Database access failures
	if (lower_error.find("cannot open database") != string::npos || lower_error.find("4060") != string::npos) {
		return StringUtil::Format("Cannot access database '%s' - check database name and permissions", database);
	}

	// TLS failures
	if (lower_error.find("tls") != string::npos || lower_error.find("ssl") != string::npos ||
		lower_error.find("handshake") != string::npos) {
		return StringUtil::Format("TLS handshake failed to %s:%d - check TLS configuration", host, port);
	}

	// Server requires encryption but client disabled it
	if (lower_error.find("encrypt_req") != string::npos ||
		(lower_error.find("encryption") != string::npos && lower_error.find("require") != string::npos)) {
		return StringUtil::Format(
			"Server requires encryption (ENCRYPT_REQ) but use_encrypt=false. "
			"Set use_encrypt=true or Encrypt=yes in connection string.");
	}

	// Certificate validation failures
	if (lower_error.find("certificate") != string::npos || lower_error.find("cert") != string::npos) {
		return StringUtil::Format("TLS certificate validation failed - server certificate not trusted");
	}

	// Connection refused
	if (lower_error.find("connection refused") != string::npos || lower_error.find("econnrefused") != string::npos) {
		return StringUtil::Format(
			"Connection refused to %s:%d - check if SQL Server is running and accepting "
			"connections",
			host, port);
	}

	// DNS/hostname resolution
	if (lower_error.find("resolve") != string::npos || lower_error.find("host") != string::npos ||
		lower_error.find("enoent") != string::npos || lower_error.find("name or service not known") != string::npos) {
		return StringUtil::Format("Cannot resolve hostname '%s' - check server name", host);
	}

	// Timeout
	if (lower_error.find("timeout") != string::npos || lower_error.find("timed out") != string::npos) {
		return StringUtil::Format("Connection timed out to %s:%d - check network connectivity and firewall settings",
								  host, port);
	}

	// Generic connection error
	if (!error.empty()) {
		return StringUtil::Format("Connection failed to %s:%d: %s", host, port, error);
	}

	return StringUtil::Format("Connection failed to %s:%d", host, port);
}

//===----------------------------------------------------------------------===//
// Azure AD Connection Validation
//===----------------------------------------------------------------------===//

void ValidateAzureConnection(ClientContext &context, const MSSQLConnectionInfo &info, int timeout_seconds) {
	MSSQL_STORAGE_DEBUG_LOG(
		1, "ValidateAzureConnection: host=%s port=%d database=%s azure_secret=%s encrypt=%s timeout=%ds",
		info.host.c_str(), info.port, info.database.c_str(), info.azure_secret_name.c_str(),
		info.use_encrypt ? "yes" : "no", timeout_seconds);

	// Acquire Azure AD token
	auto token_result = mssql::azure::AcquireToken(context, info.azure_secret_name);
	if (!token_result.success) {
		throw InvalidInputException("MSSQL Azure AD authentication failed: %s", token_result.error_message);
	}

	MSSQL_STORAGE_DEBUG_LOG(1, "ValidateAzureConnection: token acquired successfully");

	// Build FEDAUTH extension data (encodes token to UTF-16LE)
	auto fedauth_data = mssql::azure::BuildFedAuthExtension(context, info.azure_secret_name);
	if (!fedauth_data.IsValid()) {
		throw InvalidInputException("MSSQL Azure AD authentication failed: could not build FEDAUTH data");
	}

	MSSQL_STORAGE_DEBUG_LOG(1, "ValidateAzureConnection: FEDAUTH data built, token_size=%zu",
							fedauth_data.token_utf16le.size());

	// Create a temporary connection to test Azure AD credentials
	tds::TdsConnection conn;

	// Attempt TCP connection
	MSSQL_STORAGE_DEBUG_LOG(1, "ValidateAzureConnection: attempting TCP connection...");
	if (!conn.Connect(info.host, info.port, timeout_seconds)) {
		string error = conn.GetLastError();
		MSSQL_STORAGE_DEBUG_LOG(1, "ValidateAzureConnection: TCP connection FAILED - %s", error.c_str());
		throw IOException("MSSQL Azure connection validation failed: %s", error);
	}
	MSSQL_STORAGE_DEBUG_LOG(1, "ValidateAzureConnection: TCP connection succeeded");

	// Attempt Azure AD authentication (FEDAUTH)
	MSSQL_STORAGE_DEBUG_LOG(1, "ValidateAzureConnection: attempting Azure AD authentication...");
	if (!conn.AuthenticateWithFedAuth(info.database, fedauth_data.token_utf16le, info.use_encrypt)) {
		string error = conn.GetLastError();
		MSSQL_STORAGE_DEBUG_LOG(1, "ValidateAzureConnection: Azure AD authentication FAILED - %s", error.c_str());
		conn.Close();
		throw InvalidInputException("MSSQL Azure AD connection validation failed: %s", error);
	}
	MSSQL_STORAGE_DEBUG_LOG(1, "ValidateAzureConnection: Azure AD authentication succeeded");

	// Test query
	if (info.use_encrypt) {
		MSSQL_STORAGE_DEBUG_LOG(1, "ValidateAzureConnection: executing validation query (SELECT 1)...");
		try {
			if (!conn.ExecuteBatch("SELECT 1")) {
				string error = conn.GetLastError();
				MSSQL_STORAGE_DEBUG_LOG(1, "ValidateAzureConnection: validation query FAILED - %s", error.c_str());
				conn.Close();
				throw InvalidInputException("MSSQL Azure connection validation failed: validation query failed: %s",
											error);
			}
			// Drain results
			auto *socket = conn.GetSocket();
			if (socket) {
				std::vector<uint8_t> response;
				socket->ReceiveMessage(response, 5000);
				conn.TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
			}
			MSSQL_STORAGE_DEBUG_LOG(1, "ValidateAzureConnection: validation query succeeded");
		} catch (const std::exception &e) {
			string error = e.what();
			MSSQL_STORAGE_DEBUG_LOG(1, "ValidateAzureConnection: validation query FAILED with exception - %s",
									error.c_str());
			conn.Close();
			throw InvalidInputException("MSSQL Azure connection validation failed: %s", error);
		}
	}

	conn.Close();
	MSSQL_STORAGE_DEBUG_LOG(1, "ValidateAzureConnection: validation complete");
}

//===----------------------------------------------------------------------===//
// Manual Token Connection Validation (Spec 032)
//===----------------------------------------------------------------------===//

void ValidateManualTokenConnection(const MSSQLConnectionInfo &info, const std::vector<uint8_t> &token_utf16le,
                                   int timeout_seconds) {
	MSSQL_STORAGE_DEBUG_LOG(1, "ValidateManualTokenConnection: host=%s port=%d database=%s encrypt=%s timeout=%ds",
	                        info.host.c_str(), info.port, info.database.c_str(), info.use_encrypt ? "yes" : "no",
	                        timeout_seconds);

	// Create a temporary connection to test the pre-provided token
	tds::TdsConnection conn;

	// Attempt TCP connection
	MSSQL_STORAGE_DEBUG_LOG(1, "ValidateManualTokenConnection: attempting TCP connection...");
	if (!conn.Connect(info.host, info.port, timeout_seconds)) {
		string error = conn.GetLastError();
		MSSQL_STORAGE_DEBUG_LOG(1, "ValidateManualTokenConnection: TCP connection FAILED - %s", error.c_str());
		throw IOException("MSSQL manual token connection validation failed: %s", error);
	}
	MSSQL_STORAGE_DEBUG_LOG(1, "ValidateManualTokenConnection: TCP connection succeeded");

	// Attempt Azure AD authentication (FEDAUTH) with the pre-provided token
	MSSQL_STORAGE_DEBUG_LOG(1, "ValidateManualTokenConnection: attempting FEDAUTH with manual token...");
	if (!conn.AuthenticateWithFedAuth(info.database, token_utf16le, info.use_encrypt)) {
		string error = conn.GetLastError();
		MSSQL_STORAGE_DEBUG_LOG(1, "ValidateManualTokenConnection: FEDAUTH FAILED - %s", error.c_str());
		conn.Close();
		throw InvalidInputException("MSSQL manual token authentication failed: %s", error);
	}
	MSSQL_STORAGE_DEBUG_LOG(1, "ValidateManualTokenConnection: FEDAUTH succeeded");

	// Test query
	if (info.use_encrypt) {
		MSSQL_STORAGE_DEBUG_LOG(1, "ValidateManualTokenConnection: executing validation query (SELECT 1)...");
		try {
			if (!conn.ExecuteBatch("SELECT 1")) {
				string error = conn.GetLastError();
				MSSQL_STORAGE_DEBUG_LOG(1, "ValidateManualTokenConnection: validation query FAILED - %s",
				                        error.c_str());
				conn.Close();
				throw InvalidInputException("MSSQL manual token connection validation failed: query failed: %s", error);
			}
			// Drain results
			auto *socket = conn.GetSocket();
			if (socket) {
				std::vector<uint8_t> response;
				socket->ReceiveMessage(response, 5000);
				conn.TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
			}
			MSSQL_STORAGE_DEBUG_LOG(1, "ValidateManualTokenConnection: validation query succeeded");
		} catch (const std::exception &e) {
			string error = e.what();
			MSSQL_STORAGE_DEBUG_LOG(1, "ValidateManualTokenConnection: validation query FAILED with exception - %s",
			                        error.c_str());
			conn.Close();
			throw InvalidInputException("MSSQL manual token connection validation failed: %s", error);
		}
	}

	conn.Close();
	MSSQL_STORAGE_DEBUG_LOG(1, "ValidateManualTokenConnection: validation complete");
}

void ValidateConnection(const MSSQLConnectionInfo &info, int timeout_seconds) {
	MSSQL_STORAGE_DEBUG_LOG(1, "ValidateConnection: host=%s port=%d user=%s database=%s encrypt=%s timeout=%ds",
							info.host.c_str(), info.port, info.user.c_str(), info.database.c_str(),
							info.use_encrypt ? "yes" : "no", timeout_seconds);

	// Create a temporary connection to test credentials
	tds::TdsConnection conn;

	// Attempt TCP connection
	MSSQL_STORAGE_DEBUG_LOG(1, "ValidateConnection: attempting TCP connection...");
	if (!conn.Connect(info.host, info.port, timeout_seconds)) {
		string error = conn.GetLastError();
		string translated = TranslateConnectionError(error, info.host, info.port, info.user, info.database);
		MSSQL_STORAGE_DEBUG_LOG(1, "ValidateConnection: TCP connection FAILED - raw: %s, translated: %s", error.c_str(),
								translated.c_str());
		throw IOException("MSSQL connection validation failed: %s", translated);
	}
	MSSQL_STORAGE_DEBUG_LOG(1, "ValidateConnection: TCP connection succeeded");

	// Attempt authentication
	MSSQL_STORAGE_DEBUG_LOG(1, "ValidateConnection: attempting authentication...");
	if (!conn.Authenticate(info.user, info.password, info.database, info.use_encrypt)) {
		string error = conn.GetLastError();
		string translated = TranslateConnectionError(error, info.host, info.port, info.user, info.database);
		MSSQL_STORAGE_DEBUG_LOG(1, "ValidateConnection: authentication FAILED - raw: %s, translated: %s", error.c_str(),
								translated.c_str());
		conn.Close();
		throw InvalidInputException("MSSQL connection validation failed: %s", translated);
	}
	MSSQL_STORAGE_DEBUG_LOG(1, "ValidateConnection: authentication succeeded");

	// If TLS is enabled, execute a simple validation query to verify TLS data path works
	// This catches TLS issues that may only appear during actual data transfer
	if (info.use_encrypt) {
		MSSQL_STORAGE_DEBUG_LOG(1, "ValidateConnection: executing TLS validation query (SELECT 1)...");
		try {
			if (!conn.ExecuteBatch("SELECT 1")) {
				string error = conn.GetLastError();
				string translated = TranslateConnectionError(error, info.host, info.port, info.user, info.database);
				MSSQL_STORAGE_DEBUG_LOG(1, "ValidateConnection: TLS validation query FAILED - raw: %s, translated: %s",
										error.c_str(), translated.c_str());
				conn.Close();
				throw InvalidInputException(
					"MSSQL connection validation failed: TLS connection established but validation query failed. "
					"The server may have network issues or TLS may be misconfigured. Details: %s",
					translated);
			}
			// Drain any results to reset connection state
			auto *socket = conn.GetSocket();
			if (socket) {
				std::vector<uint8_t> response;
				socket->ReceiveMessage(response, 5000);
				conn.TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
			}
			MSSQL_STORAGE_DEBUG_LOG(1, "ValidateConnection: TLS validation query succeeded");
		} catch (const std::exception &e) {
			string error = e.what();
			string translated = TranslateConnectionError(error, info.host, info.port, info.user, info.database);
			MSSQL_STORAGE_DEBUG_LOG(1, "ValidateConnection: TLS validation query FAILED with exception - %s",
									error.c_str());
			conn.Close();
			throw InvalidInputException(
				"MSSQL connection validation failed: TLS connection established but validation query failed. "
				"Details: %s",
				translated);
		}
	}

	// Close the test connection - it will be recreated by the pool
	conn.Close();
	MSSQL_STORAGE_DEBUG_LOG(1, "ValidateConnection: validation complete, test connection closed");
}

//===----------------------------------------------------------------------===//
// Storage Extension callbacks
//===----------------------------------------------------------------------===//

unique_ptr<Catalog> MSSQLAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
								AttachedDatabase &db, const string &name, AttachInfo &info, AttachOptions &options) {
	// Extract SECRET, azure_secret, and access_token parameters (optional if connection string is provided)
	// Remove them from options so DuckDB's StorageOptions doesn't reject them as unrecognized
	string secret_name;
	string azure_secret_name;
	string access_token;  // Spec 032: Direct Azure AD JWT token
	bool catalog_option_specified = false;
	bool catalog_enabled_option = true;	 // Default to true
	for (auto it = options.options.begin(); it != options.options.end();) {
		auto lower_name = StringUtil::Lower(it->first);
		if (lower_name == "secret") {
			secret_name = it->second.ToString();
			it = options.options.erase(it);
		} else if (lower_name == "azure_secret") {
			azure_secret_name = it->second.ToString();
			it = options.options.erase(it);
		} else if (lower_name == "access_token") {
			// Spec 032: Parse ACCESS_TOKEN ATTACH option
			access_token = it->second.ToString();
			it = options.options.erase(it);
		} else if (lower_name == "catalog") {
			catalog_option_specified = true;
			catalog_enabled_option = it->second.GetValue<bool>();
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
		// If access_token or azure_secret is provided as option, allow missing user/password
		bool azure_auth_option = !access_token.empty() || !azure_secret_name.empty();
		ctx->connection_info = MSSQLConnectionInfo::FromConnectionString(connection_string, azure_auth_option);

		// Set access_token from ATTACH option if provided (Spec 032: takes precedence)
		if (!access_token.empty()) {
			ctx->connection_info->access_token = access_token;
			ctx->connection_info->use_azure_auth = true;
		} else if (!azure_secret_name.empty()) {
			// Set azure_secret from ATTACH option if provided
			ctx->connection_info->azure_secret_name = azure_secret_name;
			ctx->connection_info->use_azure_auth = true;
		}
	} else {
		// Neither SECRET nor connection string provided
		throw InvalidInputException(
			"MSSQL Error: Either SECRET or connection string is required for ATTACH.\n"
			"With secret: ATTACH '' AS %s (TYPE mssql, SECRET <secret_name>)\n"
			"With connection string: ATTACH 'Server=host;Database=db;User Id=user;Password=pass' AS %s (TYPE mssql)",
			name, name);
	}

	// Apply CATALOG option from ATTACH if specified (overrides connection string/secret value)
	if (catalog_option_specified) {
		ctx->connection_info->catalog_enabled = catalog_enabled_option;
		MSSQL_STORAGE_DEBUG_LOG(1, "CATALOG option from ATTACH: %s", catalog_enabled_option ? "true" : "false");
	}

	// T040 (Bug 0.7): Cache endpoint type at ATTACH time for performance
	// Fabric endpoints don't support BCP/INSERT BULK, need fallback to INSERT
	ctx->connection_info->is_fabric_endpoint = ctx->connection_info->IsFabricEndpoint();
	if (ctx->connection_info->is_fabric_endpoint) {
		MSSQL_STORAGE_DEBUG_LOG(1, "Fabric endpoint detected: %s (BCP disabled, using INSERT fallback)",
								ctx->connection_info->host.c_str());
	}

	// Validate connection before registering context or creating catalog
	// This ensures we fail fast on invalid credentials
	auto pool_config = LoadPoolConfig(context);

	// For Azure auth, we need to acquire token and use FEDAUTH validation
	std::vector<uint8_t> fedauth_token_utf16le;
	if (!ctx->connection_info->access_token.empty()) {
		// Spec 032: Manual token authentication - validate token format and audience at ATTACH time
		MSSQL_STORAGE_DEBUG_LOG(1, "Manual token auth: validating token at ATTACH time");

		// Create auth strategy - this validates JWT format, audience, and expiration
		auto auth_strategy = tds::AuthStrategyFactory::CreateManualToken(
		    ctx->connection_info->access_token, ctx->connection_info->database);

		// Get the pre-encoded UTF-16LE token for pool creation
		tds::FedAuthInfo dummy_info;  // Not used by ManualTokenAuthStrategy
		fedauth_token_utf16le = auth_strategy->GetFedAuthToken(dummy_info);

		// Validate actual connection to server using the pre-provided token
		ValidateManualTokenConnection(*ctx->connection_info, fedauth_token_utf16le, pool_config.connection_timeout);
	} else if (ctx->connection_info->use_azure_auth) {
		// T027 (FR-006): Validate FEDAUTH connections at ATTACH time (fail-fast)
		// This ensures invalid credentials are detected immediately, not on first query
		MSSQL_STORAGE_DEBUG_LOG(1, "Azure auth: validating connection at ATTACH time");
		ValidateAzureConnection(context, *ctx->connection_info, pool_config.connection_timeout);

		// Build FEDAUTH token for pool factory (uses validated credentials)
		auto fedauth_data = mssql::azure::BuildFedAuthExtension(context, ctx->connection_info->azure_secret_name);
		fedauth_token_utf16le = std::move(fedauth_data.token_utf16le);
	} else {
		ValidateConnection(*ctx->connection_info, pool_config.connection_timeout);
	}

	// Register context (only reached if validation succeeds)
	auto &manager = MSSQLContextManager::Get(*context.db);
	manager.RegisterContext(name, ctx);

	// Create connection pool for this context
	if (!ctx->connection_info->access_token.empty() || ctx->connection_info->use_azure_auth) {
		// Use Azure AD authentication pool (manual token or azure_secret)
		MssqlPoolManager::Instance().GetOrCreatePoolWithAzureAuth(
			name, pool_config, ctx->connection_info->host, ctx->connection_info->port, ctx->connection_info->database,
			fedauth_token_utf16le, ctx->connection_info->use_encrypt);
	} else {
		// Use SQL authentication pool
		MssqlPoolManager::Instance().GetOrCreatePool(
			name, pool_config, ctx->connection_info->host, ctx->connection_info->port, ctx->connection_info->user,
			ctx->connection_info->password, ctx->connection_info->database, ctx->connection_info->use_encrypt);
	}

	// Create MSSQLCatalog with connection info and access mode from options
	// The catalog will use the connection pool to query SQL Server
	// options.access_mode is set by DuckDB based on the READ_ONLY option in ATTACH
	// catalog_enabled flag determines whether schema discovery is available
	auto catalog = make_uniq<MSSQLCatalog>(db, name, ctx->connection_info, options.access_mode,
										   ctx->connection_info->catalog_enabled);
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
