#pragma once

#include "duckdb/common/types/hugeint.hpp"
#include <cstdint>
#include <vector>

namespace duckdb {
namespace tds {
namespace encoding {

//===----------------------------------------------------------------------===//
// DecimalEncoding - Convert SQL Server DECIMAL/NUMERIC and MONEY formats
//===----------------------------------------------------------------------===//

class DecimalEncoding {
public:
	// Convert SQL Server DECIMAL/NUMERIC wire format to DuckDB hugeint
	// TDS format: sign (1 byte) + magnitude (little-endian integer)
	// sign: 0 = negative, 1 = positive
	// Returns the unscaled integer value for DECIMAL storage
	static hugeint_t ConvertDecimal(const uint8_t* data, size_t length);

	// Convert SQL Server MONEY (8 bytes) to unscaled DECIMAL(19,4) value
	// TDS format: int64_t little-endian representing value × 10000
	static hugeint_t ConvertMoney(const uint8_t* data);

	// Convert SQL Server SMALLMONEY (4 bytes) to unscaled DECIMAL(10,4) value
	// TDS format: int32_t little-endian representing value × 10000
	static hugeint_t ConvertSmallMoney(const uint8_t* data);
};

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb
