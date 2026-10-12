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

	// Encode ALL rows of a DataChunk into buffer, one 0xD1 ROW token per row
	// (token byte INCLUDED, unlike EncodeRow). W1+W2 (spec 054): per-column
	// state — UnifiedVectorFormat, resolved family encoder, NULL wire kind —
	// is hoisted once per chunk instead of being rebuilt per cell. This is
	// the production write path (BCPWriter::WriteRows).
	// @param buffer Output buffer (appended to)
	// @param chunk Source DataChunk (chunk.size() rows are encoded)
	// @param columns Column metadata for type information (target columns)
	// @param column_mapping Optional mapping: mapping[target_idx] = source_idx, or -1 for NULL
	//                       If nullptr, assumes 1:1 positional mapping
	static void EncodeChunk(vector<uint8_t> &buffer, DataChunk &chunk, const vector<mssql::BCPColumnMetadata> &columns,
							const vector<int32_t> *column_mapping = nullptr);

	// Encode a complete row from DataChunk into buffer
	// Iterates columns and calls type-specific encoders
	// Per-row compatibility path: rebuilds the per-column state (formats,
	// encoder resolution) on every call — prefer EncodeChunk for whole-chunk
	// encoding.
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
	// Wire widths, public because a column's PAYLOAD width is its contract with
	// COLMETADATA — the columnar scatter sizes the whole chunk from it before a
	// single value is touched (spec 057 step 3), so it cannot be private to the
	// appending encoder.
	//   time     scale 0-2: 3 bytes, 3-4: 4, 5-7: 5
	//   decimal  precision 1-9: 5, 10-19: 9, 20-28: 13, 29-38: 17
	static uint8_t GetTimeByteSize(uint8_t scale);
	static uint8_t GetDecimalByteSize(uint8_t precision);

	static void EncodeDecimal(vector<uint8_t> &buffer, const hugeint_t &value, uint8_t precision, uint8_t scale);

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

	//! Issue #447: the date of a date / datetime2 / datetimeoffset value, as
	//! days since 1970-01-01, must lie in SQL Server's 0001-01-01..9999-12-31.
	//! The wire's three-byte day count wraps anything else into a different,
	//! valid date -- DuckDB's 'infinity' became 1969-12-31 -- so the value is
	//! refused instead, by name. One compare per value, on the temporal arms
	//! only.
	static constexpr int64_t SERVER_MIN_EPOCH_DAY = -719162;  // 0001-01-01
	static constexpr int64_t SERVER_MAX_EPOCH_DAY = 2932896;  // 9999-12-31
	static inline void RequireServerDay(int64_t epoch_day) {
		if (epoch_day < SERVER_MIN_EPOCH_DAY || epoch_day > SERVER_MAX_EPOCH_DAY) {
			ThrowOutsideServerRange(epoch_day);
		}
	}
	[[noreturn]] static void ThrowOutsideServerRange(int64_t epoch_day);
	//! A timestamp of 9999-12-31 that rounds into the next day at the column's
	//! scale (datetime2(3) <- 23:59:59.9999995): no such day to carry into.
	[[noreturn]] static void ThrowRoundsPastServerRange();
	//! DuckDB's TIME '24:00:00': SQL Server's time ends at 23:59:59.9999999.
	//! The encoders used to store it as 23:59:59.999999 (or, at scale 7, send
	//! ticks outside the type), and a filter matched that instant's rows.
	static constexpr int64_t MICROS_PER_SERVER_DAY = 86400000000LL;
	static inline void RequireServerTime(int64_t micros) {
		if (micros < 0 || micros >= MICROS_PER_SERVER_DAY) {
			ThrowTimeOutsideServerRange();
		}
	}
	[[noreturn]] static void ThrowTimeOutsideServerRange();
	//! A TIMESTAMP's ±infinity: in TIMESTAMP_NS its raw value divides into a
	//! day of 1677 / 2262, inside the server's range, so the day check alone
	//! would send it as that day.
	static inline void RequireFiniteTimestamp(int64_t raw) {
		if (raw == NumericLimits<int64_t>::Maximum() || raw == -NumericLimits<int64_t>::Maximum()) {
			ThrowOutsideServerRange(raw > 0 ? date_t::infinity().days : date_t::ninfinity().days);
		}
	}

	// Time (TIMENTYPE 0x29)
	// Wire format: [length:1] [value:3/4/5 LE]
	// Value is 10^(-scale) seconds since midnight
	static void EncodeTime(vector<uint8_t> &buffer, dtime_t value, uint8_t scale);

	// DateTime2 (DATETIME2NTYPE 0x2A)
	// Wire format: [length:1] [time_portion] [date:3 LE]
	// Assumes ts is a µs-precision timestamp_t (DuckDB LogicalTypeId::TIMESTAMP).
	// For other TIMESTAMP_{MS,NS,S} variants use EncodeDatetime2Raw and compute
	// the components in the source's native precision (see codec::datetime).
	static void EncodeDatetime2(vector<uint8_t> &buffer, timestamp_t ts, uint8_t scale);

	// DateTime2 wire encoder from pre-computed (time_value, date_value, scale)
	// components. Used by codec::datetime to bypass the µs-only path in
	// EncodeDatetime2 — preserves full source precision for TIMESTAMP_NS into
	// DATETIME2(7), TIMESTAMP_SEC into DATETIME2(0), etc. Components must
	// already match `scale` (time_value = sub-day ticks at 10^(-scale) seconds;
	// date_value = days since 0001-01-01).
	static void EncodeDatetime2Raw(vector<uint8_t> &buffer, uint64_t time_value, uint32_t date_value, uint8_t scale);

	// DateTimeOffset (DATETIMEOFFSETNTYPE 0x2B)
	// Wire format: [length:1] [time] [date:3] [offset_minutes:2 signed LE]
	static void EncodeDatetimeOffset(vector<uint8_t> &buffer, timestamp_t ts, int16_t offset_minutes, uint8_t scale);

	// DateTimeOffset wire encoder from pre-computed components — see
	// EncodeDatetime2Raw rationale.
	static void EncodeDatetimeOffsetRaw(vector<uint8_t> &buffer, uint64_t time_value, uint32_t date_value,
										int16_t offset_minutes, uint8_t scale);

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

	// Convert DuckDB timestamp to TDS datetime2 components

	// Convert DuckDB timestamp to TDS datetime2 components
	static void TimestampToDatetime2Components(timestamp_t ts, uint8_t scale, uint64_t &time_value,
											   uint32_t &date_value);
};

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb
