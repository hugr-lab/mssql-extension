//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/datetime_codec.cpp
//
// DateTime family implementation. See codec/datetime_codec.hpp.
//
// Behaviour parity (vs pre-spec-045 baseline):
//
//   DecodeFromTds  — switch on col.type_id, delegate to
//     DateTimeEncoding::Convert{Date,Time,Datetime,SmallDatetime,
//     Datetime2,DatetimeOffset}. Mirrors TypeConverter::ConvertDate /
//     ConvertTime / ConvertDateTime / ConvertDatetimeOffset including
//     DATETIMEN's length-dispatched (4 → smalldatetime / 8 → datetime)
//     fork.
//
//   EncodeToBcp    — switch on DuckDB type id, delegate to
//     BCPRowEncoder::Encode{Date,Time,Datetime2,DatetimeOffset}. col.scale
//     is the BCP COLMETADATA-declared TIME/DATETIME2/DATETIMEOFFSET scale
//     (0..7).
//
//   FormatSqlLiteral — FR-022 byte-identical across both LiteralContext
//     values. Canonical T-SQL forms with explicit CAST(... AS <TYPE>(7)):
//       DATE          'YYYY-MM-DD'
//       TIME          'HH:MM:SS.fffffff'
//       TIMESTAMP*    CAST('YYYY-MM-DDTHH:MM:SS.fffffff' AS DATETIME2(7))
//       TIMESTAMP_TZ  CAST('YYYY-MM-DDTHH:MM:SS.fffffff+HH:MM' AS DATETIMEOFFSET(7))
//     This unifies the pre-spec-045 filter_encoder form (which used
//     Date::ToString / Timestamp::ToString and could diverge on edge
//     cases) onto the more explicit INSERT-path form.
//
//   FormatDdlTypeName — FR-024/FR-027/FR-028 byte-identical across both
//     DdlContext values. New arms for TIMESTAMP_MS/NS/SEC (pre-spec-045
//     both DDL functions threw NotImplementedException for these):
//       DATE          → DATE
//       TIME          → TIME(7)
//       TIMESTAMP     → DATETIME2(6)   (μs — DuckDB native)
//       TIMESTAMP_MS  → DATETIME2(3)   (ms — exact)
//       TIMESTAMP_NS  → DATETIME2(7)   (ns — closest fit, lossy 2 digits)
//       TIMESTAMP_SEC → DATETIME2(0)   (s  — exact)
//       TIMESTAMP_TZ  → DATETIMEOFFSET(7)
//     Note: pre-spec-045 MapTypeToSQLServer used DATETIME2(6) for TIMESTAMP
//     while MapLogicalTypeToCTAS used DATETIME2(7). Spec 045 unifies on (6)
//     for both (μs precision matches DuckDB's TIMESTAMP exactly — DATETIME2(7)
//     was reserving wire space for digits the DuckDB peer can never populate).
//
//   RenderAsString — issue-#89 fallback. Dispatches on col.type_id +
//     bytes.size() to handle every wire-format combination, returning the
//     bare text form (no SQL quoting). Reused by WriteAsStringFallback so
//     the rendered text is deterministic and round-trippable.
//===----------------------------------------------------------------------===//

#include "codec/datetime_codec.hpp"

#include "copy/target_resolver.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "mssql_compat.hpp"
#include "tds/encoding/bcp_row_encoder.hpp"
#include "tds/encoding/datetime_encoding.hpp"
#include "tds/tds_column_metadata.hpp"
#include "tds/tds_types.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>

