#pragma once

#include <string>
#include <vector>
#include "dml/insert/mssql_insert_config.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLInsertTarget - Target table metadata for INSERT operations
//
// Contains all information needed to generate INSERT statements:
// - Fully qualified table name
// - Column metadata with INSERT-specific flags
// - Identity column tracking
// - Column indices for INSERT and RETURNING
//===----------------------------------------------------------------------===//

struct MSSQLInsertTarget {
	// Fully qualified table name components
	string catalog_name;  // Database/catalog name
	string schema_name;	  // Schema name (e.g., "dbo")
	string table_name;	  // Table name

	// All columns in the table (in ordinal order)
	vector<MSSQLInsertColumn> columns;

	// Indices into columns vector for columns being inserted
	// (excludes identity columns unless explicitly specified)
	vector<idx_t> insert_column_indices;

	// Indices into columns vector for RETURNING columns
	// (populated when RETURNING clause is used)
	vector<idx_t> returning_column_indices;

	// `mssql_convert_varchar_max`, resolved where the target is built. The OUTPUT
	// list is built by the same function as the scan's SELECT list, and that
	// function takes the setting — so a varchar(max) column is cast, or not,
	// identically on both paths.
	bool convert_varchar_max = true;

	// Identity column tracking
	bool has_identity_column;	  // Table has an IDENTITY column
	idx_t identity_column_index;  // Index of identity column in columns vector

	// Spec 077 W2: the INSERT names the identity column (explicitly, or
	// positionally with a value for every column), so the statement path has
	// to bracket its batches with SET IDENTITY_INSERT — and refuse a NULL in
	// that column before sending anything, because DuckDB hands DEFAULT and
	// NULL to us identically. Never set for a view target: SET IDENTITY_INSERT
	// takes a table, and an explicit identity value through a view is the
	// server's own refusal.
	bool identity_in_list = false;

	// Default constructor
	MSSQLInsertTarget() : has_identity_column(false), identity_column_index(0) {}

	//===----------------------------------------------------------------------===//
	// Accessors
	//===----------------------------------------------------------------------===//

	// Get fully qualified table name for SQL generation
	// Returns: [catalog].[schema].[table]
	string GetFullyQualifiedName() const;

	// Get the number of columns being inserted
	idx_t GetInsertColumnCount() const {
		return insert_column_indices.size();
	}

	// Get the number of columns in RETURNING
	idx_t GetReturningColumnCount() const {
		return returning_column_indices.size();
	}

	// Check if RETURNING is enabled
	bool HasReturning() const {
		return !returning_column_indices.empty();
	}

	// Get column by index in insert_column_indices
	const MSSQLInsertColumn &GetInsertColumn(idx_t idx) const {
		return columns[insert_column_indices[idx]];
	}

	// Get column by index in returning_column_indices
	const MSSQLInsertColumn &GetReturningColumn(idx_t idx) const {
		return columns[returning_column_indices[idx]];
	}
};

}  // namespace duckdb
