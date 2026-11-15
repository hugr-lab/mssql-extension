#pragma once

#include <vector>
#include "catalog/mssql_primary_key.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb {

//! Issue #559: throws for a rowid component read as NULL -- a key column is NOT NULL, so the value
//! did not fit its read type (a datetime2(7) outside 1677-2262 as TIMESTAMP_NS). Names the column.
[[noreturn]] void RefuseNullKeyValue(const string &column);
//! RefuseNullKeyValue's message, for a caller that must fail through its own path (the staged DML).
string NullKeyValueMessage(const string &column);

//===----------------------------------------------------------------------===//
// Rowid-to-PK Extraction Utilities
//
// These utilities extract primary key values from DuckDB rowid columns.
// For tables with PK:
//   - Scalar PK (single column): rowid is the PK value directly
//   - Composite PK (multiple columns): rowid is STRUCT with PK fields
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// ExtractPKFromRowid - Extract PK values from rowid column
//
// Extracts primary key component values from a rowid vector.
// Returns a vector of Value objects for each row, where each inner vector
// contains the PK column values in order.
//
// @param rowid_vector The rowid column from DuckDB DataChunk
// @param count Number of rows to extract
// @param pk_info Primary key metadata (for composite PK field order)
// @return Vector of vectors: outer[row_idx] = inner[pk_col_idx] = value
//===----------------------------------------------------------------------===//

vector<vector<Value>> ExtractPKFromRowid(Vector &rowid_vector, idx_t count, const mssql::RowIdKeyInfo &pk_info);

//===----------------------------------------------------------------------===//
// ExtractSingleRowPK - Extract PK values for a single row
//
// @param rowid_vector The rowid column
// @param row_idx Index of the row to extract
// @param pk_info Primary key metadata
// @return Vector of PK column values for this row
//===----------------------------------------------------------------------===//

vector<Value> ExtractSingleRowPK(Vector &rowid_vector, idx_t row_idx, const mssql::RowIdKeyInfo &pk_info);

//===----------------------------------------------------------------------===//
// GetPKValueAsString - Convert PK value to T-SQL literal string
//
// Used for debug logging and direct SQL generation.
// Uses existing MSSQLValueSerializer for type conversion.
//
// @param value The PK value
// @param duckdb_type The DuckDB type of the value
// @return T-SQL literal string representation
//===----------------------------------------------------------------------===//

string GetPKValueAsString(const Value &value, const LogicalType &duckdb_type);

}  // namespace duckdb
