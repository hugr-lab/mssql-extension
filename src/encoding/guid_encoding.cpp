#include "encoding/guid_encoding.hpp"
#include <cstring>

namespace duckdb {
namespace encoding {

void GuidEncoding::ReorderGuidBytes(const uint8_t* input, uint8_t* output) {
	// SQL Server GUID wire format (mixed-endian):
	//   bytes 0-3: Data1 (little-endian uint32)
	//   bytes 4-5: Data2 (little-endian uint16)
	//   bytes 6-7: Data3 (little-endian uint16)
	//   bytes 8-15: Data4 (big-endian, as-is)
	//
	// Standard UUID format (big-endian):
	//   bytes 0-3: Data1 (big-endian)
	//   bytes 4-5: Data2 (big-endian)
	//   bytes 6-7: Data3 (big-endian)
	//   bytes 8-15: Data4 (big-endian, as-is)

	// Reverse byte order for Data1 (bytes 0-3)
	output[0] = input[3];
	output[1] = input[2];
	output[2] = input[1];
	output[3] = input[0];

	// Reverse byte order for Data2 (bytes 4-5)
	output[4] = input[5];
	output[5] = input[4];

	// Reverse byte order for Data3 (bytes 6-7)
	output[6] = input[7];
	output[7] = input[6];

	// Data4 (bytes 8-15) stays as-is
	std::memcpy(output + 8, input + 8, 8);
}

hugeint_t GuidEncoding::ConvertGuid(const uint8_t* data) {
	// Reorder bytes to standard UUID format
	uint8_t reordered[16];
	ReorderGuidBytes(data, reordered);

	// Convert to hugeint_t (big-endian)
	uint64_t upper = static_cast<uint64_t>(reordered[0]) << 56 |
	                 static_cast<uint64_t>(reordered[1]) << 48 |
	                 static_cast<uint64_t>(reordered[2]) << 40 |
	                 static_cast<uint64_t>(reordered[3]) << 32 |
	                 static_cast<uint64_t>(reordered[4]) << 24 |
	                 static_cast<uint64_t>(reordered[5]) << 16 |
	                 static_cast<uint64_t>(reordered[6]) << 8 |
	                 static_cast<uint64_t>(reordered[7]);
	uint64_t lower = static_cast<uint64_t>(reordered[8]) << 56 |
	                 static_cast<uint64_t>(reordered[9]) << 48 |
	                 static_cast<uint64_t>(reordered[10]) << 40 |
	                 static_cast<uint64_t>(reordered[11]) << 32 |
	                 static_cast<uint64_t>(reordered[12]) << 24 |
	                 static_cast<uint64_t>(reordered[13]) << 16 |
	                 static_cast<uint64_t>(reordered[14]) << 8 |
	                 static_cast<uint64_t>(reordered[15]);

	// DuckDB expects UUID with high bit flipped for sortability
	// See duckdb/src/common/types/uuid.cpp
	hugeint_t result;
	result.upper = static_cast<int64_t>(upper ^ (uint64_t(1) << 63));
	result.lower = lower;

	return result;
}

}  // namespace encoding
}  // namespace duckdb
