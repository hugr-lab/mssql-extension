#include "catalog/mssql_catalog.hpp"
#include "azure/azure_token.hpp"
#include "catalog/mssql_ddl_translator.hpp"
#include "catalog/mssql_schema_entry.hpp"
#include "catalog/mssql_statistics.hpp"
#include "catalog/mssql_table_entry.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "connection/mssql_pool_manager.hpp"
#include "connection/mssql_settings.hpp"
#include "dml/ctas/mssql_ctas_planner.hpp"
#include "dml/delete/mssql_delete_target.hpp"
#include "dml/delete/mssql_physical_delete.hpp"
#include "dml/insert/mssql_insert_config.hpp"
#include "dml/insert/mssql_insert_target.hpp"
#include "dml/insert/mssql_physical_insert.hpp"
#include "dml/mssql_dml_config.hpp"
#include "dml/update/mssql_physical_update.hpp"
#include "dml/update/mssql_update_target.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "query/mssql_simple_query.hpp"

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
						   shared_ptr<MSSQLConnectionInfo> connection_info, AccessMode access_mode,
						   bool catalog_enabled)
	: Catalog(db),
	  context_name_(context_name),
	  connection_info_(std::move(connection_info)),
	  access_mode_(access_mode),
	  catalog_enabled_(catalog_enabled),
	  default_schema_("dbo") {
	// Create metadata cache with TTL from settings (0 = manual refresh only)
	int64_t cache_ttl = 0;	// Default: manual refresh only
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

	// Skip metadata initialization when catalog integration is disabled
	// (mssql_scan/mssql_exec will still work via raw queries)
	if (!catalog_enabled_) {
		return;
	}

	// Query database collation (needed for column metadata)
	if (connection_pool_) {
		QueryDatabaseCollation();
	}
}

tds::ConnectionFactory MSSQLCatalog::CreateConnectionFactory() {
	auto conn_info = connection_info_;	// Capture shared_ptr
	return [conn_info]() -> std::shared_ptr<tds::TdsConnection> {
		auto connection = std::make_shared<tds::TdsConnection>();
		// First establish TCP connection
		if (!connection->Connect(conn_info->host, conn_info->port)) {
			throw IOException("Failed to connect to MSSQL server %s:%d", conn_info->host, conn_info->port);
		}
		// Then authenticate (optionally with TLS)
		if (!connection->Authenticate(conn_info->user, conn_info->password, conn_info->database,
									  conn_info->use_encrypt)) {
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

	// Ensure cache settings are loaded (sets TTL)
	if (transaction.context) {
		EnsureCacheLoaded(*transaction.context);
	}

	// T035 (FR-003/Bug 0.2): Check cache BEFORE acquiring connection to reduce connection usage
	// Fast path: If schemas are already loaded and schema exists in cache, skip connection acquisition
	if (metadata_cache_->GetSchemasState() == CacheLoadState::LOADED && metadata_cache_->HasSchema(name)) {
		return &GetOrCreateSchemaEntry(name);
	}

	// T013-T014 (FR-003): Use ConnectionProvider for transaction-aware connection acquisition
	// This ensures schema lookups during INSERT in transaction use the pinned connection
	if (!connection_pool_) {
		throw InternalException("Connection pool not initialized");
	}

	std::shared_ptr<tds::TdsConnection> connection;
	if (transaction.context) {
		// Use ConnectionProvider for proper transaction handling
		connection = ConnectionProvider::GetConnection(*transaction.context, *this);
	} else {
		// Fallback to direct pool access if no context available
		connection = connection_pool_->Acquire();
	}
	if (!connection) {
		throw IOException("Failed to acquire connection for schema lookup");
	}

	// Trigger lazy loading of schema list
	metadata_cache_->EnsureSchemasLoaded(*connection);

	// Release connection properly (no-op if pinned to transaction)
	if (transaction.context) {
		ConnectionProvider::ReleaseConnection(*transaction.context, *this, std::move(connection));
	} else {
		connection_pool_->Release(std::move(connection));
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

void MSSQLCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	// Ensure cache is loaded (sets TTL)
	EnsureCacheLoaded(context);

	// T036 (FR-003/Bug 0.2): Check cache BEFORE acquiring connection
	// Fast path: If schemas are already loaded, get names without acquiring connection
	vector<string> schema_names;
	if (metadata_cache_->TryGetCachedSchemaNames(schema_names)) {
		// Cache hit - iterate without connection
		for (const auto &name : schema_names) {
			auto &schema_entry = GetOrCreateSchemaEntry(name);
			callback(schema_entry);
		}
		return;
	}

	// T015-T016 (FR-003): Use ConnectionProvider for transaction-aware connection acquisition
	if (!connection_pool_) {
		throw InternalException("Connection pool not initialized");
	}

	// Use ConnectionProvider for proper transaction handling
	auto connection = ConnectionProvider::GetConnection(context, *this);
	if (!connection) {
		throw IOException("Failed to acquire connection for schema scan");
	}

	schema_names = metadata_cache_->GetSchemaNames(*connection);

	// Release connection properly (no-op if pinned to transaction)
	ConnectionProvider::ReleaseConnection(context, *this, std::move(connection));

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

optional_ptr<CatalogEntry> MSSQLCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	CheckWriteAccess("CREATE SCHEMA");

	// Generate T-SQL for CREATE SCHEMA
	string tsql = MSSQLDDLTranslator::TranslateCreateSchema(info.schema);

	// Execute DDL on SQL Server
	if (transaction.HasContext()) {
		ExecuteDDL(transaction.GetContext(), tsql);
	} else {
		throw InternalException("Cannot execute CREATE SCHEMA without client context");
	}

	// Point invalidation: invalidate schema list so new schema is visible
	metadata_cache_->InvalidateAll();

	return &GetOrCreateSchemaEntry(info.schema);
}

void MSSQLCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	CheckWriteAccess("DROP SCHEMA");

	// Generate T-SQL for DROP SCHEMA
	string tsql = MSSQLDDLTranslator::TranslateDropSchema(info.name);

	// Execute DDL on SQL Server
	ExecuteDDL(context, tsql);

	// Point invalidation: invalidate schema list
	metadata_cache_->InvalidateAll();

	// Remove the schema entry from our local cache
	{
		std::lock_guard<std::mutex> lock(schema_mutex_);
		schema_entries_.erase(info.name);
	}
}

//===----------------------------------------------------------------------===//
// Write Operations (all throw - read-only catalog)
//===----------------------------------------------------------------------===//

PhysicalOperator &MSSQLCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
										   optional_ptr<PhysicalOperator> plan) {
	// Check write access first (throws if read-only)
	CheckWriteAccess("INSERT");

	// Get the target table entry
	auto &table_entry = op.table.Cast<MSSQLTableEntry>();

	// Build MSSQLInsertTarget from table metadata
	MSSQLInsertTarget target;
	target.catalog_name = context_name_;
	target.schema_name = table_entry.ParentSchema().name;
	target.table_name = table_entry.name;

	// Get MSSQL column info
	auto &mssql_columns = table_entry.GetMSSQLColumns();

	// Determine which columns are being inserted
	// If no column map is specified, use all non-identity columns
	vector<idx_t> insert_col_indices;
	if (op.column_index_map.empty()) {
		// All columns - INSERT without column list
		for (idx_t i = 0; i < mssql_columns.size(); i++) {
			insert_col_indices.push_back(i);
		}
	} else {
		// Specific columns from the INSERT statement
		// The column_index_map maps physical column index -> source index in values
		// IMPORTANT: We must preserve INSERT statement column order, not table column order.
		// Build a list of (source_index, table_col_index) pairs and sort by source index.
		vector<pair<idx_t, idx_t>> col_pairs;
		for (idx_t i = 0; i < mssql_columns.size(); i++) {
			PhysicalIndex phys_idx(i);
			if (i < op.column_index_map.size()) {
				auto mapped_index = op.column_index_map[phys_idx];
				if (mapped_index != DConstants::INVALID_INDEX) {
					col_pairs.emplace_back(mapped_index, i);
				}
			}
		}
		// Sort by source index (INSERT statement order)
		std::sort(col_pairs.begin(), col_pairs.end());
		// Extract table column indices in INSERT statement order
		for (auto &pair : col_pairs) {
			insert_col_indices.push_back(pair.second);
		}
	}

	// Build column metadata for insert target
	target.has_identity_column = false;
	target.identity_column_index = 0;

	for (idx_t i = 0; i < mssql_columns.size(); i++) {
		auto &col = mssql_columns[i];
		MSSQLInsertColumn insert_col;
		insert_col.name = col.name;
		insert_col.duckdb_type = col.duckdb_type;
		insert_col.mssql_type = col.sql_type_name;
		insert_col.is_identity = false;	 // Will be detected below if needed
		insert_col.is_nullable = col.is_nullable;
		insert_col.has_default = false;	 // TODO: Query this from sys.columns
		insert_col.collation = col.collation_name;
		insert_col.precision = col.precision;
		insert_col.scale = col.scale;
		target.columns.push_back(std::move(insert_col));
	}

	// Set insert column indices
	target.insert_column_indices = std::move(insert_col_indices);

	// Handle RETURNING columns
	if (op.return_chunk) {
		// Map RETURNING columns
		for (idx_t i = 0; i < mssql_columns.size(); i++) {
			target.returning_column_indices.push_back(i);
		}
	}

	// Load insert configuration from settings
	MSSQLInsertConfig config = LoadInsertConfig(context);

	// Determine result types
	vector<LogicalType> result_types;
	if (op.return_chunk) {
		// RETURNING mode - return the inserted columns
		for (auto &col_idx : target.returning_column_indices) {
			result_types.push_back(target.columns[col_idx].duckdb_type);
		}
	} else {
		// Count mode - return BIGINT count
		result_types.push_back(LogicalType::BIGINT);
	}

	// Create the physical operator using planner.Make<T>()
	auto &physical_insert = planner.Make<MSSQLPhysicalInsert>(std::move(result_types), op.estimated_cardinality,
															  std::move(target), std::move(config), op.return_chunk);

	// Add child operator if present
	if (plan) {
		physical_insert.children.push_back(*plan);
	}

	return physical_insert;
}

PhysicalOperator &MSSQLCatalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
												  LogicalCreateTable &op, PhysicalOperator &plan) {
	// Check write access first (throws if read-only)
	CheckWriteAccess("CREATE TABLE AS");

	// Delegate to CTAS planner
	return mssql::CTASPlanner::Plan(context, planner, *this, op, plan);
}

PhysicalOperator &MSSQLCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
										   PhysicalOperator &plan) {
	// Check write access first (throws if read-only)
	CheckWriteAccess("DELETE");

	// Get the target table entry
	auto &table_entry = op.table.Cast<MSSQLTableEntry>();

	// Check if table has a primary key (required for DELETE via rowid)
	const auto &pk_info = table_entry.GetPrimaryKeyInfo(context);
	if (!pk_info.exists) {
		throw NotImplementedException("MSSQL: DELETE requires a primary key. Table '%s' has no primary key.",
									  table_entry.name);
	}

	// Build MSSQLDeleteTarget from table metadata
	MSSQLDeleteTarget target;
	target.catalog_name = context_name_;
	target.schema_name = table_entry.ParentSchema().name;
	target.table_name = table_entry.name;
	target.pk_info = pk_info;

	// Load DML configuration from settings
	MSSQLDMLConfig config = LoadDMLConfig(context);

	// Result type is BIGINT (row count)
	vector<LogicalType> result_types;
	result_types.push_back(LogicalType::BIGINT);

	// Create the physical operator using planner.Make<T>()
	auto &physical_delete =
		planner.Make<MSSQLPhysicalDelete>(std::move(result_types), op.estimated_cardinality, std::move(target), config);

	// Add child operator (provides rowid values)
	physical_delete.children.push_back(plan);

	return physical_delete;
}

PhysicalOperator &MSSQLCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
										   PhysicalOperator &plan) {
	// Check write access first (throws if read-only)
	CheckWriteAccess("UPDATE");

	// Get the target table entry
	auto &table_entry = op.table.Cast<MSSQLTableEntry>();

	// Check if table has a primary key (this will fetch PK info if not cached)
	const auto &pk_info = table_entry.GetPrimaryKeyInfo(context);
	if (!pk_info.exists) {
		throw NotImplementedException("MSSQL: UPDATE requires a primary key. Table '%s' has no primary key.",
									  table_entry.name);
	}

	// Get MSSQL column info
	auto &mssql_columns = table_entry.GetMSSQLColumns();

	// Check if any PK column is being updated (reject if so)
	for (auto &pk_col : pk_info.columns) {
		for (idx_t i = 0; i < op.columns.size(); i++) {
			auto physical_idx = op.columns[i].index;
			if (physical_idx < mssql_columns.size() && mssql_columns[physical_idx].name == pk_col.name) {
				throw NotImplementedException(
					"MSSQL: Updating primary key columns is not supported. Cannot update column '%s'.", pk_col.name);
			}
		}
	}

	// Build MSSQLUpdateTarget from table metadata
	MSSQLUpdateTarget target;
	target.catalog_name = context_name_;
	target.schema_name = table_entry.ParentSchema().name;
	target.table_name = table_entry.name;
	target.pk_info = pk_info;
	target.table_columns = mssql_columns;

	// Build update column metadata
	// The columns in op.columns are the physical indices of columns being updated
	// The values come after the rowid in the input chunk
	for (idx_t i = 0; i < op.columns.size(); i++) {
		auto physical_idx = op.columns[i].index;
		if (physical_idx >= mssql_columns.size()) {
			throw InternalException("UPDATE column index %llu out of bounds (table has %llu columns)",
									(unsigned long long)physical_idx, (unsigned long long)mssql_columns.size());
		}

		auto &col = mssql_columns[physical_idx];
		MSSQLUpdateColumn update_col;
		update_col.name = col.name;
		update_col.column_index = physical_idx;
		update_col.duckdb_type = col.duckdb_type;
		update_col.mssql_type = col.sql_type_name;
		update_col.collation = col.collation_name;
		update_col.precision = col.precision;
		update_col.scale = col.scale;
		update_col.is_nullable = col.is_nullable;
		// chunk_index: update expressions are at columns 0 to N-1, rowid is at column N (last)
		// See DuckDB bind_update.cpp: BindRowIdColumns appends rowid AFTER update expressions
		update_col.chunk_index = i;

		target.update_columns.push_back(std::move(update_col));
	}

	// Load DML configuration from settings
	MSSQLDMLConfig config = LoadDMLConfig(context);

	// Result type is BIGINT (row count)
	vector<LogicalType> result_types;
	result_types.push_back(LogicalType::BIGINT);

	// Create the physical operator using planner.Make<T>()
	auto &physical_update =
		planner.Make<MSSQLPhysicalUpdate>(std::move(result_types), op.estimated_cardinality, std::move(target), config);

	// Add child operator (provides rowid + new values)
	physical_update.children.push_back(plan);

	return physical_update;
}

