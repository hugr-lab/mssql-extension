#pragma once

#include "tds/tds_column_metadata.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/vector.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace encoding {

//===----------------------------------------------------------------------===//
// TypeConverter - Maps SQL Server types to DuckDB types and converts values
//===----------------------------------------------------------------------===//

class TypeConverter {
public:
	// Map SQL Server type to DuckDB LogicalType
	// Throws InvalidInputException for unsupported types
	static LogicalType GetDuckDBType(const tds::ColumnMetadata& column);

	// Convert a raw TDS value to DuckDB format and write to vector
	// value: raw bytes from TDS ROW token
	// column: column metadata for type info
	// vector: target DuckDB vector
	// row_idx: row index within the vector
	static void ConvertValue(const std::vector<uint8_t>& value, bool is_null,
	                         const tds::ColumnMetadata& column,
	                         Vector& vector, idx_t row_idx);

	// Check if a SQL Server type is supported
	static bool IsSupported(uint8_t type_id);

	// Get human-readable type name for error messages
	static std::string GetTypeName(uint8_t type_id);

private:
	// Type-specific converters
	static void ConvertInteger(const std::vector<uint8_t>& value,
	                           const tds::ColumnMetadata& column,
	                           Vector& vector, idx_t row_idx);

	static void ConvertBoolean(const std::vector<uint8_t>& value,
	                           Vector& vector, idx_t row_idx);

	static void ConvertFloat(const std::vector<uint8_t>& value,
	                         const tds::ColumnMetadata& column,
	                         Vector& vector, idx_t row_idx);

	static void ConvertDecimal(const std::vector<uint8_t>& value,
	                           const tds::ColumnMetadata& column,
	                           Vector& vector, idx_t row_idx);

	static void ConvertMoney(const std::vector<uint8_t>& value,
	                         const tds::ColumnMetadata& column,
	                         Vector& vector, idx_t row_idx);

	static void ConvertString(const std::vector<uint8_t>& value,
	                          const tds::ColumnMetadata& column,
	                          Vector& vector, idx_t row_idx);

	static void ConvertBinary(const std::vector<uint8_t>& value,
	                          Vector& vector, idx_t row_idx);

	static void ConvertDate(const std::vector<uint8_t>& value,
	                        Vector& vector, idx_t row_idx);

	static void ConvertTime(const std::vector<uint8_t>& value,
	                        const tds::ColumnMetadata& column,
	                        Vector& vector, idx_t row_idx);

	static void ConvertDateTime(const std::vector<uint8_t>& value,
	                            const tds::ColumnMetadata& column,
	                            Vector& vector, idx_t row_idx);

	static void ConvertGuid(const std::vector<uint8_t>& value,
	                        Vector& vector, idx_t row_idx);
};

}  // namespace encoding
}  // namespace duckdb
