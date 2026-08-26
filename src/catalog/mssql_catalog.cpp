#include "catalog/mssql_catalog.hpp"
#include <openssl/crypto.h>
#include "codec/target_string_type.hpp"

#include "azure/azure_token.hpp"
#include "catalog/mssql_bind_anchors.hpp"
#include "catalog/mssql_ddl_translator.hpp"
#include "catalog/mssql_schema_entry.hpp"
#include "catalog/mssql_statistics.hpp"
#include "catalog/mssql_table_entry.hpp"
#include "connection/mssql_connection_provider.hpp"
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
#include "duckdb/common/types/uuid.hpp"
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
#include "tds/auth/auth_strategy_factory.hpp"

#include <cstdio>

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
						   shared_ptr<MSSQLConnectionInfo> connection_info, tds::PoolConfiguration pool_config,
						   std::vector<uint8_t> fedauth_token_utf16le, AccessMode access_mode, bool catalog_enabled)
	: Catalog(db),
	  context_name_(context_name),
	  connection_info_(std::move(connection_info)),
	  pool_config_(std::move(pool_config)),
	  fedauth_token_utf16le_(std::move(fedauth_token_utf16le)),
	  access_mode_(access_mode),
	  catalog_enabled_(catalog_enabled),
	  default_schema_("dbo") {
	// Create metadata cache with TTL from settings (0 = manual refresh only)
	int64_t cache_ttl = 0;	// Default: manual refresh only
	metadata_cache_ = make_uniq<MSSQLMetadataCache>(cache_ttl);

	// Configure catalog visibility filters from connection info (Spec 033)
	if (!connection_info_->schema_filter.empty()) {
		catalog_filter_.SetSchemaFilter(connection_info_->schema_filter);
	}
	if (!connection_info_->table_filter.empty()) {
		catalog_filter_.SetTableFilter(connection_info_->table_filter);
	}
	if (catalog_filter_.HasFilters()) {
		metadata_cache_->SetFilter(&catalog_filter_);
	}

	// Create statistics provider with default TTL (will be configured from settings later)
	statistics_provider_ = make_uniq<MSSQLStatisticsProvider>();
}

MSSQLCatalog::~MSSQLCatalog() noexcept {
	// Wipe the cached FEDAUTH token (UTF-16LE bytes) on catalog teardown so
	// the bearer credential does not linger in heap-recycled memory. Same
	// rationale as MSSQLConnectionInfo::~MSSQLConnectionInfo — use
	// OPENSSL_cleanse to defeat dead-store elimination.
	if (!fedauth_token_utf16le_.empty()) {
		OPENSSL_cleanse(fedauth_token_utf16le_.data(), fedauth_token_utf16le_.size());
	}
}

//===----------------------------------------------------------------------===//
// Result Stream Registry (spec 047 / US3)
//===----------------------------------------------------------------------===//

std::string MSSQLCatalog::RegisterStream(std::unique_ptr<MSSQLResultStream> stream) {
	// PR #118 review L3: UUID::GenerateRandomUUID is backed by std::mt19937
	// in DuckDB (NOT a CSPRNG). Acceptable here because the registry is
	// per-catalog and in-process; an attacker who could brute-force a stream
	// ID is already executing in the same process and has access to the
	// catalog directly. Replace with a CSPRNG only if the registry ever
	// becomes externally addressable.
	auto uuid = UUID::ToString(UUID::GenerateRandomUUID());
	std::lock_guard<std::mutex> lock(streams_mutex_);
	active_streams_.emplace(uuid, std::move(stream));
	return uuid;
}

std::unique_ptr<MSSQLResultStream> MSSQLCatalog::RetrieveStream(const std::string &uuid) {
	std::lock_guard<std::mutex> lock(streams_mutex_);
	auto it = active_streams_.find(uuid);
	if (it == active_streams_.end()) {
		return nullptr;
	}
	auto stream = std::move(it->second);
	active_streams_.erase(it);
	return stream;
}

//===----------------------------------------------------------------------===//
// Initialization
//===----------------------------------------------------------------------===//

void MSSQLCatalog::Initialize(bool load_builtin) {
	// Spec 047: build the connection pool inline (per-catalog ownership).
	// Replaces the MssqlPoolManager singleton lookup that lived here before.
	// The pool is owned by this catalog and torn down via unique_ptr in the
	// catalog destructor — no singleton, no cross-instance sharing.
	// Spec 047 FR-014: resolve the LOGIN7 program_name once at factory build
	// time; the same value is captured by each closure variant below so every
	// connection refilled into the pool advertises the same APP_NAME().
	const std::string app_name = ResolveAppName(*connection_info_);

	tds::ConnectionFactory factory;
	switch (connection_info_->auth_method) {
	case AuthMethod::AZURE_AD:
	case AuthMethod::MANUAL_TOKEN: {
		// FEDAUTH path: token was pre-built by MSSQLAttach (acquire happens
		// there so credential errors surface before catalog construction).
		// Captured by value in the factory closure; same lifetime as the pool.
		auto host = connection_info_->host;
		auto port = connection_info_->port;
		auto database = connection_info_->database;
		auto encrypt = connection_info_->use_encrypt;
		auto token = fedauth_token_utf16le_;
		auto tds_packet_size = connection_info_->tds_packet_size;
		auto utf8_support = connection_info_->utf8_support;
		factory = [host, port, database, encrypt, token, app_name, tds_packet_size,
				   utf8_support]() -> std::shared_ptr<tds::TdsConnection> {
			auto conn = std::make_shared<tds::TdsConnection>();
			conn->SetRequestedPacketSize(tds_packet_size);
			conn->SetRequestUtf8Support(utf8_support);
			if (!conn->Connect(host, port)) {
				return nullptr;
			}
			if (!conn->AuthenticateWithFedAuth(database, token, encrypt, app_name)) {
				return nullptr;
			}
			return conn;
		};
		break;
	}
	case AuthMethod::KRB5:
	case AuthMethod::WINSSPI: {
		// Integrated-auth path: build a fresh authenticator per connection so
		// gss_init_sec_context state is independent across pool refills and a
		// kinit-refreshed ticket is picked up on the next fill. (Spec 042.)
		MSSQLConnectionInfo info_copy = *connection_info_;
		factory = [info_copy, app_name]() -> std::shared_ptr<tds::TdsConnection> {
			auto conn = std::make_shared<tds::TdsConnection>();
			conn->SetRequestedPacketSize(info_copy.tds_packet_size);
			conn->SetRequestUtf8Support(info_copy.utf8_support);
			if (!conn->Connect(info_copy.host, info_copy.port)) {
				fprintf(stderr, "[MSSQL POOL] integrated-auth: TCP connect to %s:%u failed: %s\n",
						info_copy.host.c_str(), static_cast<unsigned>(info_copy.port), conn->GetLastError().c_str());
				return nullptr;
			}
			// Spec 068 D3: a factory, not an instance. It is called once per
			// login attempt, so a routing hop gets a ticket for the ROUTED
			// host's SPN instead of a retry of the gateway's. The first call
			// receives this connection's original host/port, which is what the
			// pre-068 code built the authenticator from — so a non-routed login
			// is unchanged. `DeriveSpn` reads info.host/info.port, and honours
			// an explicit service_principal_name verbatim, so the override
			// survives hops with no extra handling here.
			auto auth_factory = [info_copy](const std::string &host,
											uint16_t port) -> std::shared_ptr<tds::IAuthenticator> {
				MSSQLConnectionInfo hop_info = info_copy;
				hop_info.host = host;
				hop_info.port = port;
				std::shared_ptr<tds::AuthenticationStrategy> strategy;
				try {
					strategy = tds::AuthStrategyFactory::Create(hop_info);
				} catch (const std::exception &e) {
					fprintf(stderr, "[MSSQL POOL] integrated-auth: AuthStrategyFactory::Create failed: %s\n", e.what());
					return nullptr;
				}
				if (!strategy) {
					fprintf(stderr, "[MSSQL POOL] integrated-auth: AuthStrategyFactory returned null strategy\n");
					return nullptr;
				}
				auto authenticator = strategy->GetAuthenticator();
				if (!authenticator) {
					fprintf(stderr, "[MSSQL POOL] integrated-auth: strategy provided no authenticator\n");
				}
				return authenticator;
			};
			if (!conn->AuthenticateIntegrated(info_copy.database, auth_factory, info_copy.use_encrypt, app_name,
											  info_copy.login7_max_packet)) {
				fprintf(stderr, "[MSSQL POOL] integrated-auth: %s\n", conn->GetLastError().c_str());
				return nullptr;
			}
			return conn;
		};
		break;
	}
	case AuthMethod::SQL:
	default: {
		// SQL Server username/password.
		auto host = connection_info_->host;
		auto port = connection_info_->port;
		auto username = connection_info_->user;
		auto password = connection_info_->password;
		auto database = connection_info_->database;
		auto encrypt = connection_info_->use_encrypt;
		auto tds_packet_size = connection_info_->tds_packet_size;
		auto utf8_support = connection_info_->utf8_support;
		factory = [host, port, username, password, database, encrypt, app_name, tds_packet_size,
				   utf8_support]() -> std::shared_ptr<tds::TdsConnection> {
			auto conn = std::make_shared<tds::TdsConnection>();
			conn->SetRequestedPacketSize(tds_packet_size);
			conn->SetRequestUtf8Support(utf8_support);
			if (!conn->Connect(host, port)) {
				return nullptr;
			}
			if (!conn->Authenticate(username, password, database, encrypt, app_name)) {
				return nullptr;
			}
			return conn;
		};
		break;
	}
	}

	connection_pool_ = make_shared_ptr<tds::ConnectionPool>(context_name_, pool_config_, std::move(factory));

	// Skip metadata initialization when catalog integration is disabled
	// (mssql_scan/mssql_exec will still work via raw queries)
	if (!catalog_enabled_) {
		return;
	}

	// Query database collation (needed for column metadata)
	QueryDatabaseCollation();
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

	// Check schema filter — filtered-out schemas return not found (Spec 033)
	if (catalog_filter_.HasSchemaFilter() && !catalog_filter_.MatchesSchema(name)) {
		if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw CatalogException("Schema '%s' not found in MSSQL database", name);
		}
		return nullptr;
	}

	// T035 (FR-003/Bug 0.2): Check cache BEFORE acquiring connection to reduce connection usage
	// Fast path: If schemas are already loaded and schema exists in cache, skip connection acquisition
	if (metadata_cache_->GetSchemasState() == CacheLoadState::LOADED && metadata_cache_->HasSchema(name)) {
		auto schema_sp = GetOrCreateSchemaEntryShared(name);
		if (transaction.context) {
			MSSQLBindAnchors::For(*transaction.context, *this).AnchorSchema(schema_sp);
		}
		return schema_sp.get();
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

	// Trigger lazy loading of schema list (ensure connection released on exception)
	try {
		metadata_cache_->EnsureSchemasLoaded(*connection);
	} catch (...) {
		if (transaction.context) {
			ConnectionProvider::ReleaseConnection(*transaction.context, *this, std::move(connection));
		} else {
			connection_pool_->Release(std::move(connection));
		}
		throw;
	}

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
	auto schema_sp = GetOrCreateSchemaEntryShared(name);
	if (transaction.context) {
		MSSQLBindAnchors::For(*transaction.context, *this).AnchorSchema(schema_sp);
	}
	return schema_sp.get();
}

void MSSQLCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	// Ensure cache is loaded (sets TTL)
	EnsureCacheLoaded(context);

	// T036 (FR-003/Bug 0.2): Check cache BEFORE acquiring connection
	// Fast path: If schemas are already loaded, get names without acquiring connection
	vector<string> schema_names;
	if (metadata_cache_->TryGetCachedSchemaNames(schema_names)) {
		// Cache hit - iterate without connection.
		// Spec 052 (Option D): anchor each schema entry so it survives a
		// concurrent Invalidate between DuckDB walker phase 1 (collect) and
		// phase 2 (read). Same reasoning as MSSQLTableSet::Scan.
		for (const auto &name : schema_names) {
			auto schema_sp = GetOrCreateSchemaEntryShared(name);
			MSSQLBindAnchors::For(context, *this).AnchorSchema(schema_sp);
			callback(*schema_sp);
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

	try {
		schema_names = metadata_cache_->GetSchemaNames(*connection);
	} catch (...) {
		ConnectionProvider::ReleaseConnection(context, *this, std::move(connection));
		throw;
	}

	// Release connection properly (no-op if pinned to transaction)
	ConnectionProvider::ReleaseConnection(context, *this, std::move(connection));

	for (const auto &name : schema_names) {
		auto schema_sp = GetOrCreateSchemaEntryShared(name);
		MSSQLBindAnchors::For(context, *this).AnchorSchema(schema_sp);
		callback(*schema_sp);
	}
}

shared_ptr<MSSQLSchemaEntry> MSSQLCatalog::GetOrCreateSchemaEntryShared(const string &schema_name) {
	std::lock_guard<std::mutex> lock(schema_mutex_);

	auto it = schema_entries_.find(schema_name);
	if (it != schema_entries_.end()) {
		return it->second;	// shared_ptr copy — refcount inc
	}

	// Spec 052: construct via make_shared_ptr — enable_shared_from_this on
	// MSSQLSchemaEntry requires shared_ptr ownership from the first store.
	// schema_mutex_ is held across find + emplace above, so no race is
	// possible here; the emplace simply publishes the freshly constructed
	// entry.
	auto entry = make_shared_ptr<MSSQLSchemaEntry>(*this, schema_name);
	auto insert_result = schema_entries_.emplace(schema_name, std::move(entry));
	return insert_result.first->second;
}

MSSQLSchemaEntry &MSSQLCatalog::GetOrCreateSchemaEntry(const string &schema_name) {
	// Reference-returning wrapper for internal call-sites that don't need to
	// anchor (DDL paths that use the entry briefly and discard).
	return *GetOrCreateSchemaEntryShared(schema_name);
}

optional_ptr<CatalogEntry> MSSQLCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	CheckWriteAccess("CREATE SCHEMA");

	if (!transaction.HasContext()) {
		throw InternalException("Cannot execute CREATE SCHEMA without client context");
	}

	// Handle IF NOT EXISTS: check if schema already exists (Issue #54)
	if (info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
		EntryLookupInfo lookup(CatalogType::SCHEMA_ENTRY, info.SchemaName());
		auto existing = LookupSchema(transaction, lookup, OnEntryNotFound::RETURN_NULL);
		if (existing) {
			return existing.get();
		}
	}

	// Generate T-SQL for CREATE SCHEMA
	string tsql = MSSQLDDLTranslator::TranslateCreateSchema(info.SchemaName().GetIdentifierName());

	// Execute DDL on SQL Server
	ExecuteDDL(transaction.GetContext(), tsql);

	// Point invalidation: invalidate schema list so new schema is visible
	metadata_cache_->InvalidateAll();

	return &GetOrCreateSchemaEntry(info.SchemaName().GetIdentifierName());
}

void MSSQLCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	CheckWriteAccess("DROP SCHEMA");

	// Handle IF EXISTS: check if schema exists before attempting DROP (Issue #54)
	if (info.if_not_found == OnEntryNotFound::RETURN_NULL) {
		CatalogTransaction cat_transaction = GetCatalogTransaction(context);
		EntryLookupInfo lookup(CatalogType::SCHEMA_ENTRY, info.GetQualifiedName().Name());
		auto existing = LookupSchema(cat_transaction, lookup, OnEntryNotFound::RETURN_NULL);
		if (!existing) {
			return;
		}
	}

	// Generate T-SQL for DROP SCHEMA
	string tsql = MSSQLDDLTranslator::TranslateDropSchema(info.GetQualifiedName().Name().GetIdentifierName());

	// Execute DDL on SQL Server
	ExecuteDDL(context, tsql);

	// Point invalidation: invalidate schema list
	metadata_cache_->InvalidateAll();

	// Spec 052 (Option D): just erase. Any binder that looked up this schema
	// before DROP SCHEMA fired is already anchored in its ClientContext's
	// MSSQLBindAnchors via the LookupSchema path; the schema entry stays
	// alive until that ClientContext's QueryEnd. DuckDB serializes DETACH
	// against active queries, so we don't need to worry about the catalog
	// dying mid-query.
	std::lock_guard<std::mutex> lock(schema_mutex_);
	schema_entries_.erase(info.GetQualifiedName().Name().GetIdentifierName());
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
	target.schema_name = table_entry.ParentSchema().name.GetIdentifierName();
	target.table_name = table_entry.name.GetIdentifierName();

	// Get MSSQL column info
	auto &mssql_columns = table_entry.GetMSSQLColumns();

	// Determine which columns are being inserted
	// If no column map is specified, use all non-identity columns
	vector<idx_t> insert_col_indices;
	if (op.column_index_map.empty()) {
		// 2.0 no longer populates column_index_map: the binder expands the
		// child plan to every physical column and fills the ones the INSERT
		// did not name with their bound default. The tell is the move in
		// ResolveInputProjection — an unnamed column's bound_defaults slot is
		// moved into the projection and left null, a named column's is only
		// dereferenced. Unnamed columns stay OUT of the generated column list
		// so the server applies its own identity/defaults, exactly what the
		// named-map flow produced on 1.5.x. A full-positional INSERT keeps
		// every slot non-null and inserts every column, as before.
		for (idx_t i = 0; i < mssql_columns.size(); i++) {
			const bool unnamed = i < op.bound_defaults.size() && !op.bound_defaults[i];
			if (!unnamed) {
				insert_col_indices.push_back(i);
			}
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

	// Spec 060: must match what the catalog told DuckDB about these columns —
	// the binder resolved RETURNING against those types, and MSSQLPhysicalInsert
	// references the parser's chunk straight into DuckDB's, which requires the
	// types to be equal down to the extension info.
	const bool native_types = MSSQLReportsNativeTypes(*this);

	for (idx_t i = 0; i < mssql_columns.size(); i++) {
		auto &col = mssql_columns[i];
		MSSQLInsertColumn insert_col;
		insert_col.name = col.name;
		insert_col.duckdb_type = native_types ? col.NativeDuckDBType() : col.duckdb_type;
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
	target.schema_name = table_entry.ParentSchema().name.GetIdentifierName();
	target.table_name = table_entry.name.GetIdentifierName();
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
	target.schema_name = table_entry.ParentSchema().name.GetIdentifierName();
	target.table_name = table_entry.name.GetIdentifierName();
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
	// This ensures re-attach will acquire a fresh token, not use a stale cached one.
	// Spec 047 T046b (FR-012): invalidate only this DatabaseInstance's namespace
	// so a sibling instance sharing the same secret name keeps its token.
	// PR #118 review M3: also evict tenant-suffixed variants
	// (`secret_name:tenant_a`, `secret_name:tenant_b`, ...) that the
	// interactive-auth path in AcquireToken builds — bare-name Invalidate
	// would otherwise leave those rows behind.
	if (connection_info_ && connection_info_->use_azure_auth && !connection_info_->azure_secret_name.empty()) {
		mssql::azure::TokenCache::Instance().InvalidateByPrefix(*context.db, connection_info_->azure_secret_name);
	}

	// Spec 047 T012+T020: pool teardown is implicit via ~MSSQLCatalog → unique_ptr
	// destruction; the MssqlPoolManager / MSSQLContextManager singletons that
	// used to require explicit RemovePool() / UnregisterContext() are gone.
	(void)context;
}

//===----------------------------------------------------------------------===//
// MSSQL-specific Accessors
//===----------------------------------------------------------------------===//

weak_ptr<tds::ConnectionPool> MSSQLCatalog::GetConnectionPoolHandle() const {
	return weak_ptr<tds::ConnectionPool>(connection_pool_);
}

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

MSSQLCatalog::Utf8Support MSSQLCatalog::UTF8SupportState() {
	const int8_t cached = utf8_support_acked_.load(std::memory_order_relaxed);
	if (cached >= 0) {
		return cached == 1 ? Utf8Support::Granted : Utf8Support::Declined;
	}
	// The ATTACH-time validation login already answered this on every non-lazy
	// attach, and it is carried on the connection info the same way
	// is_fabric_endpoint is.
	const int8_t from_attach = connection_info_ ? connection_info_->utf8_support_acked : -1;
	if (from_attach >= 0) {
		utf8_support_acked_.store(from_attach, std::memory_order_relaxed);
		return from_attach == 1 ? Utf8Support::Granted : Utf8Support::Declined;
	}

	// Only a lazy attach gets here: no login has happened yet at ATTACH time.
	// Borrow whatever the pool has rather than opening a connection for the
	// question. If it cannot hand one over, leave the answer unobserved rather
	// than caching a guess — the next caller retries.
	auto &pool = GetConnectionPool();
	auto conn = pool.Acquire();
	if (!conn) {
		return Utf8Support::Unknown;
	}
	const bool acked = conn->UTF8SupportAcked();
	pool.Release(conn);
	utf8_support_acked_.store(acked ? 1 : 0, std::memory_order_relaxed);
	return acked ? Utf8Support::Granted : Utf8Support::Declined;
}

string MSSQLCatalog::ResolveVarcharCollation(ClientContext &context, bool wants_varchar, bool target_is_temp) {
	if (!wants_varchar) {
		return string();
	}

	Value setting;
	string requested;
	if (context.TryGetCurrentSetting("mssql_utf8_collation", setting)) {
		requested = setting.IsNull() ? string() : setting.ToString();
	}
	if (requested.empty()) {
		// The documented way to ask for the pre-#225 behaviour deliberately.
		return string();
	}

	// A database default that is already UTF-8 (Fabric) is the case where
	// inheriting is right: imposing a Latin1 collation would also impose its
	// case- and accent-sensitivity on every later comparison against the column.
	//
	// Except for a TEMP table, which does not inherit it. A #temp lives in
	// tempdb and takes TEMPDB's collation — the server default, and typically
	// not UTF-8 even when the database is. Verified on a UTF-8 database: a temp
	// varchar column came back SQL_Latin1_General_CP1_CI_AS and 'Привет' landed
	// as '??????'. Naming the database's own collation puts the temp column back
	// in step with the permanent tables around it, and on Fabric — where every
	// string column is a varchar, so this is the whole of it — that name is one
	// of the two a warehouse accepts.
	if (StringUtil::EndsWith(StringUtil::Upper(GetDatabaseCollation()), "_UTF8")) {
		if (!target_is_temp) {
			return string();
		}
		// The name reaches T-SQL as a bare identifier. It came from the server,
		// but it is concatenated into DDL, so it is checked like any other.
		return mssql::codec::IsValidCollationName(GetDatabaseCollation()) ? GetDatabaseCollation() : requested;
	}

	// Unknown is treated as granted, NOT as declined. Declined means the server
	// has no UTF-8 collations at all and the DDL will fail with a clear message;
	// unknown means only that no connection could be borrowed to ask. Reading
	// either as "skip the collation" would turn a transient pool timeout into a
	// silently lossy table.
	if (UTF8SupportState() == Utf8Support::Declined) {
		throw NotImplementedException(
			"A VARCHAR column needs a UTF-8 collation, and this server did not grant the TDS UTF8SUPPORT feature "
			"(SQL Server 2019 introduced both). The column would take the database's code page and lose every "
			"character outside it on insert, silently. Use NVARCHAR instead, name a collation with "
			"MSSQL_VARCHAR(n, 'collation'), or set mssql_utf8_collation='' to accept that loss deliberately.");
	}
	return requested;
}

bool MSSQLCatalog::RequiresSingleByteText() const {
	return connection_info_ && connection_info_->is_fabric_endpoint;
}

string MSSQLCatalog::WireVarcharCollation(const string &ddl_collation) const {
	if (!ddl_collation.empty()) {
		return ddl_collation;
	}
	// The DDL said nothing because the database's own collation is already what
	// the column wants. The wire has no such default, so name it.
	const string &db_collation = GetDatabaseCollation();
	if (StringUtil::EndsWith(StringUtil::Upper(db_collation), "_UTF8") &&
		mssql::codec::IsValidCollationName(db_collation)) {
		return db_collation;
	}
	return string();
}

void MSSQLCatalog::ValidateStringTargets(const vector<LogicalType> &types) {
	if (!RequiresSingleByteText()) {
		return;
	}

	// Fabric Data Warehouse allows exactly these two, both UTF-8, and the choice
	// is fixed when the warehouse is created. Anything else is rejected by the
	// server with "...is not a valid collation", which is a worse place to find
	// out than here.
	static const char *const FABRIC_COLLATIONS[] = {"LATIN1_GENERAL_100_BIN2_UTF8",
													"LATIN1_GENERAL_100_CI_AS_KS_WS_SC_UTF8"};

	for (const auto &type : types) {
		mssql::codec::TargetStringType spec;
		if (!mssql::codec::TryGetTargetStringType(type, spec)) {
			continue;
		}
		if (spec.unicode) {
			throw NotImplementedException(
				"MSSQL_NVARCHAR is not available on Microsoft Fabric: a warehouse stores tables as Delta Parquet and "
				"has no UTF-16 type, so nvarchar columns cannot be created there. Use MSSQL_VARCHAR(n) — its "
				"collation is UTF-8, so it holds the same characters, counting BYTES rather than UTF-16 units.");
		}
		if (spec.collation.empty()) {
			continue;
		}
		const string upper = StringUtil::Upper(spec.collation);
		bool supported = false;
		for (const char *candidate : FABRIC_COLLATIONS) {
			if (upper == candidate) {
				supported = true;
				break;
			}
		}
		if (!supported) {
			throw NotImplementedException(
				"Collation '%s' is not available on Microsoft Fabric, which supports only "
				"Latin1_General_100_BIN2_UTF8 and Latin1_General_100_CI_AS_KS_WS_SC_UTF8 — both UTF-8, and fixed when "
				"the warehouse was created. Omit the collation to inherit the warehouse's own.",
				spec.collation);
		}
	}
}

void MSSQLCatalog::ValidateTableOptions(const MSSQLTableOptions &options) {
	if (!RequiresSingleByteText()) {
		return;
	}
	// Verified against a live warehouse: table_kind and clustered_index both come
	// back as "CREATE INDEX is not a supported statement type", data_compression
	// as "The DATA COMPRESSION keyword is not supported in the CREATE TABLE
	// statement". Delta Parquet has no indexes, and compresses itself.
	if (options.kind != MSSQLTableKind::HEAP) {
		throw NotImplementedException(
			"Microsoft Fabric stores tables as Delta Parquet and supports no indexes, so table_kind and "
			"clustered_index cannot be applied there. Omit them — a warehouse is already columnar.");
	}
	if (!options.data_compression.empty()) {
		throw NotImplementedException(
			"DATA_COMPRESSION is not available on Microsoft Fabric: a warehouse stores tables as Delta Parquet, "
			"which carries its own compression. Omit the option.");
	}
}

ErrorData MSSQLCatalog::SupportsCreateTable(BoundCreateTableInfo &info) {
	auto &base = info.Base().Cast<CreateTableInfo>();
	// PARTITIONED BY and SORTED BY stay rejected by the base implementation:
	// SQL Server expresses both, but through partition schemes and index keys
	// that this spec does not build. Only the WITH clause is claimed here.
	if (base.partition_keys.empty() && base.sort_keys.empty()) {
		return ErrorData();
	}
	return Catalog::SupportsCreateTable(info);
}

const MSSQLConnectionInfo &MSSQLCatalog::GetConnectionInfo() const {
	return *connection_info_;
}

const MSSQLCatalogFilter &MSSQLCatalog::GetCatalogFilter() const {
	return catalog_filter_;
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

	// The statistics cache too (job 1124). It was invalidated by NOTHING —
	// InvalidateAll/InvalidateTable/InvalidateSchema had no callers at all — which
	// was invisible while row counts fed only GetStorageInfo, a number the planner
	// ignores for a table function. Since the spec-070 cardinality callback they
	// ARE what the optimizer plans on, so a stale count now survives an explicit
	// mssql_invalidate_cache() and drives join order for the whole TTL.
	if (statistics_provider_) {
		statistics_provider_->InvalidateAll();
	}

	// Also clear the local schema entry cache.
	// Spec 052 (Option D): in-flight binders are anchored in their
	// ClientContext's MSSQLBindAnchors; dropping entries_ here just
	// decrements refcount.
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

	// Also invalidate the local schema entry's table set if it exists.
	// Spec 052 (Option D): MSSQLBindAnchors holds in-flight binder refs.
	std::lock_guard<std::mutex> lock(schema_mutex_);
	auto it = schema_entries_.find(schema_name);
	if (it != schema_entries_.end()) {
		it->second->GetTableSet().Invalidate();
	}
}

void MSSQLCatalog::InvalidateTableEntry(const string &schema_name, const string &table_name) {
	if (metadata_cache_) {
		// Re-fetch this table's columns (ALTER) ...
		metadata_cache_->InvalidateTable(schema_name, table_name);
		// ... and re-check the schema's table list for existence (CREATE/DROP/RENAME), but
		// WITHOUT dropping every other table's cached columns.
		metadata_cache_->InvalidateSchemaTableList(schema_name);
	}

	// Point-invalidate this table's row count as well (job 1124), so a DDL or a
	// load that changes its size is reflected in the next plan rather than after
	// the statistics TTL.
	if (statistics_provider_) {
		statistics_provider_->InvalidateTable(schema_name, table_name);
	}

	// Evict the single bound entry from the schema's table set (keeps the rest).
	std::lock_guard<std::mutex> lock(schema_mutex_);
	auto it = schema_entries_.find(schema_name);
	if (it != schema_entries_.end()) {
		it->second->GetTableSet().InvalidateEntry(table_name);
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
	metadata_cache_->SetMetadataTimeout(LoadMetadataTimeout(context));
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

	// Load cache TTL and metadata timeout from settings
	int64_t cache_ttl = LoadCatalogCacheTTL(context);
	metadata_cache_->SetTTL(cache_ttl);

	// A refresh means "forget what you think you know", so the statistics cache
	// is cleared with it — and this is the one place that can also apply the
	// documented mssql_statistics_cache_ttl_seconds, which reached the provider
	// through nothing before (SetCacheTTL had no callers either; the provider was
	// constructed with the compiled-in default and stayed there) — job 1124.
	if (statistics_provider_) {
		statistics_provider_->SetCacheTTL(LoadStatisticsCacheTTL(context));
		statistics_provider_->InvalidateAll();
	}
	metadata_cache_->SetMetadataTimeout(LoadMetadataTimeout(context));

	// Acquire connection for full cache refresh
	auto connection = connection_pool_->Acquire();
	if (!connection) {
		throw IOException("Failed to acquire connection for cache refresh");
	}

	// Perform full eager cache refresh.
	// Exception-safe: if Refresh throws (TDS hiccup under stress — 1-in-300
	// odds during scenario 5's 318 invalidations × 30s soak), the connection
	// must be returned to the pool BEFORE the exception propagates, or
	// ~ConnectionPool fires its debug-only D_ASSERT about checked-out
	// connections during catalog teardown and the process aborts on Linux.
	try {
		metadata_cache_->Refresh(*connection, database_collation_);
	} catch (...) {
		connection_pool_->Release(std::move(connection));
		throw;
	}

	// Release connection
	connection_pool_->Release(std::move(connection));

	// Invalidate all schema table sets to pick up any changes.
	// Spec 052 (Option D): in-flight binders are anchored in
	// MSSQLBindAnchors per ClientContext (released at QueryEnd).
	std::lock_guard<std::mutex> lock(schema_mutex_);
	for (auto &entry : schema_entries_) {
		entry.second->GetTableSet().Invalidate();
	}
}

}  // namespace duckdb
