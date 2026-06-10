#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "catalog/mssql_catalog_filter.hpp"
#include "catalog/mssql_metadata_cache.hpp"
#include "catalog/mssql_statistics.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "mssql_storage.hpp"
#include "query/mssql_result_stream.hpp"
#include "tds/tds_connection_pool.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Forward declarations
//===----------------------------------------------------------------------===//

class MSSQLSchemaEntry;
class MSSQLStatisticsProvider;
class PhysicalPlanGenerator;
class LogicalCreateTable;
class LogicalInsert;
class LogicalDelete;
class LogicalUpdate;

//===----------------------------------------------------------------------===//
// MSSQLCatalog - DuckDB catalog representing an attached SQL Server database
//
// This catalog provides read and optional write access to SQL Server tables
// and views via the DuckDB catalog API. It integrates with the TDS connection
// pool and metadata cache for efficient query execution.
//
// Write access (DDL operations) can be disabled by attaching with READ_ONLY.
//===----------------------------------------------------------------------===//

class MSSQLCatalog : public Catalog {
public:
	// Constructor (spec 047: catalog now owns its connection pool via unique_ptr).
	// @param pool_config Pool sizing/timeout configuration translated from DuckDB settings.
	// @param fedauth_token_utf16le Pre-acquired FEDAUTH token (UTF-16LE) for Azure/manual-token
	//        auth paths; empty for SQL auth and integrated auth. Captured by the pool factory.
	// @param catalog_enabled When false, catalog integration is disabled (raw query mode only via
	// mssql_scan/mssql_exec)
	MSSQLCatalog(AttachedDatabase &db, const string &context_name, shared_ptr<MSSQLConnectionInfo> connection_info,
				 tds::PoolConfiguration pool_config, std::vector<uint8_t> fedauth_token_utf16le, AccessMode access_mode,
				 bool catalog_enabled = true);

	// noexcept (spec 047 T046k): defaulted body destructs the per-catalog
	// `unique_ptr<ConnectionPool>` (and other unique_ptr members), each of
	// whose destructors is also `noexcept`. Marked explicit for grep-ability
	// and to keep the teardown contract loud: a throw here during
	// `~AttachedDatabase` unwind would invoke std::terminate.
	~MSSQLCatalog() noexcept override;

	//===----------------------------------------------------------------------===//
	// Required Catalog Overrides
	//===----------------------------------------------------------------------===//

	void Initialize(bool load_builtin) override;

	string GetCatalogType() override;

	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
												  OnEntryNotFound if_not_found) override;

	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;

	//===----------------------------------------------------------------------===//
	// DML Planning (all throw - writes not supported)
	//===----------------------------------------------------------------------===//

	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
								 optional_ptr<PhysicalOperator> plan) override;

	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
										PhysicalOperator &plan) override;

	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
								 PhysicalOperator &plan) override;

	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
								 PhysicalOperator &plan) override;

	unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
												unique_ptr<LogicalOperator> plan) override;

	//===----------------------------------------------------------------------===//
	// Catalog Information
	//===----------------------------------------------------------------------===//

	DatabaseSize GetDatabaseSize(ClientContext &context) override;

	bool InMemory() override;

	string GetDBPath() override;

	//===----------------------------------------------------------------------===//
	// Detach Hook
	//===----------------------------------------------------------------------===//

	void OnDetach(ClientContext &context) override;

	//===----------------------------------------------------------------------===//
	// MSSQL-specific Accessors
	//===----------------------------------------------------------------------===//

	// Get connection pool for this catalog
	tds::ConnectionPool &GetConnectionPool();

	// Get metadata cache
	MSSQLMetadataCache &GetMetadataCache();

	// Get statistics provider
	MSSQLStatisticsProvider &GetStatisticsProvider();

	// Get database default collation
	const string &GetDatabaseCollation() const;

	// Get connection info
	const MSSQLConnectionInfo &GetConnectionInfo() const;

	// Get catalog filter
	const MSSQLCatalogFilter &GetCatalogFilter() const;

	// Ensure cache is loaded (refresh if needed)
	void EnsureCacheLoaded(ClientContext &context);

	// Get context name
	const string &GetContextName() const;

	//===----------------------------------------------------------------------===//
	// Result Stream Registry (spec 047 / US3 — replaces process-wide
	// MSSQLResultStreamRegistry singleton; stream lifetime is now bounded by
	// the catalog's lifetime, so DETACH or per-instance teardown drops orphans
	// automatically.)
	//===----------------------------------------------------------------------===//

	// Register a result stream produced at Bind time and return a UUID handle
	// that survives Bind→InitGlobal copies of MSSQLScanBindData (DuckDB
	// BindData must be serializable; we store the UUID string, not the raw
	// pointer).
	std::string RegisterStream(std::unique_ptr<MSSQLResultStream> stream);

	// Retrieve and remove the stream registered under `uuid`. Returns nullptr
	// when the handle is absent (already retrieved, or never registered).
	// find+erase is atomic under one lock.
	std::unique_ptr<MSSQLResultStream> RetrieveStream(const std::string &uuid);

	//===----------------------------------------------------------------------===//
	// Access Mode (READ_ONLY Support)
	//===----------------------------------------------------------------------===//

	// Check if catalog is attached in read-only mode
	bool IsReadOnly() const;

	// Check write access and throw if read-only
	// Call this at the start of any DDL operation or mssql_exec
	// @param operation_name Optional operation name for error message
	void CheckWriteAccess(const char *operation_name = nullptr) const;

	// Get access mode
	AccessMode GetAccessMode() const;

	// Check if catalog integration is enabled
	// When false, only mssql_scan() and mssql_exec() can be used
	bool IsCatalogEnabled() const;

	//===----------------------------------------------------------------------===//
	// DDL Execution
	//===----------------------------------------------------------------------===//

	// Execute a DDL statement on SQL Server
	// @param context Client context
	// @param tsql T-SQL statement to execute
	// @throws on SQL Server error or connection failure
	void ExecuteDDL(ClientContext &context, const string &tsql);

	// Invalidate the metadata cache (forces refresh on next access)
	void InvalidateMetadataCache();

	// Invalidate a specific schema's table set (point invalidation)
	void InvalidateSchemaTableSet(const string &schema_name);

	// Invalidate a single table (point invalidation): re-fetch this table's columns and
	// re-check its existence, while keeping every other table's cached column metadata.
	void InvalidateTableEntry(const string &schema_name, const string &table_name);

	// Perform full cache refresh (for mssql_refresh_cache())
	// Unlike EnsureCacheLoaded() which only sets TTL, this does eager full refresh
	void RefreshCache(ClientContext &context);

protected:
	//===----------------------------------------------------------------------===//
	// Protected Override (required by Catalog)
	//===----------------------------------------------------------------------===//

	void DropSchema(ClientContext &context, DropInfo &info) override;

private:
	//===----------------------------------------------------------------------===//
	// Internal Methods
	//===----------------------------------------------------------------------===//

	// Get or create schema entry.
	// Spec 052 (Option D): shared variant returns the shared_ptr (used by
	// LookupSchema/ScanSchemas to anchor in MSSQLBindAnchors); reference
	// variant wraps for callers that don't need to anchor.
	shared_ptr<MSSQLSchemaEntry> GetOrCreateSchemaEntryShared(const string &schema_name);
	MSSQLSchemaEntry &GetOrCreateSchemaEntry(const string &schema_name);

	// Query database default collation
	void QueryDatabaseCollation();

	//===----------------------------------------------------------------------===//
	// Member Variables
	//===----------------------------------------------------------------------===//

	string context_name_;									   // Attached context name
	shared_ptr<MSSQLConnectionInfo> connection_info_;		   // Connection parameters
	tds::PoolConfiguration pool_config_;					   // Pool config (spec 047)
	std::vector<uint8_t> fedauth_token_utf16le_;			   // FEDAUTH token (spec 047)
	AccessMode access_mode_;								   // READ_ONLY enforced
	bool catalog_enabled_;									   // Catalog integration enabled
	MSSQLCatalogFilter catalog_filter_;						   // Regex visibility filter
	unique_ptr<tds::ConnectionPool> connection_pool_;		   // Connection pool (per-catalog owned, spec 047)
	unique_ptr<MSSQLMetadataCache> metadata_cache_;			   // Metadata cache
	unique_ptr<MSSQLStatisticsProvider> statistics_provider_;  // Statistics provider
	string database_collation_;								   // Database default collation
	string default_schema_;									   // Default schema ("dbo")
	// Spec 052 (Option D): shared_ptr ownership for schema entries. The bind-
	// time anchor (MSSQLBindAnchors, per ClientContext, released at QueryEnd)
	// keeps entries alive across concurrent Invalidate / OnDetach. emplace-
	// only insertion guards against concurrent first-load.
	unordered_map<string, shared_ptr<MSSQLSchemaEntry>> schema_entries_;
	mutable std::mutex schema_mutex_;  // Thread-safety for schema access

	// Spec 047 / US3: per-catalog result stream registry (replaces process-wide
	// MSSQLResultStreamRegistry singleton). Streams are produced at mssql_scan
	// Bind time and consumed at InitGlobal time; the UUID handle bridges the
	// two via the serializable BindData.
	mutable std::mutex streams_mutex_;
	std::unordered_map<std::string, std::unique_ptr<MSSQLResultStream>> active_streams_;
};

}  // namespace duckdb
