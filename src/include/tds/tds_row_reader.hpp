#pragma once

#include "tds_types.hpp"
#include "tds_column_metadata.hpp"
#include "tds_token_parser.hpp"
#include <cstdint>
#include <vector>

namespace duckdb {
namespace tds {

//===----------------------------------------------------------------------===//
// RowReader - Extracts typed values from ROW token data
//===----------------------------------------------------------------------===//

class RowReader {
public:
	explicit RowReader(const std::vector<ColumnMetadata>& columns);
	~RowReader() = default;

	// Read a complete row from the data buffer
	// Returns true if row was read, false if more data needed
	// Throws on parse error
	bool ReadRow(const uint8_t* data, size_t length, size_t& bytes_consumed, RowData& row);

	// Read a Null Bitmap Compressed (NBC) row from the data buffer
	// NBC rows have a bitmap indicating NULL columns, followed by data for non-NULL columns
	bool ReadNBCRow(const uint8_t* data, size_t length, size_t& bytes_consumed, RowData& row);

	// Skip a row without parsing (fast path for drain)
	// Returns true if row was skipped, false if more data needed
	bool SkipRow(const uint8_t* data, size_t length, size_t& bytes_consumed);

	// Skip a NBC row without parsing (fast path for drain)
	bool SkipNBCRow(const uint8_t* data, size_t length, size_t& bytes_consumed);

private:
	// Skip a single column value (returns bytes to skip, 0 if more data needed)
	size_t SkipValue(const uint8_t* data, size_t length, size_t col_idx);
	// Read a single column value
	// Returns bytes consumed, 0 if more data needed
	size_t ReadValue(const uint8_t* data, size_t length, size_t col_idx,
	                 std::vector<uint8_t>& value, bool& is_null);

	// NBC variants - for non-NULL columns in NBC rows (no length prefix for nullable types)
	size_t SkipValueNBC(const uint8_t* data, size_t length, size_t col_idx);
	size_t ReadValueNBC(const uint8_t* data, size_t length, size_t col_idx,
	                    std::vector<uint8_t>& value, bool& is_null);

	// Type-specific readers
	size_t ReadFixedType(const uint8_t* data, size_t length, uint8_t type_id,
	                     std::vector<uint8_t>& value);

	size_t ReadNullableFixedType(const uint8_t* data, size_t length, uint8_t type_id,
	                             uint8_t declared_length,
	                             std::vector<uint8_t>& value, bool& is_null);

	size_t ReadVariableLengthType(const uint8_t* data, size_t length, uint8_t type_id,
	                              std::vector<uint8_t>& value, bool& is_null);

	size_t ReadDecimalType(const uint8_t* data, size_t length,
	                       std::vector<uint8_t>& value, bool& is_null);

	size_t ReadDateType(const uint8_t* data, size_t length,
	                    std::vector<uint8_t>& value, bool& is_null);

	size_t ReadTimeType(const uint8_t* data, size_t length, uint8_t scale,
	                    std::vector<uint8_t>& value, bool& is_null);

	size_t ReadDateTime2Type(const uint8_t* data, size_t length, uint8_t scale,
	                         std::vector<uint8_t>& value, bool& is_null);

	size_t ReadGuidType(const uint8_t* data, size_t length,
	                    std::vector<uint8_t>& value, bool& is_null);

	const std::vector<ColumnMetadata>& columns_;
};

}  // namespace tds
}  // namespace duckdb
