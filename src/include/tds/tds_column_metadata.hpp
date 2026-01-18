#pragma once

#include "tds_types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace tds {

//===----------------------------------------------------------------------===//
// ColumnMetadata - Describes a single result column from COLMETADATA token
//===----------------------------------------------------------------------===//

struct ColumnMetadata {
	std::string name;           // Column name (UTF-8)
	uint8_t type_id;            // TDS type identifier
	uint16_t max_length;        // Maximum length for variable types
	uint8_t precision;          // Precision for DECIMAL/NUMERIC
	uint8_t scale;              // Scale for DECIMAL/NUMERIC or TIME
	uint32_t collation;         // Collation ID for string types
	uint16_t flags;             // Column flags (nullable, identity, etc.)

	// Derived properties
	bool IsNullable() const { return (flags & COL_FLAG_NULLABLE) != 0; }
	bool IsIdentity() const { return (flags & COL_FLAG_IDENTITY) != 0; }
	bool IsComputed() const { return (flags & COL_FLAG_COMPUTED) != 0; }

	// Get human-readable type name for error messages
	std::string GetTypeName() const;

	// Check if this is a variable-length type
	bool IsVariableLength() const;

	// Check if this is a nullable variant (INTN, FLOATN, etc.)
	bool IsNullableVariant() const;

	// Check if this is a PLP (Partially Length-Prefixed) type (MAX types)
	// MAX types have max_length == 0xFFFF and use chunked encoding
	bool IsPLPType() const;

	// Get the fixed size for fixed-length types (0 for variable)
	size_t GetFixedSize() const;
};

//===----------------------------------------------------------------------===//
// ColumnMetadataParser - Parse COLMETADATA token from TDS stream
//===----------------------------------------------------------------------===//

class ColumnMetadataParser {
public:
	// Parse COLMETADATA token and return column definitions
	// Returns true if parsing succeeded, false if more data needed
	// Throws on parse error
	static bool Parse(const uint8_t* data, size_t length, size_t& bytes_consumed,
	                  std::vector<ColumnMetadata>& columns);

private:
	// Parse a single column definition
	static bool ParseColumn(const uint8_t* data, size_t length, size_t& offset,
	                        ColumnMetadata& column);

	// Parse type-specific metadata (length, precision, scale, collation)
	static bool ParseTypeInfo(const uint8_t* data, size_t length, size_t& offset,
	                          ColumnMetadata& column);

	// Parse B_VARCHAR column name
	static bool ParseColumnName(const uint8_t* data, size_t length, size_t& offset,
	                            std::string& name);
};

}  // namespace tds
}  // namespace duckdb