namespace duckdb {
namespace mssql {
namespace codec {
namespace datetime {

namespace {

using duckdb::tds::TDS_TYPE_DATE;
using duckdb::tds::TDS_TYPE_DATETIME;
using duckdb::tds::TDS_TYPE_DATETIME2;
using duckdb::tds::TDS_TYPE_DATETIMEN;
using duckdb::tds::TDS_TYPE_DATETIMEOFFSET;
using duckdb::tds::TDS_TYPE_SMALLDATETIME;
using duckdb::tds::TDS_TYPE_TIME;

template <typename T>
T GetVectorValue(Vector &vec, idx_t row_idx) {
	UnifiedVectorFormat format;
	vec.ToUnifiedFormat(1, format);
	auto data = UnifiedVectorFormat::GetData<T>(format);
	auto idx = format.sel->get_index(row_idx);
	return data[idx];
}

//===----------------------------------------------------------------------===//
// Canonical text renderers — shared between FormatSqlLiteral and
// RenderAsString. Always emit 7-digit (100-ns) fractional seconds: matches
// DATETIME2(7) / DATETIMEOFFSET(7) precision and avoids SQL Server CAST
// surprises when an INSERT literal lands in a higher-scale column.
//===----------------------------------------------------------------------===//

std::string FormatDateText(date_t value) {
	int32_t y, mo, d;
	Date::Convert(value, y, mo, d);
	return StringUtil::Format("%04d-%02d-%02d", y, mo, d);
}

std::string FormatTimeText(dtime_t value) {
	int32_t h, mi, s, us;
	Time::Convert(value, h, mi, s, us);
	int32_t hundred_ns = us * 10;  // µs → 100 ns
	return StringUtil::Format("%02d:%02d:%02d.%07d", h, mi, s, hundred_ns);
}

// "YYYY-MM-DDTHH:MM:SS.fffffff" — used in the SQL literal CAST forms.
std::string FormatTimestampIsoT(timestamp_t ts) {
	date_t date_part;
	dtime_t time_part;
	Timestamp::Convert(ts, date_part, time_part);

	int32_t y, mo, d;
	Date::Convert(date_part, y, mo, d);

	int32_t h, mi, s, us;
	Time::Convert(time_part, h, mi, s, us);
	int32_t hundred_ns = us * 10;

	return StringUtil::Format("%04d-%02d-%02dT%02d:%02d:%02d.%07d", y, mo, d, h, mi, s, hundred_ns);
}

// "YYYY-MM-DD HH:MM:SS.fffffff" (space separator) — used by RenderAsString
// for the issue-#89 fallback so the rendered string is the natural sortable
// text form for downstream string consumers.
std::string FormatTimestampText(timestamp_t ts) {
	date_t date_part;
	dtime_t time_part;
	Timestamp::Convert(ts, date_part, time_part);

	int32_t y, mo, d;
	Date::Convert(date_part, y, mo, d);

	int32_t h, mi, s, us;
	Time::Convert(time_part, h, mi, s, us);
	int32_t hundred_ns = us * 10;

	return StringUtil::Format("%04d-%02d-%02d %02d:%02d:%02d.%07d", y, mo, d, h, mi, s, hundred_ns);
}

std::string FormatTzOffset(int32_t offset_seconds) {
	char sign = offset_seconds >= 0 ? '+' : '-';
	int32_t abs_off = std::abs(offset_seconds);
	int32_t oh = abs_off / 3600;
	int32_t om = (abs_off % 3600) / 60;
	return StringUtil::Format("%c%02d:%02d", sign, oh, om);
}

}  // namespace

//===----------------------------------------------------------------------===//
// Shared TIMESTAMP_* helpers — used by both DecodeFromTds (wire bytes → native
// target unit) and EncodeToBcp (native source unit → wire components).
//
// DuckDB stores each TIMESTAMP_* variant as an int64 in the type's native unit
// (TIMESTAMP_SEC=seconds, TIMESTAMP_MS=ms, TIMESTAMP=µs, TIMESTAMP_NS=ns); all
// share the `timestamp_t` physical wrapper, the unit is carried by the
// LogicalType. We do all encoding / decoding math at the variant's native
// precision so DATETIME2(7) ↔ TIMESTAMP_NS round-trips losslessly.
//===----------------------------------------------------------------------===//

constexpr int64_t SECONDS_PER_DAY = 86400LL;
constexpr int32_t DAYS_FROM_0001_TO_EPOCH = 719162;

int64_t TicksPerSecondFor(LogicalTypeId id) {
	switch (id) {
	case LogicalTypeId::TIMESTAMP_SEC:
		return 1LL;
	case LogicalTypeId::TIMESTAMP_MS:
		return 1000LL;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		return 1000000LL;
	case LogicalTypeId::TIMESTAMP_NS:
		return 1000000000LL;
	default:
		throw InternalException("TicksPerSecondFor: not a TIMESTAMP variant");
	}
}

int64_t Pow10(uint8_t exp) {
	int64_t out = 1;
	for (uint8_t i = 0; i < exp; i++) {
		out *= 10;
	}
	return out;
}

void ComputeDatetime2Components(int64_t raw_ticks, LogicalTypeId source_type, uint8_t target_scale,
								uint64_t &time_value, uint32_t &date_value) {
	int64_t source_per_sec = TicksPerSecondFor(source_type);
	int64_t source_per_day = source_per_sec * SECONDS_PER_DAY;

	int64_t days, sub_day_ticks;
	if (raw_ticks >= 0) {
		days = raw_ticks / source_per_day;
		sub_day_ticks = raw_ticks % source_per_day;
	} else {
		days = (raw_ticks - source_per_day + 1) / source_per_day;
		sub_day_ticks = raw_ticks - days * source_per_day;
	}
	date_value = static_cast<uint32_t>(days + DAYS_FROM_0001_TO_EPOCH);

	int64_t target_per_sec = Pow10(target_scale);
	if (target_per_sec >= source_per_sec) {
		// Upscale (e.g. TIMESTAMP_MS → DATETIME2(7)) — exact.
		time_value = static_cast<uint64_t>(sub_day_ticks * (target_per_sec / source_per_sec));
	} else {
		// Downscale (e.g. TIMESTAMP_NS → DATETIME2(3)) — integer truncation.
		time_value = static_cast<uint64_t>(sub_day_ticks / (source_per_sec / target_per_sec));
	}
}

//===----------------------------------------------------------------------===//
// DecodeFromTds
//===----------------------------------------------------------------------===//

// Convert a DATETIME2 wire payload (variable-length time portion + 3-byte date)
// directly into the target DuckDB TIMESTAMP_* variant's native int64 unit,
// bypassing the µs-intermediate that ConvertDatetime2 hard-codes. This is
// lossless whenever target precision ≥ wire precision (DATETIME2(7) → TIMESTAMP_NS
// preserves every 100-ns tick; DATETIME2(3) → TIMESTAMP_MS preserves every ms).
int64_t Datetime2WireToNativeUnit(const uint8_t *data, size_t time_len, uint8_t wire_scale, LogicalTypeId target_type) {
	int64_t time_ticks = 0;
	for (size_t i = 0; i < time_len; i++) {
		time_ticks |= static_cast<int64_t>(data[i]) << (i * 8);
	}
	int32_t days = static_cast<int32_t>(data[time_len]) | (static_cast<int32_t>(data[time_len + 1]) << 8) |
				   (static_cast<int32_t>(data[time_len + 2]) << 16);
	int32_t unix_days = days - DAYS_FROM_0001_TO_EPOCH;

	int64_t wire_per_sec = Pow10(wire_scale);
	int64_t target_per_sec = TicksPerSecondFor(target_type);

	int64_t sub_day_ticks;
	if (target_per_sec >= wire_per_sec) {
		sub_day_ticks = time_ticks * (target_per_sec / wire_per_sec);
	} else {
		sub_day_ticks = time_ticks / (wire_per_sec / target_per_sec);
	}
	return static_cast<int64_t>(unix_days) * SECONDS_PER_DAY * target_per_sec + sub_day_ticks;
}

size_t Datetime2TimeByteLen(uint8_t scale) {
	if (scale <= 2)
		return 3;
	if (scale <= 4)
		return 4;
	return 5;
}

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row) {
	switch (col.type_id) {
	case TDS_TYPE_DATE: {
		date_t d = tds::encoding::DateTimeEncoding::ConvertDate(bytes.data());
		FlatVector::GetData<date_t>(out)[row] = d;
		return;
	}
	case TDS_TYPE_TIME: {
		dtime_t t = tds::encoding::DateTimeEncoding::ConvertTime(bytes.data(), col.scale);
		FlatVector::GetData<dtime_t>(out)[row] = t;
		return;
	}
	case TDS_TYPE_DATETIME: {
		// DATETIME wire (~3 ms precision) always decodes to TIMESTAMP (µs).
		timestamp_t ts = tds::encoding::DateTimeEncoding::ConvertDatetime(bytes.data());
		FlatVector::GetData<timestamp_t>(out)[row] = ts;
		return;
	}
	case TDS_TYPE_SMALLDATETIME: {
		timestamp_t ts = tds::encoding::DateTimeEncoding::ConvertSmallDatetime(bytes.data());
		FlatVector::GetData<timestamp_t>(out)[row] = ts;
		return;
	}
	case TDS_TYPE_DATETIME2: {
		// Convert wire bytes directly into the target's native unit (TIMESTAMP_S /
		// TIMESTAMP_MS / TIMESTAMP / TIMESTAMP_NS) so the µs-bottleneck doesn't
		// drop precision on DATETIME2(7) → TIMESTAMP_NS round-trips.
		size_t time_len = Datetime2TimeByteLen(col.scale);
		int64_t native = Datetime2WireToNativeUnit(bytes.data(), time_len, col.scale, out.GetType().id());
		FlatVector::GetData<timestamp_t>(out)[row] = timestamp_t(native);
		return;
	}
	case TDS_TYPE_DATETIMEN: {
		// Length-dispatched: 4 → smalldatetime, 8 → datetime. Both have fixed
		// precision below µs so always decode to TIMESTAMP.
		timestamp_t ts;
		if (bytes.size() == 8) {
			ts = tds::encoding::DateTimeEncoding::ConvertDatetime(bytes.data());
		} else if (bytes.size() == 4) {
			ts = tds::encoding::DateTimeEncoding::ConvertSmallDatetime(bytes.data());
		} else {
			throw InvalidInputException("Invalid DATETIMEN length: %d", bytes.size());
		}
		FlatVector::GetData<timestamp_t>(out)[row] = ts;
		return;
	}
	case TDS_TYPE_DATETIMEOFFSET: {
		// DuckDB has no nanosecond TZ type; catalog always maps DATETIMEOFFSET
		// to TIMESTAMP_TZ (µs). Existing decoder returns UTC µs which fits.
		timestamp_t ts = tds::encoding::DateTimeEncoding::ConvertDatetimeOffset(bytes.data(), col.scale);
		FlatVector::GetData<timestamp_t>(out)[row] = ts;
		return;
	}
	default:
		throw InvalidInputException("codec::datetime::DecodeFromTds: unexpected TDS type 0x%02X", col.type_id);
	}
}

//===----------------------------------------------------------------------===//
// EncodeToBcp — Vector overload
//===----------------------------------------------------------------------===//

void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf) {
	switch (col.duckdb_type.id()) {
	case LogicalTypeId::DATE:
		tds::encoding::BCPRowEncoder::EncodeDate(buf, GetVectorValue<date_t>(in, row));
		return;
	case LogicalTypeId::TIME:
		tds::encoding::BCPRowEncoder::EncodeTime(buf, GetVectorValue<dtime_t>(in, row), col.scale);
		return;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_SEC: {
		uint64_t time_value;
		uint32_t date_value;
		ComputeDatetime2Components(GetVectorValue<timestamp_t>(in, row).value, col.duckdb_type.id(), col.scale,
								   time_value, date_value);
		tds::encoding::BCPRowEncoder::EncodeDatetime2Raw(buf, time_value, date_value, col.scale);
		return;
	}
	case LogicalTypeId::TIMESTAMP_TZ: {
		// DuckDB stores TIMESTAMP_TZ as UTC µs; offset 0 on the wire.
		uint64_t time_value;
		uint32_t date_value;
		ComputeDatetime2Components(GetVectorValue<timestamp_t>(in, row).value, LogicalTypeId::TIMESTAMP_TZ, col.scale,
								   time_value, date_value);
		tds::encoding::BCPRowEncoder::EncodeDatetimeOffsetRaw(buf, time_value, date_value, 0, col.scale);
		return;
	}
	default:
		throw NotImplementedException("codec::datetime::EncodeToBcp: unexpected DuckDB type '%s'",
									  col.duckdb_type.ToString());
	}
}

//===----------------------------------------------------------------------===//
// EncodeToBcp — Value overload
//===----------------------------------------------------------------------===//

void EncodeToBcp(const Value &value, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf) {
	// Use the *Value::Get extractors instead of Value::GetValue<T>() —
	// the latter invokes DuckDB cast operators that fail for TIMESTAMP_TZ
	// (no TIMESTAMP_TZ→TIMESTAMP cast is registered in some builds). The
	// *Value::Get helpers grab the raw 64-bit storage directly, which is
	// what we want: all TIMESTAMP_* share an int64 physical representation.
	switch (col.duckdb_type.id()) {
	case LogicalTypeId::DATE:
		tds::encoding::BCPRowEncoder::EncodeDate(buf, DateValue::Get(value));
		return;
	case LogicalTypeId::TIME:
		tds::encoding::BCPRowEncoder::EncodeTime(buf, TimeValue::Get(value), col.scale);
		return;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_SEC: {
		uint64_t time_value;
		uint32_t date_value;
		ComputeDatetime2Components(TimestampValue::Get(value).value, col.duckdb_type.id(), col.scale, time_value,
								   date_value);
		tds::encoding::BCPRowEncoder::EncodeDatetime2Raw(buf, time_value, date_value, col.scale);
		return;
	}
	case LogicalTypeId::TIMESTAMP_TZ: {
		uint64_t time_value;
		uint32_t date_value;
		ComputeDatetime2Components(TimestampValue::Get(value).value, LogicalTypeId::TIMESTAMP_TZ, col.scale, time_value,
								   date_value);
		tds::encoding::BCPRowEncoder::EncodeDatetimeOffsetRaw(buf, time_value, date_value, 0, col.scale);
		return;
	}
	default:
		throw NotImplementedException("codec::datetime::EncodeToBcp(Value): unexpected DuckDB type '%s'",
									  col.duckdb_type.ToString());
	}
}

//===----------------------------------------------------------------------===//
// FormatSqlLiteral
//===----------------------------------------------------------------------===//

// Render any TIMESTAMP_* variant's raw int64 directly into the 7-digit ISO
// CAST-form ("YYYY-MM-DDTHH:MM:SS.fffffff") at the source's native precision.
// Bypasses the µs-only `Timestamp::Convert` / `Time::Convert` chain so
// TIMESTAMP_NS sends every 100-ns tick to the server.
static std::string FormatTimestampIsoTFromSource(int64_t raw, LogicalTypeId source) {
	uint64_t time_value;  // ticks at scale 7 (100 ns)
	uint32_t date_value;  // days since 0001-01-01
	ComputeDatetime2Components(raw, source, /*target_scale=*/7, time_value, date_value);

	int32_t unix_days = static_cast<int32_t>(date_value) - DAYS_FROM_0001_TO_EPOCH;
	date_t d(unix_days);
	int32_t y, mo, day;
	Date::Convert(d, y, mo, day);

	// time_value is in 100-ns ticks since midnight.
	constexpr int64_t HUNDRED_NS_PER_SEC = 10000000LL;
	int64_t sec_in_day = static_cast<int64_t>(time_value) / HUNDRED_NS_PER_SEC;
	int64_t hundred_ns = static_cast<int64_t>(time_value) % HUNDRED_NS_PER_SEC;
	int32_t h = static_cast<int32_t>(sec_in_day / 3600);
	int32_t mi = static_cast<int32_t>((sec_in_day % 3600) / 60);
	int32_t s = static_cast<int32_t>(sec_in_day % 60);

	return StringUtil::Format("%04d-%02d-%02dT%02d:%02d:%02d.%07lld", y, mo, day, h, mi, s,
							  static_cast<long long>(hundred_ns));
}

std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext /*ctx*/) {
	if (v.IsNull()) {
		return "NULL";
	}
	switch (type.id()) {
	case LogicalTypeId::DATE:
		return "'" + FormatDateText(DateValue::Get(v)) + "'";
	case LogicalTypeId::TIME:
		return "'" + FormatTimeText(TimeValue::Get(v)) + "'";
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
		return "CAST('" + FormatTimestampIsoTFromSource(TimestampValue::Get(v).value, type.id()) + "' AS DATETIME2(7))";
	case LogicalTypeId::TIMESTAMP_TZ:
		// DuckDB stores TIMESTAMP_TZ as UTC µs internally; emit offset +00:00.
		return "CAST('" + FormatTimestampIsoTFromSource(TimestampValue::Get(v).value, LogicalTypeId::TIMESTAMP_TZ) +
			   FormatTzOffset(0) + "' AS DATETIMEOFFSET(7))";
	default:
		throw InvalidInputException("codec::datetime::FormatSqlLiteral: unexpected DuckDB type '%s'", type.ToString());
	}
}

//===----------------------------------------------------------------------===//
// FormatDdlTypeName
//===----------------------------------------------------------------------===//

std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig & /*cfg*/, DdlContext /*ctx*/) {
	switch (type.id()) {
	case LogicalTypeId::DATE:
		return "DATE";
	case LogicalTypeId::TIME:
		return "TIME(7)";
	case LogicalTypeId::TIMESTAMP:
		return "DATETIME2(6)";	// μs — DuckDB native, exact match
	case LogicalTypeId::TIMESTAMP_MS:
		return "DATETIME2(3)";	// ms — exact
	case LogicalTypeId::TIMESTAMP_NS:
		return "DATETIME2(7)";	// ns — closest fit, lossy 2 digits (FR-028)
	case LogicalTypeId::TIMESTAMP_SEC:
		return "DATETIME2(0)";	// s  — exact
	case LogicalTypeId::TIMESTAMP_TZ:
		return "DATETIMEOFFSET(7)";
	default:
		throw NotImplementedException("codec::datetime::FormatDdlTypeName: unsupported DuckDB type '%s'",
									  type.ToString());
	}
}

//===----------------------------------------------------------------------===//
// EstimateLiteralSize
//===----------------------------------------------------------------------===//

size_t EstimateLiteralSize(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::DATE:
		return 12;	// 'YYYY-MM-DD'
	case LogicalTypeId::TIME:
		return 20;	// 'HH:MM:SS.fffffff'
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
		return 60;	// CAST('YYYY-MM-DDTHH:MM:SS.fffffff' AS DATETIME2(7))
	case LogicalTypeId::TIMESTAMP_TZ:
		return 75;	// CAST('YYYY-MM-DDTHH:MM:SS.fffffff+HH:MM' AS DATETIMEOFFSET(7))
	default:
		throw InvalidInputException("codec::datetime::EstimateLiteralSize: unexpected DuckDB type '%s'",
									type.ToString());
	}
}

//===----------------------------------------------------------------------===//
// RenderAsString (issue #89 fallback)
//===----------------------------------------------------------------------===//

std::string RenderAsString(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col) {
	switch (col.type_id) {
	case TDS_TYPE_DATE: {
		date_t d = tds::encoding::DateTimeEncoding::ConvertDate(bytes.data());
		return FormatDateText(d);
	}
	case TDS_TYPE_TIME: {
		dtime_t t = tds::encoding::DateTimeEncoding::ConvertTime(bytes.data(), col.scale);
		return FormatTimeText(t);
	}
	case TDS_TYPE_DATETIME: {
		timestamp_t ts = tds::encoding::DateTimeEncoding::ConvertDatetime(bytes.data());
		return FormatTimestampText(ts);
	}
	case TDS_TYPE_SMALLDATETIME: {
		timestamp_t ts = tds::encoding::DateTimeEncoding::ConvertSmallDatetime(bytes.data());
		return FormatTimestampText(ts);
	}
	case TDS_TYPE_DATETIME2: {
		timestamp_t ts = tds::encoding::DateTimeEncoding::ConvertDatetime2(bytes.data(), col.scale);
		return FormatTimestampText(ts);
	}
	case TDS_TYPE_DATETIMEN: {
		timestamp_t ts;
		if (bytes.size() == 8) {
			ts = tds::encoding::DateTimeEncoding::ConvertDatetime(bytes.data());
		} else if (bytes.size() == 4) {
			ts = tds::encoding::DateTimeEncoding::ConvertSmallDatetime(bytes.data());
		} else {
			throw InvalidInputException("Invalid DATETIMEN length: %d", bytes.size());
		}
		return FormatTimestampText(ts);
	}
	case TDS_TYPE_DATETIMEOFFSET: {
		timestamp_t ts = tds::encoding::DateTimeEncoding::ConvertDatetimeOffset(bytes.data(), col.scale);
		// Wire stores UTC + display offset; render UTC text + +00:00 so
		// downstream string consumers see an unambiguous instant.
		return FormatTimestampText(ts) + FormatTzOffset(0);
	}
	default:
		throw InvalidInputException("codec::datetime::RenderAsString: unexpected TDS type 0x%02X", col.type_id);
	}
}

}  // namespace datetime
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
