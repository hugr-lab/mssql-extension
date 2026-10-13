#pragma once

#include <cstdint>
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {
namespace tds {
namespace encoding {

//===----------------------------------------------------------------------===//
// DateTimeEncoding - Convert SQL Server date/time wire formats
//===----------------------------------------------------------------------===//

class DateTimeEncoding {
public:
	// Convert SQL Server DATE (3 bytes) to DuckDB date_t
	// TDS format: 3-byte unsigned little-endian days since 0001-01-01
	static date_t ConvertDate(const uint8_t *data);

	// Convert SQL Server TIME to DuckDB dtime_t
	// TDS format: 3-5 bytes depending on scale (100ns ticks since midnight)
	// Scale 0-2: 3 bytes, Scale 3-4: 4 bytes, Scale 5-7: 5 bytes
	static dtime_t ConvertTime(const uint8_t *data, uint8_t scale);

	// Convert SQL Server DATETIME (8 bytes) to DuckDB timestamp_t
	// TDS format: 4 bytes days since 1900-01-01 + 4 bytes ticks (1/300 sec)
	static timestamp_t ConvertDatetime(const uint8_t *data);

	// Issue #426: the value ConvertDatetime reads for the DATETIME tick at or
	// below `value` (`up` false), or at or above it (`up` true). A tick reads as
	// its microsecond, truncated (.003333 for the server's .00333...), so a
	// value read back is its own tick's bound both ways, and a value between
	// two reads has a different tick each way. The tick a comparison with a
	// DATETIME column is decided by; past the day's last read (23:59:59.996666),
	// `up` is the next day's midnight.
	static timestamp_t DatetimeTickBound(timestamp_t value, bool up);

	// Convert SQL Server DATETIME2 to DuckDB timestamp_t
	// TDS format: time (3-5 bytes) + date (3 bytes)
	static timestamp_t ConvertDatetime2(const uint8_t *data, uint8_t scale);

	// Convert SQL Server SMALLDATETIME (4 bytes) to DuckDB timestamp_t
	// TDS format: 2 bytes days since 1900-01-01 + 2 bytes minutes since midnight
	static timestamp_t ConvertSmallDatetime(const uint8_t *data);

	// Convert SQL Server DATETIMEOFFSET to DuckDB timestamp_t (UTC)
	// TDS format: time (3-5 bytes) + date (3 bytes) + offset (2 bytes signed minutes)
	// The timestamp is returned as UTC by subtracting the offset
	static timestamp_t ConvertDatetimeOffset(const uint8_t *data, uint8_t scale);

	// Get the byte length for TIME/DATETIME2 based on scale
	static size_t GetTimeByteLength(uint8_t scale);
};

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb
