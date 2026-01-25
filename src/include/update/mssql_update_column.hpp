#pragma once

#include <string>
#include "duckdb/common/types.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLUpdateColumn - Metadata for a column being updated
//
// Contains information needed to serialize values and generate SQL.
// Similar to MSSQLInsertColumn but specific to UPDATE SET clause.
//===----------------------------------------------------------------------===//

struct MSSQLUpdateColumn {
	// Column name (for SQL generation: SET [name] = ...)
	string name;

	// Index in the target table's column list
	idx_t column_index = 0;

	// DuckDB logical type (for value serialization)
	LogicalType duckdb_type;

	// SQL Server type name (for reference/debugging)
	string mssql_type;

	// Collation name (for text types, may be empty)
	string collation;

	// Precision/scale for DECIMAL types
	uint8_t precision = 0;
	uint8_t scale = 0;

	// Allow NULL values
	bool is_nullable = true;

	// Position in input DataChunk (after rowid)
	// rowid is at index 0, first update column is at index 1
	idx_t chunk_index = 0;

	// Default constructor
	MSSQLUpdateColumn() = default;

	// Full constructor
	MSSQLUpdateColumn(const string &name, idx_t column_index, LogicalType duckdb_type, const string &mssql_type,
					  const string &collation, uint8_t precision, uint8_t scale, bool is_nullable, idx_t chunk_index)
		: name(name),
		  column_index(column_index),
		  duckdb_type(std::move(duckdb_type)),
		  mssql_type(mssql_type),
		  collation(collation),
		  precision(precision),
		  scale(scale),
		  is_nullable(is_nullable),
		  chunk_index(chunk_index) {}
};

}  // namespace duckdb
