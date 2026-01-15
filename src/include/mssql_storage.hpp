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
#include "duckdb/catalog/duck_catalog.hpp"
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
	uint16_t port;
	string database;
	string user;
	string password;
	bool use_ssl = false;
	bool connected = false;

	// Create from secret
	static shared_ptr<MSSQLConnectionInfo> FromSecret(ClientContext &context, const string &secret_name);

	// Create from connection string
	// Format: "Server=host,port;Database=db;User Id=user;Password=pass;Encrypt=yes/no"
	static shared_ptr<MSSQLConnectionInfo> FromConnectionString(const string &connection_string);

	// Validate connection string format
	// Returns: empty string if valid, error message if invalid
	static string ValidateConnectionString(const string &connection_string);
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
// MSSQLCatalog - Custom catalog that cleans up context on detach
//===----------------------------------------------------------------------===//
class MSSQLCatalog : public DuckCatalog {
public:
	MSSQLCatalog(AttachedDatabase &db, const string &context_name);

	void OnDetach(ClientContext &context) override;

private:
	string context_name;
};

//===----------------------------------------------------------------------===//
// Registration and callbacks
//===----------------------------------------------------------------------===//

// Register storage extension for ATTACH TYPE mssql
void RegisterMSSQLStorageExtension(ExtensionLoader &loader);

// Attach callback
unique_ptr<Catalog> MSSQLAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                AttachedDatabase &db, const string &name, AttachInfo &info, AttachOptions &options);

// Transaction manager factory
unique_ptr<TransactionManager> MSSQLCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                                             AttachedDatabase &db, Catalog &catalog);

}  // namespace duckdb
