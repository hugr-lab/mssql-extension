#include "tds/encoding/guid_encoding.hpp"
#include <cstring>

namespace duckdb {
namespace tds {
namespace encoding {

void GuidEncoding::ReorderGuidBytes(const uint8_t *input, uint8_t *output) {
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

hugeint_t GuidEncoding::ConvertGuid(const uint8_t *data) {
	// The byte shuffle above decomposes into WORD operations, which is what this
	// does instead of moving sixteen bytes one at a time through a scratch array.
	//
	// Reversing Data1's four bytes and then reading them big-endian is exactly
	// reading them little-endian in the first place — the two reversals cancel.
	// Same for Data2 and Data3. Only Data4 keeps its wire order, and reading it
	// big-endian is one byte swap of a 64-bit load.
	//
	// Little-endian host is already assumed throughout the read path (the 1:1
	// integer columns are stored straight from the wire).
	uint32_t data1;
	uint16_t data2;
	uint16_t data3;
	std::memcpy(&data1, data, 4);
	std::memcpy(&data2, data + 4, 2);
	std::memcpy(&data3, data + 6, 2);

	const uint64_t upper =
		(static_cast<uint64_t>(data1) << 32) | (static_cast<uint64_t>(data2) << 16) | static_cast<uint64_t>(data3);
	// Written as a byte-wise big-endian assembly on purpose: every compiler
	// recognises this shape and emits a single rev/bswap.
	const uint64_t lower = static_cast<uint64_t>(data[8]) << 56 | static_cast<uint64_t>(data[9]) << 48 |
						   static_cast<uint64_t>(data[10]) << 40 | static_cast<uint64_t>(data[11]) << 32 |
						   static_cast<uint64_t>(data[12]) << 24 | static_cast<uint64_t>(data[13]) << 16 |
						   static_cast<uint64_t>(data[14]) << 8 | static_cast<uint64_t>(data[15]);

	// DuckDB stores UUIDs with the high bit flipped so that the integer order
	// matches the textual order (duckdb/src/common/types/uuid.cpp).
	hugeint_t result;
	result.upper = static_cast<int64_t>(upper ^ (uint64_t(1) << 63));
	result.lower = lower;
	return result;
}

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb
