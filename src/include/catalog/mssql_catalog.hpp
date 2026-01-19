#pragma once

#include <memory>
#include <unordered_map>
#include "catalog/mssql_metadata_cache.hpp"
#include "catalog/mssql_statistics.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "mssql_storage.hpp"
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
	// Constructor
	MSSQLCatalog(AttachedDatabase &db, const string &context_name, shared_ptr<MSSQLConnectionInfo> connection_info,
				 AccessMode access_mode);

	~MSSQLCatalog() override;

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

	// Ensure cache is loaded (refresh if needed)
	void EnsureCacheLoaded(ClientContext &context);

	// Get context name
	const string &GetContextName() const;

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

protected:
	//===----------------------------------------------------------------------===//
	// Protected Override (required by Catalog)
	//===----------------------------------------------------------------------===//

	void DropSchema(ClientContext &context, DropInfo &info) override;

private:
	//===----------------------------------------------------------------------===//
	// Internal Methods
	//===----------------------------------------------------------------------===//

	// Get or create schema entry
	MSSQLSchemaEntry &GetOrCreateSchemaEntry(const string &schema_name);

	// Query database default collation
	void QueryDatabaseCollation();

	// Create connection factory for the pool
	tds::ConnectionFactory CreateConnectionFactory();

	//===----------------------------------------------------------------------===//
	// Member Variables
	//===----------------------------------------------------------------------===//

	string context_name_;												  // Attached context name
	shared_ptr<MSSQLConnectionInfo> connection_info_;					  // Connection parameters
	AccessMode access_mode_;											  // READ_ONLY enforced
	shared_ptr<tds::ConnectionPool> connection_pool_;					  // Connection pool
	unique_ptr<MSSQLMetadataCache> metadata_cache_;						  // Metadata cache
	unique_ptr<MSSQLStatisticsProvider> statistics_provider_;			  // Statistics provider
	string database_collation_;											  // Database default collation
	string default_schema_;												  // Default schema ("dbo")
	unordered_map<string, unique_ptr<MSSQLSchemaEntry>> schema_entries_;  // Schema entry cache
	mutable std::mutex schema_mutex_;									  // Thread-safety for schema access
};

}  // namespace duckdb
