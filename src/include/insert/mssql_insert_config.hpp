#pragma once

#include "duckdb/common/types.hpp"
#include <algorithm>
#include <cstddef>

namespace duckdb {

//===----------------------------------------------------------------------===//
// Default Values for INSERT Settings
//===----------------------------------------------------------------------===//

// Default batch size (rows per INSERT statement)
constexpr idx_t MSSQL_DEFAULT_INSERT_BATCH_SIZE = 2000;

// Default maximum rows per INSERT statement (hard cap)
constexpr idx_t MSSQL_DEFAULT_INSERT_MAX_ROWS_PER_STATEMENT = 2000;

// Default maximum SQL statement size in bytes (8MB)
constexpr idx_t MSSQL_DEFAULT_INSERT_MAX_SQL_BYTES = 8388608;

// Default: use OUTPUT INSERTED for RETURNING clause
constexpr bool MSSQL_DEFAULT_INSERT_USE_RETURNING_OUTPUT = true;

// Minimum allowed max_sql_bytes (1KB)
constexpr idx_t MSSQL_MIN_INSERT_SQL_BYTES = 1024;

//===----------------------------------------------------------------------===//
// MSSQLInsertConfig - Configuration for INSERT operations
//
// Loaded from DuckDB settings at runtime via GetInsertConfig().
// Controls batching behavior, size limits, and RETURNING mode.
//===----------------------------------------------------------------------===//

struct MSSQLInsertConfig {
	// Maximum rows per INSERT statement (user-specified batch size)
	idx_t batch_size = MSSQL_DEFAULT_INSERT_BATCH_SIZE;

	// Hard cap on rows per INSERT statement
	idx_t max_rows_per_statement = MSSQL_DEFAULT_INSERT_MAX_ROWS_PER_STATEMENT;

	// Maximum SQL statement size in bytes
	idx_t max_sql_bytes = MSSQL_DEFAULT_INSERT_MAX_SQL_BYTES;

	// Use OUTPUT INSERTED for RETURNING clause
	bool use_returning_output = MSSQL_DEFAULT_INSERT_USE_RETURNING_OUTPUT;

	//===----------------------------------------------------------------------===//
	// Derived Values
	//===----------------------------------------------------------------------===//

	// Get effective rows per statement (minimum of batch_size and max_rows_per_statement)
	idx_t EffectiveRowsPerStatement() const {
		return std::min(batch_size, max_rows_per_statement);
	}

	//===----------------------------------------------------------------------===//
	// Validation
	//===----------------------------------------------------------------------===//

	// Validate configuration values
	// @throws InvalidInputException if values are out of range
	void Validate() const;
};

//===----------------------------------------------------------------------===//
// MSSQLInsertColumn - Column metadata for INSERT operations
//
// Contains information needed to serialize values and generate SQL.
// Extended from MSSQLColumnInfo with INSERT-specific flags.
//===----------------------------------------------------------------------===//

struct MSSQLInsertColumn {
	// Column name (for SQL generation)
	string name;

	// DuckDB logical type (for value serialization)
	LogicalType duckdb_type;

	// SQL Server type name (for reference/debugging)
	string mssql_type;

	// Column flags
	bool is_identity;     // IDENTITY column (auto-generated)
	bool is_nullable;     // Allows NULL values
	bool has_default;     // Has DEFAULT constraint

	// Collation name (for text types, may be empty)
	string collation;

	// Precision/scale for DECIMAL types
	uint8_t precision;
	uint8_t scale;

	// Default constructor
	MSSQLInsertColumn()
	    : is_identity(false), is_nullable(true), has_default(false),
	      precision(0), scale(0) {}

	// Full constructor
	MSSQLInsertColumn(const string &name, LogicalType duckdb_type, const string &mssql_type,
	                  bool is_identity, bool is_nullable, bool has_default,
	                  const string &collation, uint8_t precision, uint8_t scale)
	    : name(name), duckdb_type(std::move(duckdb_type)), mssql_type(mssql_type),
	      is_identity(is_identity), is_nullable(is_nullable), has_default(has_default),
	      collation(collation), precision(precision), scale(scale) {}
};

}  // namespace duckdb
