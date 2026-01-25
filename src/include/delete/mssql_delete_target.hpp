//===----------------------------------------------------------------------===//
//                         DuckDB
//
// mssql_delete_target.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <vector>
#include "catalog/mssql_primary_key.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {

// PrimaryKeyInfo is in mssql namespace
using mssql::PrimaryKeyInfo;

//===----------------------------------------------------------------------===//
// MSSQLDeleteTarget - Target table metadata for DELETE operations
//
// Contains all information needed to:
// - Generate DELETE SQL statements
// - Map rowid to PK columns
//===----------------------------------------------------------------------===//

struct MSSQLDeleteTarget {
	//===----------------------------------------------------------------------===//
	// Table Identity
	//===----------------------------------------------------------------------===//

	//! DuckDB catalog name (MSSQL attachment name) - used for pool lookup
	string catalog_name;

	//! SQL Server schema name (e.g., "dbo")
	string schema_name;

	//! SQL Server table name
	string table_name;

	//===----------------------------------------------------------------------===//
	// Primary Key Info (for rowid → PK mapping)
	//===----------------------------------------------------------------------===//

	//! PK metadata from MSSQLTableEntry
	PrimaryKeyInfo pk_info;

	//===----------------------------------------------------------------------===//
	// Helper Methods
	//===----------------------------------------------------------------------===//

	//! Get fully qualified table name: [schema].[table]
	string GetFullyQualifiedName() const;

	//! Get number of parameters per row (pk column count)
	idx_t GetParamsPerRow() const;

	//! Check if table has scalar (single-column) PK
	bool IsScalarPK() const {
		return pk_info.IsScalar();
	}

	//! Check if table has composite (multi-column) PK
	bool IsCompositePK() const {
		return pk_info.IsComposite();
	}

	//! Check if table has a PK at all
	bool HasPrimaryKey() const {
		return pk_info.exists;
	}
};

}  // namespace duckdb
