#include "tds/encoding/datetime_encoding.hpp"
#include <cstring>

namespace duckdb {
namespace tds {
namespace encoding {

// Days from 0001-01-01 to 1970-01-01
constexpr int32_t DAYS_FROM_0001_TO_EPOCH = 719162;

// Days from 1900-01-01 to 1970-01-01
constexpr int32_t DAYS_FROM_1900_TO_EPOCH = 25567;

// Microseconds per day
constexpr int64_t MICROS_PER_DAY = 86400000000LL;

// Microseconds per second
constexpr int64_t MICROS_PER_SECOND = 1000000LL;

date_t DateTimeEncoding::ConvertDate(const uint8_t* data) {
	// DATE: 3 bytes unsigned little-endian, days since 0001-01-01
	int32_t days = static_cast<int32_t>(data[0]) |
	               (static_cast<int32_t>(data[1]) << 8) |
	               (static_cast<int32_t>(data[2]) << 16);

	// Convert to days since 1970-01-01
	return date_t(days - DAYS_FROM_0001_TO_EPOCH);
}

dtime_t DateTimeEncoding::ConvertTime(const uint8_t* data, uint8_t scale) {
	// TIME: 3-5 bytes depending on scale
	// Value is in 100-nanosecond units
	size_t len = GetTimeByteLength(scale);

	int64_t ticks = 0;
	for (size_t i = 0; i < len; i++) {
		ticks |= static_cast<int64_t>(data[i]) << (i * 8);
	}

	// Convert from 100ns units to microseconds
	int64_t microseconds = ticks / 10;

	return dtime_t(microseconds);
}

timestamp_t DateTimeEncoding::ConvertDatetime(const uint8_t* data) {
	// DATETIME: 4 bytes days since 1900-01-01 + 4 bytes ticks (1/300 second)
	int32_t days = 0;
	int32_t ticks = 0;
	std::memcpy(&days, data, 4);
	std::memcpy(&ticks, data + 4, 4);

	// Convert to days since 1970-01-01
	int32_t unix_days = days - DAYS_FROM_1900_TO_EPOCH;

	// Convert ticks to microseconds (1 tick = 1/300 second)
	// microseconds = ticks * 1000000 / 300 = ticks * 10000 / 3
	int64_t microseconds = (static_cast<int64_t>(ticks) * 10000) / 3;

	return timestamp_t(static_cast<int64_t>(unix_days) * MICROS_PER_DAY + microseconds);
}

timestamp_t DateTimeEncoding::ConvertDatetime2(const uint8_t* data, uint8_t scale) {
	// DATETIME2: time (3-5 bytes) + date (3 bytes)
	size_t time_len = GetTimeByteLength(scale);

	// Read time (100ns units)
	int64_t time_ticks = 0;
	for (size_t i = 0; i < time_len; i++) {
		time_ticks |= static_cast<int64_t>(data[i]) << (i * 8);
	}

	// Read date (days since 0001-01-01)
	int32_t days = static_cast<int32_t>(data[time_len]) |
	               (static_cast<int32_t>(data[time_len + 1]) << 8) |
	               (static_cast<int32_t>(data[time_len + 2]) << 16);

	// Convert to days since 1970-01-01
	int32_t unix_days = days - DAYS_FROM_0001_TO_EPOCH;

	// Convert time to microseconds (from 100ns units)
	int64_t microseconds = time_ticks / 10;

	return timestamp_t(static_cast<int64_t>(unix_days) * MICROS_PER_DAY + microseconds);
}

timestamp_t DateTimeEncoding::ConvertSmallDatetime(const uint8_t* data) {
	// SMALLDATETIME: 2 bytes days since 1900-01-01 + 2 bytes minutes since midnight
	uint16_t days = 0;
	uint16_t minutes = 0;
	std::memcpy(&days, data, 2);
	std::memcpy(&minutes, data + 2, 2);

	// Convert to days since 1970-01-01
	int32_t unix_days = static_cast<int32_t>(days) - DAYS_FROM_1900_TO_EPOCH;

	// Convert minutes to microseconds
	int64_t microseconds = static_cast<int64_t>(minutes) * 60 * MICROS_PER_SECOND;

	return timestamp_t(static_cast<int64_t>(unix_days) * MICROS_PER_DAY + microseconds);
}

timestamp_t DateTimeEncoding::ConvertDatetimeOffset(const uint8_t* data, uint8_t scale) {
	// DATETIMEOFFSET: time (3-5 bytes) + date (3 bytes) + offset (2 bytes signed minutes)
	// Note: SQL Server stores DATETIMEOFFSET with the time component already in UTC.
	// The offset is stored for display purposes only. We just need to read the UTC time.
	//
	// Time encoding varies by scale:
	// - Time is stored in units of 10^(-scale) seconds
	// - Scale 7: 100-nanosecond ticks (divide by 10 for microseconds)
	// - Scale 3: millisecond ticks (multiply by 1000 for microseconds)
	// - Scale 0: second ticks (multiply by 1000000 for microseconds)
	size_t time_len = GetTimeByteLength(scale);

	// Read time ticks - this is already in UTC
	int64_t time_ticks = 0;
	for (size_t i = 0; i < time_len; i++) {
		time_ticks |= static_cast<int64_t>(data[i]) << (i * 8);
	}

	// Read date (days since 0001-01-01) - this is already in UTC
	int32_t days = static_cast<int32_t>(data[time_len]) |
	               (static_cast<int32_t>(data[time_len + 1]) << 8) |
	               (static_cast<int32_t>(data[time_len + 2]) << 16);

	// Offset (2 bytes) is at data[time_len + 3] but we don't need it for UTC conversion
	// The time is already stored as UTC

	// Convert to days since 1970-01-01
	int32_t unix_days = days - DAYS_FROM_0001_TO_EPOCH;

	// Convert time ticks to microseconds based on scale
	// Time is in units of 10^(-scale) seconds, we need microseconds (10^(-6) seconds)
	// microseconds = ticks * 10^(6-scale)
	int64_t microseconds;
	if (scale <= 6) {
		// Multiply for scales 0-6
		int64_t multiplier = 1;
		for (int i = 0; i < 6 - scale; i++) {
			multiplier *= 10;
		}
		microseconds = time_ticks * multiplier;
	} else {
		// Divide for scale 7
		microseconds = time_ticks / 10;
	}

	// Calculate UTC timestamp directly (time is already in UTC)
	int64_t utc_timestamp = static_cast<int64_t>(unix_days) * MICROS_PER_DAY + microseconds;

	return timestamp_t(utc_timestamp);
}

size_t DateTimeEncoding::GetTimeByteLength(uint8_t scale) {
	// Scale 0-2: 3 bytes
	// Scale 3-4: 4 bytes
	// Scale 5-7: 5 bytes
	if (scale <= 2) return 3;
	if (scale <= 4) return 4;
	return 5;
}

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb
