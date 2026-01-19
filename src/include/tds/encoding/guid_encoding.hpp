#pragma once

#include <cstdint>
#include "duckdb/common/types/hugeint.hpp"

namespace duckdb {
namespace tds {
namespace encoding {

//===----------------------------------------------------------------------===//
// GuidEncoding - Convert SQL Server UNIQUEIDENTIFIER wire format
//===----------------------------------------------------------------------===//

class GuidEncoding {
public:
	// Convert SQL Server UNIQUEIDENTIFIER (16 bytes) to DuckDB UUID
	// TDS GUID format (mixed-endian):
	//   bytes 0-3: Data1 (little-endian uint32)
	//   bytes 4-5: Data2 (little-endian uint16)
	//   bytes 6-7: Data3 (little-endian uint16)
	//   bytes 8-15: Data4 (big-endian, as-is)
	// DuckDB UUID is stored as big-endian hugeint_t
	static hugeint_t ConvertGuid(const uint8_t *data);

	// Reorder GUID bytes from TDS mixed-endian to standard big-endian
	// Output buffer must be 16 bytes
	static void ReorderGuidBytes(const uint8_t *input, uint8_t *output);
};

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb
