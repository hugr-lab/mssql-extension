#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

namespace mssql {
struct BCPColumnMetadata;
}  // namespace mssql

namespace tds {
namespace encoding {

//===----------------------------------------------------------------------===//
// BCPRowEncoder - Binary row encoding for TDS BulkLoadBCP protocol
//
// Encodes DuckDB values into TDS wire format for BulkLoadBCP ROW tokens.
// Each value is encoded as: [length_prefix] [data_bytes]
//
// Encoding rules follow MS-TDS specification for BULK_LOAD packet type 0x07.
//===----------------------------------------------------------------------===//

class BCPRowEncoder {
public:
	//===----------------------------------------------------------------------===//
	// Row-Level Encoding
	//===----------------------------------------------------------------------===//

	// Encode a complete row from DataChunk into buffer
	// Iterates columns and calls type-specific encoders
	// @param buffer Output buffer (ROW token data, not including 0xD1)
	// @param chunk Source DataChunk
	// @param row_idx Row index within the chunk
	// @param columns Column metadata for type information (target columns)
	// @param column_mapping Optional mapping: mapping[target_idx] = source_idx, or -1 for NULL
	//                       If nullptr, assumes 1:1 positional mapping
	static void EncodeRow(vector<uint8_t> &buffer, DataChunk &chunk, idx_t row_idx,
	                      const vector<mssql::BCPColumnMetadata> &columns,
	                      const vector<int32_t> *column_mapping = nullptr);

	// Encode a single Value into buffer
	// @param buffer Output buffer
	// @param value DuckDB Value to encode
	// @param col Column metadata for type information
	static void EncodeValue(vector<uint8_t> &buffer, const Value &value, const mssql::BCPColumnMetadata &col);

	//===----------------------------------------------------------------------===//
	// Type-Specific Encoders
	//===----------------------------------------------------------------------===//

	// Integer types (INTNTYPE 0x26)
	// Wire format: [length:1] [value:1/2/4/8 LE]
	static void EncodeInt8(vector<uint8_t> &buffer, int8_t value);
	static void EncodeInt16(vector<uint8_t> &buffer, int16_t value);
	static void EncodeInt32(vector<uint8_t> &buffer, int32_t value);
	static void EncodeInt64(vector<uint8_t> &buffer, int64_t value);
	static void EncodeUInt8(vector<uint8_t> &buffer, uint8_t value);

	// Bit type (BITNTYPE 0x68)
	// Wire format: [length:1] [value:1]
	static void EncodeBit(vector<uint8_t> &buffer, bool value);

	// Float types (FLTNTYPE 0x6D)
	// Wire format: [length:1] [value:4/8 IEEE754 LE]
	static void EncodeFloat(vector<uint8_t> &buffer, float value);
	static void EncodeDouble(vector<uint8_t> &buffer, double value);

	// Decimal type (DECIMALNTYPE 0x6A)
	// Wire format: [length:1] [sign:1] [mantissa:4/8/12/16 LE]
	// sign: 0x00=negative, 0x01=non-negative
	static void EncodeDecimal(vector<uint8_t> &buffer, const hugeint_t &value, uint8_t precision, uint8_t scale);

	// Unicode string (NVARCHARTYPE 0xE7) - standard USHORT length prefix
	// Wire format: [length:2 LE] [utf16le_bytes]
	// NULL: [0xFFFF]
	// Use for nvarchar(n) where n <= 4000
	static void EncodeNVarchar(vector<uint8_t> &buffer, const string_t &value);

	// Unicode string (NVARCHARTYPE 0xE7) - PLP (Partially Length-prefixed) format
	// Wire format: [total_length:8 LE] [chunk_length:4 LE] [chunk_data] [terminator:4 = 0x00000000]
	// NULL: [0xFFFFFFFFFFFFFFFF]
	// Use for nvarchar(max) when max_length == 0xFFFF
	static void EncodeNVarcharPLP(vector<uint8_t> &buffer, const string_t &value);

	// Binary data (BIGVARBINARYTYPE 0xA5) - standard USHORT length prefix
	// Wire format: [length:2 LE] [bytes]
	// NULL: [0xFFFF]
	// Use for varbinary(n) where n <= 8000
	static void EncodeBinary(vector<uint8_t> &buffer, const string_t &value);

	// Binary data (BIGVARBINARYTYPE 0xA5) - PLP format
	// Wire format: [total_length:8 LE] [chunk_length:4 LE] [chunk_data] [terminator:4 = 0x00000000]
	// NULL: [0xFFFFFFFFFFFFFFFF]
	// Use for varbinary(max) when max_length == 0xFFFF
	static void EncodeBinaryPLP(vector<uint8_t> &buffer, const string_t &value);

	// GUID (GUIDTYPE 0x24)
	// Wire format: [length:1] [Data1:4 LE] [Data2:2 LE] [Data3:2 LE] [Data4:8 BE]
	// Mixed-endian per MS-DTYP spec
	static void EncodeGUID(vector<uint8_t> &buffer, const hugeint_t &uuid);

	// Date (DATENTYPE 0x28)
	// Wire format: [length:1] [days:3 LE unsigned]
	// Days since 0001-01-01
	static void EncodeDate(vector<uint8_t> &buffer, date_t value);

	// Time (TIMENTYPE 0x29)
	// Wire format: [length:1] [value:3/4/5 LE]
	// Value is 10^(-scale) seconds since midnight
	static void EncodeTime(vector<uint8_t> &buffer, dtime_t value, uint8_t scale);

	// DateTime2 (DATETIME2NTYPE 0x2A)
	// Wire format: [length:1] [time_portion] [date:3 LE]
	static void EncodeDatetime2(vector<uint8_t> &buffer, timestamp_t ts, uint8_t scale);

	// DateTimeOffset (DATETIMEOFFSETNTYPE 0x2B)
	// Wire format: [length:1] [time] [date:3] [offset_minutes:2 signed LE]
	static void EncodeDatetimeOffset(vector<uint8_t> &buffer, timestamp_t ts, int16_t offset_minutes, uint8_t scale);

	//===----------------------------------------------------------------------===//
	// NULL Encoding
	//===----------------------------------------------------------------------===//

	// Encode NULL for fixed-length types (INTNTYPE, BITNTYPE, FLTNTYPE, etc.)
	// Wire format: [0x00]
	static void EncodeNullFixed(vector<uint8_t> &buffer);

	// Encode NULL for variable-length USHORTLEN types (NVARCHARTYPE, BIGVARBINARYTYPE)
	// Wire format: [0xFFFF]
	static void EncodeNullVariable(vector<uint8_t> &buffer);

	// Encode NULL for PLP types (nvarchar(max), varbinary(max))
	// Wire format: [0xFFFFFFFFFFFFFFFF] (8 bytes)
	static void EncodeNullPLP(vector<uint8_t> &buffer);

	// Encode NULL for GUID type
	// Wire format: [0x00]
	static void EncodeNullGUID(vector<uint8_t> &buffer);

	// Encode NULL for date/time types
	// Wire format: [0x00]
	static void EncodeNullDateTime(vector<uint8_t> &buffer);

private:
	//===----------------------------------------------------------------------===//
	// Helper Methods
	//===----------------------------------------------------------------------===//

	// Convert string to UTF-16LE bytes
	static vector<uint8_t> StringToUTF16LE(const string_t &str);

	// Get time byte size based on scale
	// scale 0-2: 3 bytes, scale 3-4: 4 bytes, scale 5-7: 5 bytes
	static uint8_t GetTimeByteSize(uint8_t scale);

	// Get decimal byte size based on precision
	// precision 1-9: 5, 10-19: 9, 20-28: 13, 29-38: 17
	static uint8_t GetDecimalByteSize(uint8_t precision);

	// Convert DuckDB timestamp to TDS datetime2 components
	static void TimestampToDatetime2Components(timestamp_t ts, uint8_t scale, uint64_t &time_value,
											   uint32_t &date_value);
};

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb
