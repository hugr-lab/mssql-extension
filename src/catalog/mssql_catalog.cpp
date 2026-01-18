#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_schema_entry.hpp"
#include "catalog/mssql_ddl_translator.hpp"
#include "catalog/mssql_statistics.hpp"
#include "connection/mssql_pool_manager.hpp"
#include "connection/mssql_settings.hpp"
#include "query/mssql_simple_query.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// SQL Query for Database Collation
//===----------------------------------------------------------------------===//

static const char *DATABASE_COLLATION_SQL =
    "SELECT CAST(DATABASEPROPERTYEX(DB_NAME(), 'Collation') AS NVARCHAR(128)) AS db_collation";

//===----------------------------------------------------------------------===//
// Constructor / Destructor
//===----------------------------------------------------------------------===//

MSSQLCatalog::MSSQLCatalog(AttachedDatabase &db, const string &context_name,
                           shared_ptr<MSSQLConnectionInfo> connection_info, AccessMode access_mode)
    : Catalog(db), context_name_(context_name), connection_info_(std::move(connection_info)),
      access_mode_(access_mode), default_schema_("dbo") {

	// Create metadata cache with TTL from settings (0 = manual refresh only)
	int64_t cache_ttl = 0;  // Default: manual refresh only
	metadata_cache_ = make_uniq<MSSQLMetadataCache>(cache_ttl);

	// Create statistics provider with default TTL (will be configured from settings later)
	statistics_provider_ = make_uniq<MSSQLStatisticsProvider>();
}

MSSQLCatalog::~MSSQLCatalog() = default;

//===----------------------------------------------------------------------===//
// Initialization
//===----------------------------------------------------------------------===//

void MSSQLCatalog::Initialize(bool load_builtin) {
	// Get or create connection pool for this catalog
	// The pool is managed by MssqlPoolManager and shared with other operations
	auto &pool_manager = MssqlPoolManager::Instance();

	// Check if pool already exists (created during attach)
	auto existing_pool = pool_manager.GetPool(context_name_);
	if (existing_pool) {
		// Wrap raw pointer in shared_ptr with no-op deleter (pool manager owns the pool)
		connection_pool_ = shared_ptr<tds::ConnectionPool>(existing_pool, [](tds::ConnectionPool *) {});
	}
	// Note: Pool should be created during ATTACH; if missing, queries will fail later

	// Query database collation (needed for column metadata)
	if (connection_pool_) {
		QueryDatabaseCollation();
	}
}

tds::ConnectionFactory MSSQLCatalog::CreateConnectionFactory() {
	auto conn_info = connection_info_;  // Capture shared_ptr
	return [conn_info]() -> std::shared_ptr<tds::TdsConnection> {
		auto connection = std::make_shared<tds::TdsConnection>();
		// First establish TCP connection
		if (!connection->Connect(conn_info->host, conn_info->port)) {
			throw IOException("Failed to connect to MSSQL server %s:%d", conn_info->host, conn_info->port);
		}
		// Then authenticate (optionally with TLS)
		if (!connection->Authenticate(conn_info->user, conn_info->password,
		                              conn_info->database, conn_info->use_encrypt)) {
			throw IOException("Failed to authenticate to MSSQL server");
		}
		return connection;
	};
}

void MSSQLCatalog::QueryDatabaseCollation() {
	if (!connection_pool_) {
		return;
	}

	auto connection = connection_pool_->Acquire();
	if (!connection) {
		return;
	}

	try {
		// Use MSSQLSimpleQuery for clean query execution
		std::string collation = MSSQLSimpleQuery::ExecuteScalar(*connection, DATABASE_COLLATION_SQL);

		if (!collation.empty()) {
			database_collation_ = collation;

			// Update metadata cache with collation
			if (metadata_cache_) {
				metadata_cache_->SetDatabaseCollation(database_collation_);
			}
		}
	} catch (...) {
		connection_pool_->Release(std::move(connection));
		throw;
	}

	connection_pool_->Release(std::move(connection));
}

//===----------------------------------------------------------------------===//
// Catalog Type
//===----------------------------------------------------------------------===//

string MSSQLCatalog::GetCatalogType() {
	return "mssql";
}

//===----------------------------------------------------------------------===//
// Schema Operations
//===----------------------------------------------------------------------===//

