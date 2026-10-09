#pragma once

// Spec 080 D3, rung 3: a table with no usable key is keyed by every column.
//
// DuckDB's binder appends the columns a table names in GetRowIdColumns() to an
// UPDATE / DELETE / MERGE projection, and looks each of them up in
// GetVirtualColumns(), which takes only ids at or above VIRTUAL_COLUMN_START
// (TableBinding refuses a physical id there). So the key travels as one hidden
// virtual column per physical column, numbered from MSSQL_KEYLESS_KEY_START,
// carrying that column's value: the scan reads it as the physical column, and
// the DML operator finds the key in the last N columns of its chunk, where a
// keyed table has its one rowid.
//
// Each hidden column is named like its physical column. A binding resolves a
// name to the physical column first (Binding::Initialize fills the name map
// before TableBinding adds the virtual ones), so the hidden columns cannot be
// referenced from SQL, and `rowid` stays refused on a keyless table.
//
// The range sits 2^32 above VIRTUAL_COLUMN_START, clear of the ids DuckDB
// itself hands out there (multi-file readers start at VIRTUAL_COLUMN_START, the
// row-id family sits at the top of the range).

#include "duckdb/common/constants.hpp"

namespace duckdb {
namespace mssql {

constexpr column_t MSSQL_KEYLESS_KEY_START = UINT64_C(9223372036854775808) + (UINT64_C(1) << 32);
constexpr column_t MSSQL_KEYLESS_KEY_END = MSSQL_KEYLESS_KEY_START + (UINT64_C(1) << 32);

inline bool IsKeylessKeyColumn(column_t column_id) {
	return column_id >= MSSQL_KEYLESS_KEY_START && column_id < MSSQL_KEYLESS_KEY_END;
}

inline column_t KeylessKeyColumn(idx_t physical_index) {
	return MSSQL_KEYLESS_KEY_START + physical_index;
}

//! The physical column a hidden key column reads; any other id is returned as is.
inline column_t ResolveKeylessKeyColumn(column_t column_id) {
	return IsKeylessKeyColumn(column_id) ? column_id - MSSQL_KEYLESS_KEY_START : column_id;
}

}  // namespace mssql
}  // namespace duckdb
