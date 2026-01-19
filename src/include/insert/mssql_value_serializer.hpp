#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include <string>

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLValueSerializer - Converts DuckDB values to T-SQL literal strings
//
// This class provides static methods to convert DuckDB values into T-SQL
// literal strings suitable for embedding in SQL statements.
//
// Key design decisions:
// - All strings use N'...' Unicode literals for server-side collation handling
// - Single quotes are escaped by doubling: ' → ''
// - Identifiers are bracket-quoted: name → [name], with ] → ]]
// - NaN and Infinity values are rejected (SQL Server doesn't support them)
// - UBIGINT uses CAST to DECIMAL(20,0) to handle values > BIGINT max
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

	//===----------------------------------------------------------------------===//
	// Type-Specific Serializers
	//===----------------------------------------------------------------------===//

	// Boolean: returns "0" or "1" (BIT type)
	static string SerializeBoolean(bool value);

	// Integer types: returns decimal string
	static string SerializeInteger(int64_t value);

	// UBIGINT: uses CAST to DECIMAL(20,0) for values > BIGINT max
	static string SerializeUBigInt(uint64_t value);

	// Float: returns decimal or scientific notation
	// @throws InvalidInputException for NaN or Infinity
	static string SerializeFloat(float value);

	// Double: returns decimal or scientific notation
	// @throws InvalidInputException for NaN or Infinity
	static string SerializeDouble(double value);

	// Decimal: preserves scale, returns decimal literal
	static string SerializeDecimal(const hugeint_t &value, uint8_t width, uint8_t scale);

	// String: returns N'escaped_string' (Unicode literal)
	static string SerializeString(const string_t &value);

	// Blob: returns 0x hex encoding
	static string SerializeBlob(const string_t &value);

	// UUID: returns string literal 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx'
	static string SerializeUUID(const hugeint_t &value);

	// Date: returns ISO date literal 'YYYY-MM-DD'
	static string SerializeDate(date_t value);

	// Time: returns ISO time literal 'HH:MM:SS.fffffff'
	static string SerializeTime(dtime_t value);

	// Timestamp: returns CAST('YYYY-MM-DDTHH:MM:SS.fffffff' AS DATETIME2(7))
	static string SerializeTimestamp(timestamp_t value);

	// Timestamp with timezone: returns CAST('...' AS DATETIMEOFFSET(7))
	// @param value UTC timestamp
	// @param offset_seconds Timezone offset in seconds
	static string SerializeTimestampTZ(timestamp_t value, int32_t offset_seconds);

private:
	// Helper to check for NaN/Inf and throw if found
	static void ValidateFloatValue(double value);
};

}  // namespace duckdb
