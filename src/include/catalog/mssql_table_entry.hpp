#pragma once

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "catalog/mssql_column_info.hpp"
#include "catalog/mssql_metadata_cache.hpp"
#include <vector>

namespace duckdb {

//===----------------------------------------------------------------------===//
// Forward declarations
//===----------------------------------------------------------------------===//

class MSSQLSchemaEntry;
class MSSQLCatalog;

//===----------------------------------------------------------------------===//
// MSSQLTableEntry - DuckDB table entry for SQL Server table/view
//===----------------------------------------------------------------------===//

class MSSQLTableEntry : public TableCatalogEntry {
public:
	// Constructor from table metadata
	MSSQLTableEntry(Catalog &catalog, SchemaCatalogEntry &schema,
	                const MSSQLTableMetadata &metadata);

	~MSSQLTableEntry() override;

	//===----------------------------------------------------------------------===//
	// Required Overrides
	//===----------------------------------------------------------------------===//

	TableFunction GetScanFunction(ClientContext &context,
	                              unique_ptr<FunctionData> &bind_data) override;

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;

	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	void BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj,
	                           LogicalUpdate &update, ClientContext &context) override;

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

private:
	vector<MSSQLColumnInfo> mssql_columns_;   // Column metadata with collation
	MSSQLObjectType object_type_;             // TABLE or VIEW
	idx_t approx_row_count_;                  // Cardinality estimate
};

}  // namespace duckdb
