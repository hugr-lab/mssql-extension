#pragma once

#include <string>
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLValueSerializer - Thin wrapper around the codec literal layer
//
// Public entry points (Serialize / SerializeFromVector / EstimateSerializedSize)
// dispatch every per-type case through mssql::codec::FormatSqlLiteral so the
// INSERT / UPDATE / DELETE paths share a single canonical SQL-literal renderer
// with the filter pushdown path (spec 045 — type codec consolidation).
//
// What remains here:
//   - Identifier / string escaping helpers (still T-SQL specific).
//   - SerializeDecimal: hugeint+scale → fixed-point string, kept as a public
//     helper because codec::decimal::FormatSqlLiteral delegates here for the
//     core rendering (single source of truth).
//===----------------------------------------------------------------------===//

class MSSQLValueSerializer {
public:
	//===----------------------------------------------------------------------===//
	// Main Entry Points
	//===----------------------------------------------------------------------===//

	// Serialize a DuckDB Value to T-SQL literal string
	// @param value The value to serialize
	// @param target_type Target column's logical type (used for casting decisions)
	// @return T-SQL literal string (e.g., "N'hello'", "123", "NULL")
	// @throws InvalidInputException for unsupported values (NaN, Inf)
	static string Serialize(const Value &value, const LogicalType &target_type);

	// Serialize a value from a Vector at given index
	// More efficient than extracting Value first for bulk operations
	// @param vector The vector containing values
	// @param index Row index in the vector
	// @param target_type Target column's logical type
	// @return T-SQL literal string
	static string SerializeFromVector(Vector &vector, idx_t index, const LogicalType &target_type);

	// Estimate serialized size for batch sizing decisions
	// @param value The value to estimate
	// @param type The value's logical type
	// @return Estimated byte size of serialized literal
	static idx_t EstimateSerializedSize(const Value &value, const LogicalType &type);

	//===----------------------------------------------------------------------===//
	// Identifier Escaping
	//===----------------------------------------------------------------------===//

	// Escape identifier for T-SQL using bracket quoting
	// @param name Raw identifier name
	// @return Escaped identifier (e.g., "name" → "[name]", "na]me" → "[na]]me]")
	static string EscapeIdentifier(const string &name);

	// Escape string value for T-SQL (without N'' wrapper)
	// @param value Raw string value
	// @return Escaped string with ' → ''
	static string EscapeString(const string &value);

	// Decimal serializer kept as a public helper: codec::decimal::FormatSqlLiteral
	// delegates here for the hugeint+scale → fixed-point rendering (single
	// canonical source). All other per-type Serialize<X> helpers were deleted in
	// spec 045 Phase 6 close-out — INSERT literal rendering goes exclusively
	// through codec::FormatSqlLiteral now.
	static string SerializeDecimal(const hugeint_t &value, uint8_t width, uint8_t scale);
};

}  // namespace duckdb