optional_ptr<SchemaCatalogEntry> MSSQLCatalog::LookupSchema(CatalogTransaction transaction,
                                                             const EntryLookupInfo &schema_lookup,
                                                             OnEntryNotFound if_not_found) {
	auto &name = schema_lookup.GetEntryName();

	// Ensure metadata cache is loaded
	if (transaction.context) {
		EnsureCacheLoaded(*transaction.context);
	}

	// Check if schema exists in cache
	if (!metadata_cache_->HasSchema(name)) {
		if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw CatalogException("Schema '%s' not found in MSSQL database", name);
		}
		return nullptr;
	}

	// Get or create schema entry
	return &GetOrCreateSchemaEntry(name);
}

void MSSQLCatalog::ScanSchemas(ClientContext &context,
                               std::function<void(SchemaCatalogEntry &)> callback) {
	// Ensure cache is loaded
	EnsureCacheLoaded(context);

	auto schema_names = metadata_cache_->GetSchemaNames();
	for (const auto &name : schema_names) {
		auto &schema_entry = GetOrCreateSchemaEntry(name);
		callback(schema_entry);
	}
}

MSSQLSchemaEntry &MSSQLCatalog::GetOrCreateSchemaEntry(const string &schema_name) {
	std::lock_guard<std::mutex> lock(schema_mutex_);

	auto it = schema_entries_.find(schema_name);
	if (it != schema_entries_.end()) {
		return *it->second;
	}

	// Create new schema entry
	auto entry = make_uniq<MSSQLSchemaEntry>(*this, schema_name);
	auto &entry_ref = *entry;
	schema_entries_[schema_name] = std::move(entry);
	return entry_ref;
}

optional_ptr<CatalogEntry> MSSQLCatalog::CreateSchema(CatalogTransaction transaction,
                                                       CreateSchemaInfo &info) {
	CheckWriteAccess("CREATE SCHEMA");

	// Generate T-SQL for CREATE SCHEMA
	string tsql = MSSQLDDLTranslator::TranslateCreateSchema(info.schema);

	// Execute DDL on SQL Server
	if (transaction.HasContext()) {
		ExecuteDDL(transaction.GetContext(), tsql);
	} else {
		throw InternalException("Cannot execute CREATE SCHEMA without client context");
	}

	// Invalidate cache so the new schema is visible
	InvalidateMetadataCache();

	// Re-load and return the schema entry
	if (transaction.HasContext()) {
		EnsureCacheLoaded(transaction.GetContext());
	}

	return &GetOrCreateSchemaEntry(info.schema);
}

void MSSQLCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	CheckWriteAccess("DROP SCHEMA");

	// Generate T-SQL for DROP SCHEMA
	string tsql = MSSQLDDLTranslator::TranslateDropSchema(info.name);

	// Execute DDL on SQL Server
	ExecuteDDL(context, tsql);

	// Invalidate cache
	InvalidateMetadataCache();

	// Remove the schema entry from our local cache
	{
		std::lock_guard<std::mutex> lock(schema_mutex_);
		schema_entries_.erase(info.name);
	}
}

//===----------------------------------------------------------------------===//
// Write Operations (all throw - read-only catalog)
//===----------------------------------------------------------------------===//

PhysicalOperator &MSSQLCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner,
                                            LogicalInsert &op, optional_ptr<PhysicalOperator> plan) {
	throw NotImplementedException("MSSQL catalog is read-only: INSERT is not supported");
}

PhysicalOperator &MSSQLCatalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
                                                   LogicalCreateTable &op, PhysicalOperator &plan) {
	throw NotImplementedException("MSSQL catalog is read-only: CREATE TABLE AS is not supported");
}

PhysicalOperator &MSSQLCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner,
                                            LogicalDelete &op, PhysicalOperator &plan) {
	throw NotImplementedException("MSSQL catalog is read-only: DELETE is not supported");
}

PhysicalOperator &MSSQLCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner,
                                            LogicalUpdate &op, PhysicalOperator &plan) {
	throw NotImplementedException("MSSQL catalog is read-only: UPDATE is not supported");
}

unique_ptr<LogicalOperator> MSSQLCatalog::BindCreateIndex(Binder &binder, CreateStatement &stmt,
                                                           TableCatalogEntry &table,
                                                           unique_ptr<LogicalOperator> plan) {
	throw NotImplementedException("MSSQL catalog is read-only: CREATE INDEX is not supported");
}

//===----------------------------------------------------------------------===//
// Catalog Information
//===----------------------------------------------------------------------===//

DatabaseSize MSSQLCatalog::GetDatabaseSize(ClientContext &context) {
	DatabaseSize size;
	size.free_blocks = 0;
	size.total_blocks = 0;
	size.used_blocks = 0;
	size.wal_size = 0;
	size.block_size = 0;
	return size;
}

