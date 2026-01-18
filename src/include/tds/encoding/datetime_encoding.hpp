#pragma once

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include <cstdint>

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
	static date_t ConvertDate(const uint8_t* data);

	// Convert SQL Server TIME to DuckDB dtime_t
	// TDS format: 3-5 bytes depending on scale (100ns ticks since midnight)
	// Scale 0-2: 3 bytes, Scale 3-4: 4 bytes, Scale 5-7: 5 bytes
	static dtime_t ConvertTime(const uint8_t* data, uint8_t scale);

	// Convert SQL Server DATETIME (8 bytes) to DuckDB timestamp_t
	// TDS format: 4 bytes days since 1900-01-01 + 4 bytes ticks (1/300 sec)
	static timestamp_t ConvertDatetime(const uint8_t* data);

	// Convert SQL Server DATETIME2 to DuckDB timestamp_t
	// TDS format: time (3-5 bytes) + date (3 bytes)
	static timestamp_t ConvertDatetime2(const uint8_t* data, uint8_t scale);

	// Convert SQL Server SMALLDATETIME (4 bytes) to DuckDB timestamp_t
	// TDS format: 2 bytes days since 1900-01-01 + 2 bytes minutes since midnight
	static timestamp_t ConvertSmallDatetime(const uint8_t* data);

	// Get the byte length for TIME/DATETIME2 based on scale
	static size_t GetTimeByteLength(uint8_t scale);
};

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb
