#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"

namespace duckdb {
namespace mssql {

//===----------------------------------------------------------------------===//
// CTASTarget - Target table information for CTAS operation
//===----------------------------------------------------------------------===//

struct CTASTarget {
	// Attached database name (e.g., "mssql")
	string catalog_name;

	// SQL Server schema (e.g., "dbo")
	string schema_name;

	// Table name (e.g., "new_orders")
	string table_name;

	// CREATE OR REPLACE TABLE mode
	bool or_replace = false;

	// CREATE TABLE IF NOT EXISTS mode (silently succeed if table exists)
	bool if_not_exists = false;

	// ON CONFLICT behavior from DuckDB
	OnCreateConflict on_conflict = OnCreateConflict::ERROR_ON_CONFLICT;

	// Get fully qualified table name [schema].[table]
	string GetQualifiedName() const {
		return "[" + schema_name + "].[" + table_name + "]";
	}

	// Get full three-part name catalog.schema.table
	string GetFullName() const {
		return catalog_name + "." + schema_name + "." + table_name;
	}
};

//===----------------------------------------------------------------------===//
// CTASColumnDef - Column definition derived from source query
//===----------------------------------------------------------------------===//

struct CTASColumnDef {
	// Column name (from SELECT alias or generated)
	string name;

	// DuckDB type from source query
	LogicalType duckdb_type;

	// Translated SQL Server type (e.g., "int", "nvarchar(max)")
	string mssql_type;

	// True unless source column is NOT NULL
	bool nullable = true;

	CTASColumnDef() = default;

	CTASColumnDef(string name_p, LogicalType duckdb_type_p, string mssql_type_p, bool nullable_p)
		: name(std::move(name_p)),
		  duckdb_type(std::move(duckdb_type_p)),
		  mssql_type(std::move(mssql_type_p)),
		  nullable(nullable_p) {}
};

}  // namespace mssql
}  // namespace duckdb
