#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "catalog/mssql_metadata_cache.hpp"
#include "tds/tds_connection_pool.hpp"
#include "mssql_storage.hpp"
#include <memory>
#include <unordered_map>

namespace duckdb {

//===----------------------------------------------------------------------===//
// Forward declarations
//===----------------------------------------------------------------------===//

class MSSQLSchemaEntry;
class PhysicalPlanGenerator;
class LogicalCreateTable;
class LogicalInsert;
class LogicalDelete;
class LogicalUpdate;

//===----------------------------------------------------------------------===//
// MSSQLCatalog - DuckDB catalog representing an attached SQL Server database
//
// This catalog provides read-only access to SQL Server tables and views via
// the DuckDB catalog API. It integrates with the TDS connection pool and
// metadata cache for efficient query execution.
//===----------------------------------------------------------------------===//

class MSSQLCatalog : public Catalog {
public:
	// Constructor
	MSSQLCatalog(AttachedDatabase &db, const string &context_name,
	             shared_ptr<MSSQLConnectionInfo> connection_info,
	             AccessMode access_mode);

	~MSSQLCatalog() override;

	//===----------------------------------------------------------------------===//
	// Required Catalog Overrides
	//===----------------------------------------------------------------------===//

	void Initialize(bool load_builtin) override;

	string GetCatalogType() override;

	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction,
	                                              const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override;

	void ScanSchemas(ClientContext &context,
	                 std::function<void(SchemaCatalogEntry &)> callback) override;

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction,
	                                        CreateSchemaInfo &info) override;

	//===----------------------------------------------------------------------===//
	// DML Planning (all throw - writes not supported)
	//===----------------------------------------------------------------------===//

	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner,
	                             LogicalInsert &op, optional_ptr<PhysicalOperator> plan) override;

	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
	                                    LogicalCreateTable &op, PhysicalOperator &plan) override;

	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner,
	                             LogicalDelete &op, PhysicalOperator &plan) override;

	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner,
	                             LogicalUpdate &op, PhysicalOperator &plan) override;

	unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt,
	                                            TableCatalogEntry &table,
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

	// Get database default collation
	const string &GetDatabaseCollation() const;

	// Get connection info
	const MSSQLConnectionInfo &GetConnectionInfo() const;

	// Ensure cache is loaded (refresh if needed)
	void EnsureCacheLoaded(ClientContext &context);

	// Get context name
	const string &GetContextName() const;

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

	string context_name_;                                              // Attached context name
	shared_ptr<MSSQLConnectionInfo> connection_info_;                  // Connection parameters
	AccessMode access_mode_;                                           // READ_ONLY enforced
	shared_ptr<tds::ConnectionPool> connection_pool_;                  // Connection pool
	unique_ptr<MSSQLMetadataCache> metadata_cache_;                    // Metadata cache
	string database_collation_;                                        // Database default collation
	string default_schema_;                                            // Default schema ("dbo")
	unordered_map<string, unique_ptr<MSSQLSchemaEntry>> schema_entries_;  // Schema entry cache
	mutable std::mutex schema_mutex_;                                  // Thread-safety for schema access
};

}  // namespace duckdb
