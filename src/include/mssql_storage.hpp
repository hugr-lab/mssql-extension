//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// mssql_storage.hpp
//
// Storage extension for ATTACH/DETACH and context management
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

#include <atomic>
#include <mutex>

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLConnectionInfo - Connection parameters extracted from secret or connection string
//===----------------------------------------------------------------------===//
struct MSSQLConnectionInfo {
	string host;
	uint16_t port = 1433;
	string database;
	string user;
	string password;
	bool use_encrypt = true;  // Enable TLS encryption (default: true for security)
	bool connected = false;
	bool catalog_enabled = true;  // Enable DuckDB catalog integration (false = raw query mode only)

	//===----------------------------------------------------------------------===//
	// Azure AD Authentication (Phase 2 FEDAUTH)
	//===----------------------------------------------------------------------===//
	bool use_azure_auth = false;  // Use Azure AD authentication instead of SQL auth
	string azure_secret_name;	  // Name of the Azure secret for token acquisition

	//===----------------------------------------------------------------------===//
	// Manual Token Authentication (Spec 032: FEDAUTH Token Provider Enhancements)
	//===----------------------------------------------------------------------===//
	string access_token;  // Pre-provided Azure AD JWT access token (takes precedence over azure_secret)

	//===----------------------------------------------------------------------===//
	// Catalog Visibility Filters (Spec 033: regex-based object filtering)
	//===----------------------------------------------------------------------===//
	string schema_filter;  // Regex pattern for schema visibility (empty = all visible)
	string table_filter;   // Regex pattern for table/view visibility (empty = all visible)

	//===----------------------------------------------------------------------===//
	// ORDER BY Pushdown (Spec 039)
	//===----------------------------------------------------------------------===//
	int8_t order_pushdown = -1;	 // -1 = not specified (use global setting), 0 = disabled, 1 = enabled

	//===----------------------------------------------------------------------===//
	// Endpoint Type Flags (T040-T041: cached at ATTACH time for performance)
	//===----------------------------------------------------------------------===//
	bool is_fabric_endpoint = false;  // True if targeting Microsoft Fabric (no BCP/INSERT BULK support)

	// Check if this connection targets an Azure endpoint
	// Azure endpoints require Azure AD auth support and TLS hostname verification
	bool IsAzureEndpoint() const;

	// Check if this connection targets a Microsoft Fabric endpoint
	// Fabric has limited feature support (e.g., DBCC commands not available)
	bool IsFabricEndpoint() const;

	// Check if this connection targets an Azure Synapse endpoint
	bool IsSynapseEndpoint() const;

	// Create from secret
	static shared_ptr<MSSQLConnectionInfo> FromSecret(ClientContext &context, const string &secret_name);

	// Create from connection string (ADO.NET format or URI format)
	// ADO.NET: "Server=host,port;Database=db;User Id=user;Password=pass;Encrypt=yes/no"
	// URI: "mssql://user:password@host:port/database?encrypt=true"
	// Parameters:
	//   azure_auth - if true, user/password are optional (Azure AD authentication via azure_secret)
	static shared_ptr<MSSQLConnectionInfo> FromConnectionString(const string &connection_string,
																bool azure_auth = false);

	// Validate connection string format
	// Returns: empty string if valid, error message if invalid
	// Parameters:
	//   azure_auth - if true, user/password are optional (Azure AD authentication via azure_secret)
	static string ValidateConnectionString(const string &connection_string, bool azure_auth = false);

	// Check if string is a URI format (mssql://...)
	static bool IsUriFormat(const string &str);

	// Check if string is a connection string (contains key=value pairs)
	static bool IsConnectionString(const string &str);
};

//===----------------------------------------------------------------------===//
// MSSQLContext - Attached context state
//===----------------------------------------------------------------------===//
struct MSSQLContext {
	string name;
	string secret_name;
	shared_ptr<MSSQLConnectionInfo> connection_info;
	optional_ptr<AttachedDatabase> attached_db;

	MSSQLContext(const string &name, const string &secret_name);
};

//===----------------------------------------------------------------------===//
// MSSQLContextManager - Global context manager (singleton per DatabaseInstance)
//===----------------------------------------------------------------------===//
class MSSQLContextManager {
public:
	// Get singleton instance for a DatabaseInstance
	static MSSQLContextManager &Get(DatabaseInstance &db);

	// Context operations
	void RegisterContext(const string &name, shared_ptr<MSSQLContext> ctx);
	void UnregisterContext(const string &name);
	shared_ptr<MSSQLContext> GetContext(const string &name);
	bool HasContext(const string &name);
	vector<string> ListContexts();

private:
	mutex lock;
	case_insensitive_map_t<shared_ptr<MSSQLContext>> contexts;
};

//===----------------------------------------------------------------------===//
// MSSQLStorageExtensionInfo - Shared state for storage extension
//===----------------------------------------------------------------------===//
struct MSSQLStorageExtensionInfo : public StorageExtensionInfo {
	// Reserved for future connection pooling, etc.
};

//===----------------------------------------------------------------------===//
// Registration and callbacks
//===----------------------------------------------------------------------===//

// Validate connection by attempting to connect and authenticate
// Throws IOException or InvalidInputException with descriptive error on failure
void ValidateConnection(const MSSQLConnectionInfo &info, int timeout_seconds = 30);

// Register storage extension for ATTACH TYPE mssql
void RegisterMSSQLStorageExtension(ExtensionLoader &loader);

// Attach callback
unique_ptr<Catalog> MSSQLAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
								AttachedDatabase &db, const string &name, AttachInfo &info, AttachOptions &options);

// Transaction manager factory
unique_ptr<TransactionManager> MSSQLCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
															 AttachedDatabase &db, Catalog &catalog);

}  // namespace duckdb