bool MSSQLCatalog::InMemory() {
	return false;  // This is a remote database
}

string MSSQLCatalog::GetDBPath() {
	// Return connection info as path representation
	return "mssql://" + connection_info_->host + ":" + std::to_string(connection_info_->port) + "/" +
	       connection_info_->database;
}

//===----------------------------------------------------------------------===//
// Detach Hook
//===----------------------------------------------------------------------===//

void MSSQLCatalog::OnDetach(ClientContext &context) {
	// Remove connection pool for this context (shuts down and cleans up connections)
	MssqlPoolManager::Instance().RemovePool(context_name_);

	// Unregister context from the manager
	auto &manager = MSSQLContextManager::Get(*context.db);
	manager.UnregisterContext(context_name_);
}

//===----------------------------------------------------------------------===//
// MSSQL-specific Accessors
//===----------------------------------------------------------------------===//

tds::ConnectionPool &MSSQLCatalog::GetConnectionPool() {
	if (!connection_pool_) {
		throw IOException("MSSQL connection pool not initialized");
	}
	return *connection_pool_;
}

MSSQLMetadataCache &MSSQLCatalog::GetMetadataCache() {
	return *metadata_cache_;
}

MSSQLStatisticsProvider &MSSQLCatalog::GetStatisticsProvider() {
	return *statistics_provider_;
}

const string &MSSQLCatalog::GetDatabaseCollation() const {
	return database_collation_;
}

const MSSQLConnectionInfo &MSSQLCatalog::GetConnectionInfo() const {
	return *connection_info_;
}

const string &MSSQLCatalog::GetContextName() const {
	return context_name_;
}

//===----------------------------------------------------------------------===//
// Access Mode (READ_ONLY Support)
//===----------------------------------------------------------------------===//

bool MSSQLCatalog::IsReadOnly() const {
	return access_mode_ == AccessMode::READ_ONLY;
}

AccessMode MSSQLCatalog::GetAccessMode() const {
	return access_mode_;
}

void MSSQLCatalog::CheckWriteAccess(const char *operation_name) const {
	if (IsReadOnly()) {
		if (operation_name) {
			throw CatalogException("Cannot execute %s: MSSQL catalog '%s' is attached in read-only mode",
			                       operation_name, context_name_);
		} else {
			throw CatalogException("Cannot modify MSSQL catalog '%s': attached in read-only mode", context_name_);
		}
	}
}

//===----------------------------------------------------------------------===//
// DDL Execution
//===----------------------------------------------------------------------===//

void MSSQLCatalog::ExecuteDDL(ClientContext &context, const string &tsql) {
	if (!connection_pool_) {
		throw IOException("MSSQL connection pool not initialized - cannot execute DDL");
	}

	auto connection = connection_pool_->Acquire();
	if (!connection) {
		throw IOException("Failed to acquire connection for DDL execution");
	}

	try {
		auto result = MSSQLSimpleQuery::Execute(*connection, tsql);

		if (!result.success) {
			connection_pool_->Release(std::move(connection));
			throw CatalogException("MSSQL DDL error: SQL Server error %d: %s",
			                       result.error_number, result.error_message);
		}
	} catch (...) {
		connection_pool_->Release(std::move(connection));
		throw;
	}

	connection_pool_->Release(std::move(connection));
}

void MSSQLCatalog::InvalidateMetadataCache() {
	if (metadata_cache_) {
		metadata_cache_->Invalidate();
	}

	// Also clear the local schema entry cache
	std::lock_guard<std::mutex> lock(schema_mutex_);
	for (auto &entry : schema_entries_) {
		entry.second->GetTableSet().Invalidate();
	}
}

void MSSQLCatalog::EnsureCacheLoaded(ClientContext &context) {
	if (!connection_pool_) {
		throw IOException("MSSQL connection pool not initialized - cannot refresh cache");
	}

	// Check if cache needs refresh
	if (!metadata_cache_->NeedsRefresh() && !metadata_cache_->IsExpired()) {
		return;
	}

	// Load cache TTL from settings
	int64_t cache_ttl = LoadCatalogCacheTTL(context);
	metadata_cache_->SetTTL(cache_ttl);

	// Acquire connection and refresh cache
	auto connection = connection_pool_->Acquire();
	if (!connection) {
		throw IOException("Failed to acquire connection for metadata refresh");
	}

	try {
		metadata_cache_->Refresh(*connection, database_collation_);
	} catch (...) {
		connection_pool_->Release(std::move(connection));
		throw;
	}

	connection_pool_->Release(std::move(connection));
}

}  // namespace duckdb
