#pragma once

#include <atomic>
#include <mutex>
#include <vector>
#include "catalog/mssql_column_info.hpp"
#include "catalog/mssql_index_kind.hpp"
#include "catalog/mssql_metadata_cache.hpp"
#include "catalog/mssql_primary_key.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/shared_ptr.hpp"	   // duckdb::enable_shared_from_this (spec 052)
#include "duckdb/common/table_column.hpp"  // For virtual_column_map_t

namespace duckdb {

//===----------------------------------------------------------------------===//
// Forward declarations
//===----------------------------------------------------------------------===//

class MSSQLSchemaEntry;
class MSSQLCatalog;

//! Spec 060: does this catalog report MSSQL_VARCHAR(n) / MSSQL_NVARCHAR(n) for
//! its string columns, or a bare VARCHAR? Read from the global setting rather
//! than threaded through the metadata cache. Entries are cached, so a change
//! applies to entries built afterwards — mssql_invalidate_cache() to apply it
//! to the ones already loaded.
//!
//! Every place that hands DuckDB a type for a table column must agree with
//! this, or the two disagree behind DuckDB's back: the binder resolves
//! RETURNING against the ColumnDefinition types built here, so an INSERT whose
//! physical operator declared the plain type would hand a VARCHAR chunk to a
//! plan expecting MSSQL_NVARCHAR. That is why MSSQLCatalog::PlanInsert calls
//! this too, and not only MakeTableInfo / GetScanFunction.
bool MSSQLReportsNativeTypes(Catalog &catalog);

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

	// Since duckdb 888cd17bee ("moving ColumnList ownership out of
	// TableCatalogEntry and into DuckTableEntry") the base class no longer
	// holds the column list: it declares GetColumns() pure virtual and its
	// constructor takes nothing but the name, constraints and flags out of the
	// CreateTableInfo it is handed. Every catalog entry now owns its own list,
	// so this one does too — see columns_ below.
	const ColumnList &GetColumns() const override;

	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;

	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	void BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj, LogicalUpdate &update,
							   ClientContext &context) override;

	//===----------------------------------------------------------------------===//
	// Virtual Column Support (rowid)
	//===----------------------------------------------------------------------===//

	// Override to expose rowid virtual column with correct type based on PK
	// The key came with the metadata the entry was built from (spec 084 D5).
	virtual_column_map_t GetVirtualColumns() const override;

	// Override to gate the rowid column required by UPDATE/DELETE binding.
	// For tables without a primary key (or views) this throws a clean BinderException
	// instead of letting DuckDB's BindRowIdColumns() hit an internal assertion (issue #141).
	vector<column_t> GetRowIdColumns() const override;

	//! Spec 080 D3, rung 3: why this table cannot be keyed by every column, or
	//! empty when it can (a base table with no usable key, on a platform that
	//! takes keyless DML, whose every column round-trips exactly). Answerable
	//! from the metadata alone; the statement-shape guards (a fully pushed
	//! WHERE, no volatile function, no FROM / USING / MERGE) are plan time's.
	string KeylessKeyRefusal() const;

	//! Whether UPDATE / DELETE key this table by every column (rung 3).
	bool UsesKeylessKey() const;

private:
	//! KeylessKeyRefusal() with a leading space, or empty.
	string KeylessSuffix() const;

public:
	//===----------------------------------------------------------------------===//
	// MSSQL-specific Accessors
	//===----------------------------------------------------------------------===//

	// Get MSSQL column info (includes collation)
	const vector<MSSQLColumnInfo> &GetMSSQLColumns() const;

	// Get object type (TABLE or VIEW)
	MSSQLObjectType GetObjectType() const;

	// Get approximate row count
	idx_t GetApproxRowCount() const;

	//! Spec 080 W3: an autocommit DELETE through the catalog removed `rows`
	//! rows; the planner's estimate follows (never down to 0, which reads as
	//! "unknown"), and the statistics cache's count for the table is dropped.
	//! Not called inside a transaction: the shared entry holds committed state.
	void NoteRowsDeleted(idx_t rows);

	//! Physical shape of the object (heap / clustered rowstore / clustered
	//! columnstore), from the catalog's metadata query (spec 049). What the
	//! bulk-load TABLOCK policy and the columnstore warm-up gate read for an
	//! INSERT (spec 062 W2), with no query of their own.
	MSSQLIndexKind GetIndexKind() const {
		return index_kind_;
	}

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

	// Check if table has a primary key
	bool HasPrimaryKey(ClientContext &context);

	// Get full PK metadata
	const mssql::RowIdKeyInfo &GetPrimaryKeyInfo(ClientContext &context);

	//! The key if it is already loaded (with the table's metadata since spec
	//! 076), else null -- never a round trip (spec 079 PR E1's gain check).
	const mssql::RowIdKeyInfo *LoadedPrimaryKeyInfo() const {
		return pk_loaded_.load(std::memory_order_acquire) ? &pk_info_ : nullptr;
	}

private:
	// The DuckDB-visible columns. Owned here since the base stopped owning
	// them; GetInfo() and the whole binder read the table's shape through
	// GetColumns(), so this is what makes the entry a table at all.
	ColumnList columns_;

	vector<MSSQLColumnInfo> mssql_columns_;	 // Column metadata with collation
	MSSQLObjectType object_type_;			 // TABLE or VIEW
	//! Cardinality estimate; atomic since a DELETE adjusts it (spec 080 W3)
	//! while other sessions plan against it.
	std::atomic<idx_t> approx_row_count_;
	MSSQLIndexKind index_kind_;	 // Physical shape (spec 049)

	// The rowid key, set by the constructor from the metadata it is built from
	// and never written again (spec 084 D5: every load that publishes a
	// table's columns carries its key -- the single-table batch and the bulk
	// loads -- so nothing discovers it later). pk_loaded_ is the release /
	// acquire publication flag its lock-free readers check (spec 052).
	mutable std::atomic<bool> pk_loaded_{false};
	mutable mssql::RowIdKeyInfo pk_info_;
};

}  // namespace duckdb