unique_ptr<LogicalOperator> MSSQLCatalog::BindCreateIndex(Binder &binder, CreateStatement &stmt,
														  TableCatalogEntry &table, unique_ptr<LogicalOperator> plan) {
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
	// T023 (FR-005): Invalidate cached Azure token on detach
	// This ensures re-attach will acquire a fresh token, not use a stale cached one
	if (connection_info_ && connection_info_->use_azure_auth && !connection_info_->azure_secret_name.empty()) {
		mssql::azure::TokenCache::Instance().Invalidate(connection_info_->azure_secret_name);
	}

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

bool MSSQLCatalog::IsCatalogEnabled() const {
	return catalog_enabled_;
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
			throw CatalogException("MSSQL DDL error: SQL Server error %d: %s", result.error_number,
								   result.error_message);
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

void MSSQLCatalog::InvalidateSchemaTableSet(const string &schema_name) {
	// Invalidate the schema's table list in the metadata cache
	if (metadata_cache_) {
		metadata_cache_->InvalidateSchema(schema_name);
	}

	// Also invalidate the local schema entry's table set if it exists
	std::lock_guard<std::mutex> lock(schema_mutex_);
	auto it = schema_entries_.find(schema_name);
	if (it != schema_entries_.end()) {
		it->second->GetTableSet().Invalidate();
	}
}

void MSSQLCatalog::EnsureCacheLoaded(ClientContext &context) {
	// Check if catalog integration is disabled
	if (!catalog_enabled_) {
		throw CatalogException(
			"MSSQL catalog '%s' is attached with catalog=false (catalog disabled). "
			"Schema discovery and direct table access are not available. "
			"Use mssql_scan('%s', 'SELECT ...') or mssql_exec('%s', 'SQL') for raw queries.",
			context_name_, context_name_, context_name_);
	}

	if (!connection_pool_) {
		throw IOException("MSSQL connection pool not initialized - cannot refresh cache");
	}

	// Load cache TTL from settings and apply it
	// Lazy loading will handle actual metadata loading on first access
	int64_t cache_ttl = LoadCatalogCacheTTL(context);
	metadata_cache_->SetTTL(cache_ttl);
	metadata_cache_->SetDatabaseCollation(database_collation_);

	// Note: No eager Refresh() call - lazy loading handles this
	// Each cache level (schemas, tables, columns) loads independently on first access
}

void MSSQLCatalog::RefreshCache(ClientContext &context) {
	// Check if catalog integration is disabled
	if (!catalog_enabled_) {
		throw CatalogException(
			"MSSQL catalog '%s' is attached with catalog=false (catalog disabled). "
			"Cache refresh not available. "
			"Use mssql_scan('%s', 'SELECT ...') or mssql_exec('%s', 'SQL') for raw queries.",
			context_name_, context_name_, context_name_);
	}

	if (!connection_pool_) {
		throw IOException("MSSQL connection pool not initialized - cannot refresh cache");
	}

	// Load cache TTL from settings and apply it
	int64_t cache_ttl = LoadCatalogCacheTTL(context);
	metadata_cache_->SetTTL(cache_ttl);

	// Acquire connection for full cache refresh
	auto connection = connection_pool_->Acquire();
	if (!connection) {
		throw IOException("Failed to acquire connection for cache refresh");
	}

	// Perform full eager cache refresh
	metadata_cache_->Refresh(*connection, database_collation_);

	// Release connection
	connection_pool_->Release(std::move(connection));

	// Invalidate all schema table sets to pick up any changes
	std::lock_guard<std::mutex> lock(schema_mutex_);
	for (auto &entry : schema_entries_) {
		entry.second->GetTableSet().Invalidate();
	}
}

}  // namespace duckdb
