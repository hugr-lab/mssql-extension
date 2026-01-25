#pragma once

#include <string>
#include <vector>
#include "duckdb/common/types.hpp"
#include "catalog/mssql_primary_key.hpp"
#include "catalog/mssql_column_info.hpp"
#include "update/mssql_update_column.hpp"

namespace duckdb {

// PrimaryKeyInfo is in mssql namespace
using mssql::PrimaryKeyInfo;

//===----------------------------------------------------------------------===//
// MSSQLUpdateTarget - Target table metadata for UPDATE operations
//
// Contains all information needed to:
// - Generate UPDATE SQL statements
// - Map rowid to PK columns
// - Serialize values
//===----------------------------------------------------------------------===//

struct MSSQLUpdateTarget {
	//===----------------------------------------------------------------------===//
	// Table Identity
	//===----------------------------------------------------------------------===//

	// DuckDB catalog name (MSSQL attachment name)
	string catalog_name;

	// SQL Server schema name
	string schema_name;

	// SQL Server table name
	string table_name;

	//===----------------------------------------------------------------------===//
	// Primary Key Info (for rowid → PK mapping)
	//===----------------------------------------------------------------------===//

	// PK metadata from MSSQLTableEntry
	PrimaryKeyInfo pk_info;

	//===----------------------------------------------------------------------===//
	// Columns Being Updated
	//===----------------------------------------------------------------------===//

	// Columns in the SET clause
	vector<MSSQLUpdateColumn> update_columns;

	//===----------------------------------------------------------------------===//
	// All Table Columns (for type information)
	//===----------------------------------------------------------------------===//

	// Complete column list from table metadata
	vector<MSSQLColumnInfo> table_columns;

	//===----------------------------------------------------------------------===//
	// Helper Methods
	//===----------------------------------------------------------------------===//

	// Get fully qualified table name: [schema].[table]
	string GetFullyQualifiedName() const;

	// Get number of parameters per row (pk_cols + update_cols)
	idx_t GetParamsPerRow() const;

	// Check if table has scalar (single-column) PK
	bool IsScalarPK() const {
		return pk_info.IsScalar();
	}

	// Check if table has composite (multi-column) PK
	bool IsCompositePK() const {
		return pk_info.IsComposite();
	}

	// Check if table has a PK at all
	bool HasPrimaryKey() const {
		return pk_info.exists;
	}
};

}  // namespace duckdb
