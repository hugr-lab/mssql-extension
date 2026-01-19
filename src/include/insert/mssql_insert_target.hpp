#pragma once

#include <string>
#include <vector>
#include "duckdb/common/types.hpp"
#include "insert/mssql_insert_config.hpp"

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

	// Identity column tracking
	bool has_identity_column;	  // Table has an IDENTITY column
	idx_t identity_column_index;  // Index of identity column in columns vector

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
