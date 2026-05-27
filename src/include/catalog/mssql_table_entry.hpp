#pragma once

#include <atomic>
#include <mutex>
#include <vector>
#include "catalog/mssql_column_info.hpp"
#include "catalog/mssql_metadata_cache.hpp"
#include "catalog/mssql_primary_key.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/shared_ptr.hpp"		// duckdb::enable_shared_from_this (spec 052)
#include "duckdb/common/table_column.hpp"  // For virtual_column_map_t

namespace duckdb {

//===----------------------------------------------------------------------===//
// Forward declarations
//===----------------------------------------------------------------------===//

class MSSQLSchemaEntry;
class MSSQLCatalog;

//===----------------------------------------------------------------------===//
// MSSQLTableEntry - DuckDB table entry for SQL Server table/view
//
// Inherits enable_shared_from_this (spec 052) so GetScanFunction can anchor
// this entry into the bind data via shared_from_this() — the bind data's
// shared_ptr keeps the entry alive through query execute even if
// MSSQLTableSet::Invalidate() races concurrently.
//===----------------------------------------------------------------------===//

class MSSQLTableEntry : public TableCatalogEntry, public enable_shared_from_this<MSSQLTableEntry> {
public:
	// Constructor from table metadata
	MSSQLTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, const MSSQLTableMetadata &metadata);

	~MSSQLTableEntry() override;

	//===----------------------------------------------------------------------===//
	// Required Overrides
	//===----------------------------------------------------------------------===//

	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;

	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	void BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj, LogicalUpdate &update,
							   ClientContext &context) override;

	//===----------------------------------------------------------------------===//
	// Virtual Column Support (rowid)
	//===----------------------------------------------------------------------===//

	// Override to expose rowid virtual column with correct type based on PK
	// Note: PK info is lazy-loaded in GetScanFunction(), which is called before this
	virtual_column_map_t GetVirtualColumns() const override;

	//===----------------------------------------------------------------------===//
	// MSSQL-specific Accessors
	//===----------------------------------------------------------------------===//

	// Get MSSQL column info (includes collation)
	const vector<MSSQLColumnInfo> &GetMSSQLColumns() const;

	// Get object type (TABLE or VIEW)
	MSSQLObjectType GetObjectType() const;

	// Get approximate row count
	idx_t GetApproxRowCount() const;

	// Get parent MSSQL catalog
	MSSQLCatalog &GetMSSQLCatalog();

	// Get parent schema entry
	MSSQLSchemaEntry &GetMSSQLSchema();

	//===----------------------------------------------------------------------===//
	// Primary Key / RowId Support
	//===----------------------------------------------------------------------===//

	// Get the rowid type for this table
	// - Scalar PK: returns PK column type (e.g., INTEGER)
	// - Composite PK: returns STRUCT type
	// - No PK: throws BinderException
	// - VIEW: throws BinderException
	LogicalType GetRowIdType(ClientContext &context);

	// Check if table has a primary key (lazy loads PK info)
	bool HasPrimaryKey(ClientContext &context);

	// Get full PK metadata (lazy loads if needed)
	const mssql::PrimaryKeyInfo &GetPrimaryKeyInfo(ClientContext &context);

private:
	vector<MSSQLColumnInfo> mssql_columns_;	 // Column metadata with collation
	MSSQLObjectType object_type_;			 // TABLE or VIEW
	idx_t approx_row_count_;				 // Cardinality estimate

	// Lazy-loaded PK cache.
	// Spec 052 EnsurePKLoaded race fix: pk_load_mutex_ serialises concurrent
	// callers. Without it, two threads both saw the load flag false and both
	// did `pk_info_ = Discover(...)`, double-freeing the loser's previous
	// `vector<PKColumnInfo>`. Caught by ASan during spec 052 scenario-5
	// stress.
	//
	// pk_loaded_ is a separate atomic (hoisted out of PrimaryKeyInfo so the
	// struct remains move-assignable). The fast path in EnsurePKLoaded and
	// the publication check in GetVirtualColumns use load(acquire) /
	// store(release) so a reader observing pk_loaded_ == true is guaranteed
	// to see the fully-published pk_info_ assigned under the mutex.
	mutable std::atomic<bool> pk_loaded_{false};
	mutable std::mutex pk_load_mutex_;
	mutable mssql::PrimaryKeyInfo pk_info_;

	// Ensure PK info is loaded
	void EnsurePKLoaded(ClientContext &context) const;
};

}  // namespace duckdb
