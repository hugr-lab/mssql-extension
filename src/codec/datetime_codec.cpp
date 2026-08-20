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

#include "codec/scatter_position.hpp"

#include "codec/vector_format.hpp"
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
#include <limits>
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
		return;
	}
	// Downscale (e.g. TIMESTAMP_NS → DATETIME2(3)): ROUND, do not truncate.
	//
	// SQL Server's own conversion rounds — CAST('12:00:00.1239999' AS
	// datetime2(3)) is .124, and CAST('12:00:00.9999999' AS datetime2(0)) is
	// 12:00:01, crossing the second. Truncating gave .123 and 12:00:00, so the
	// same data loaded by COPY and by INSERT ... CAST disagreed, silently, on
	// anything with sub-second precision finer than the target.
	//
	// It also disagreed with this extension's own decimal path, which narrows by
	// rounding for exactly this reason. One rule now: a narrowing conversion
	// lands where the server would have put it.
	//
	// `sub_day_ticks` is non-negative (it is the remainder within a day), so a
	// plain +half is correct and no sign handling is needed.
	//
	// Rounding CAN carry past the end of the day: 23:59:59.999999 into scale 3
	// rounds to 86 400 000 ms, which is 24:00:00 and not a legal time — the
	// server rejects the row outright ("returned invalid data for column"). SQL
	// Server rolls the DATE instead: CAST('2024-01-15 23:59:59.9999999' AS
	// datetime2(3)) is 2024-01-16 00:00:00.000, verified against the server. So
	// carry into the date rather than emitting an out-of-range time.
	const int64_t divisor = source_per_sec / target_per_sec;
	int64_t scaled = (sub_day_ticks + divisor / 2) / divisor;
	const int64_t target_per_day = target_per_sec * SECONDS_PER_DAY;
	if (scaled >= target_per_day) {
		scaled = 0;
		date_value += 1;
	}
	time_value = static_cast<uint64_t>(scaled);
}

//===----------------------------------------------------------------------===//
// Columnar scatter (spec 057 step 3)
//
// One call per COLUMN per block, with every column-invariant hoisted. What the
// per-value path was paying for constants:
//
//   * `EncodeTime` computed its divisor with a LOOP — `for (i < 6 - scale)
//     divisor *= 10` — up to six multiplies per value for a number fixed by the
//     column's scale.
//   * `GetTimeByteSize(scale)` ran per value.
//   * `ComputeDatetime2Components` recomputed source_per_sec, source_per_day,
//     target_per_sec and the scale DIRECTION per value, plus a division.
//   * every byte of the payload went out through its own push_back.
//
// All of those are metadata. Here they are resolved once and the loop writes the
// payload with two sized memcpys — the time field and the 3-byte date — whose
// widths are template parameters.
//===----------------------------------------------------------------------===//

namespace {

template <bool UPSCALE, int TIME_BYTES, bool HAS_SEL, class POS>
void ScatterDt2(POS pos, bool all_valid, idx_t row_begin, idx_t rows, const UnifiedVectorFormat &fmt, int64_t per_day,
				int64_t factor, int64_t half, int64_t per_day_target, uint8_t total_size) {
	const int64_t *src = reinterpret_cast<const int64_t *>(fmt.data);
	const SelectionVector *sel = fmt.sel;
	for (idx_t r = 0; r < rows; r++) {
		uint8_t *out = pos.At(r);
		const idx_t idx = HAS_SEL ? sel->get_index_unsafe(row_begin + r) : row_begin + r;
		if (!all_valid && !fmt.validity.RowIsValid(idx)) {
			*out = 0x00;
			pos.Advance(r, 1);
			continue;
		}
		const int64_t raw = src[idx];
		int64_t days;
		int64_t sub;
		if (raw >= 0) {
			days = raw / per_day;
			sub = raw % per_day;
		} else {
			days = (raw - per_day + 1) / per_day;
			sub = raw - days * per_day;
		}
		uint32_t date_value = static_cast<uint32_t>(days + DAYS_FROM_0001_TO_EPOCH);
		// Narrowing ROUNDS, matching SQL Server, and a round that reaches the end
		// of the day carries into the date — see ComputeDatetime2Components for
		// both, verified against the server. `half` and `per_day_target` are
		// column constants hoisted by the caller, so this costs nothing per value
		// beyond one compare that is false for every value but the last tick.
		uint64_t time_value;
		if (UPSCALE) {
			time_value = static_cast<uint64_t>(sub * factor);
		} else {
			int64_t scaled = (sub + half) / factor;
			if (scaled >= per_day_target) {
				scaled = 0;
				date_value += 1;
			}
			time_value = static_cast<uint64_t>(scaled);
		}
		out[0] = total_size;
		std::memcpy(out + 1, &time_value, TIME_BYTES);
		// Three bytes, not four: a uint32 memcpy would clobber the next column.
		std::memcpy(out + 1 + TIME_BYTES, &date_value, 3);
		pos.Advance(r, 1 + static_cast<size_t>(total_size));
	}
}

template <bool UPSCALE, int TIME_BYTES, class POS>
void ScatterDt2Sel(POS pos, bool all_valid, idx_t row_begin, idx_t rows, const UnifiedVectorFormat &fmt,
				   int64_t per_day, int64_t factor, int64_t half, int64_t per_day_target, uint8_t total_size) {
	if (fmt.sel->IsSet()) {
		ScatterDt2<UPSCALE, TIME_BYTES, true>(pos, all_valid, row_begin, rows, fmt, per_day, factor, half,
											  per_day_target, total_size);
	} else {
		ScatterDt2<UPSCALE, TIME_BYTES, false>(pos, all_valid, row_begin, rows, fmt, per_day, factor, half,
											   per_day_target, total_size);
	}
}

// TIME(scale): the scaled tick count since midnight, no date field. DuckDB's
// dtime_t is microseconds, so the same factor/half/direction logic applies —
// TIME simply stops after the time field.
template <int TIME_BYTES, bool UPSCALE, bool HAS_SEL, class POS>
void ScatterTime(POS pos, bool all_valid, idx_t row_begin, idx_t rows, const UnifiedVectorFormat &fmt, int64_t factor,
				 int64_t half, int64_t per_day) {
	const int64_t *src = reinterpret_cast<const int64_t *>(fmt.data);
	const SelectionVector *sel = fmt.sel;
	for (idx_t r = 0; r < rows; r++) {
		uint8_t *out = pos.At(r);
		const idx_t idx = HAS_SEL ? sel->get_index_unsafe(row_begin + r) : row_begin + r;
		if (!all_valid && !fmt.validity.RowIsValid(idx)) {
			*out = 0x00;
			pos.Advance(r, 1);
			continue;
		}
		const int64_t micros = src[idx];
		uint64_t ticks =
			UPSCALE ? static_cast<uint64_t>(micros * factor) : static_cast<uint64_t>((micros + half) / factor);
		// Rounding at the last tick of the day has nowhere to carry — a TIME has
		// no date field, which is exactly why its datetime2 and datetimeoffset
		// peers can carry and this cannot. SQL Server SATURATES rather than
		// wrapping: CAST('23:59:59.999999' AS time(0)) is 23:59:59, not 24:00:00
		// and not 00:00:00. Without this the kernel emitted 86400, which is
		// outside the type's domain in either direction.
		if (ticks >= static_cast<uint64_t>(per_day)) {
			ticks = static_cast<uint64_t>(per_day) - 1;
		}
		out[0] = TIME_BYTES;
		std::memcpy(out + 1, &ticks, TIME_BYTES);
		pos.Advance(r, 1 + TIME_BYTES);
	}
}

// DATETIMEOFFSET(scale): the datetime2 payload plus a 2-byte signed offset in
// minutes. DuckDB stores TIMESTAMP_TZ as UTC microseconds and carries no
// per-value zone, so the offset is always 0 — the same answer the row path
// gives.
//
// Fabric Warehouse has no datetimeoffset type at all, so this arm simply never
// resolves against such a target. That is a property of the catalog rather than
// something to special-case here.
template <bool UPSCALE, int TIME_BYTES, bool HAS_SEL, class POS>
void ScatterDtOffset(POS pos, bool all_valid, idx_t row_begin, idx_t rows, const UnifiedVectorFormat &fmt,
					 int64_t per_day, int64_t factor, int64_t half, int64_t per_day_target, uint8_t total_size) {
	const int64_t *src = reinterpret_cast<const int64_t *>(fmt.data);
	const SelectionVector *sel = fmt.sel;
	for (idx_t r = 0; r < rows; r++) {
		uint8_t *out = pos.At(r);
		const idx_t idx = HAS_SEL ? sel->get_index_unsafe(row_begin + r) : row_begin + r;
		if (!all_valid && !fmt.validity.RowIsValid(idx)) {
			*out = 0x00;
			pos.Advance(r, 1);
			continue;
		}
		const int64_t raw = src[idx];
		int64_t days;
		int64_t sub;
		if (raw >= 0) {
			days = raw / per_day;
			sub = raw % per_day;
		} else {
			days = (raw - per_day + 1) / per_day;
			sub = raw - days * per_day;
		}
		uint32_t date_value = static_cast<uint32_t>(days + DAYS_FROM_0001_TO_EPOCH);
		uint64_t time_value;
		if (UPSCALE) {
			time_value = static_cast<uint64_t>(sub * factor);
		} else {
			int64_t scaled = (sub + half) / factor;
			if (scaled >= per_day_target) {
				scaled = 0;
				date_value += 1;
			}
			time_value = static_cast<uint64_t>(scaled);
		}
		out[0] = total_size;
		std::memcpy(out + 1, &time_value, TIME_BYTES);
		std::memcpy(out + 1 + TIME_BYTES, &date_value, 3);
		out[1 + TIME_BYTES + 3] = 0;
		out[1 + TIME_BYTES + 4] = 0;
		pos.Advance(r, 1 + static_cast<size_t>(total_size));
	}
}

template <bool HAS_SEL, class POS>
void ScatterDate(POS pos, bool all_valid, idx_t row_begin, idx_t rows, const UnifiedVectorFormat &fmt) {
	const int32_t *src = reinterpret_cast<const int32_t *>(fmt.data);
	const SelectionVector *sel = fmt.sel;
	for (idx_t r = 0; r < rows; r++) {
		uint8_t *out = pos.At(r);
		const idx_t idx = HAS_SEL ? sel->get_index_unsafe(row_begin + r) : row_begin + r;
		if (!all_valid && !fmt.validity.RowIsValid(idx)) {
			*out = 0x00;
			pos.Advance(r, 1);
			continue;
		}
		const uint32_t days = static_cast<uint32_t>(src[idx] + DAYS_FROM_0001_TO_EPOCH);
		out[0] = 3;
		std::memcpy(out + 1, &days, 3);
		pos.Advance(r, 4);
	}
}

// A DATE source into a DATETIME2 target — advertised by IsTypeCompatible and, in
// the row path, previously read as a timestamp_t out of an int32 vector, which
// aborted the COPY with `INTERNAL Error: Expected unified vector format of type
// INT64, but found type INT32`. Issue #153's shape in the temporal family.
//
// It needs no scale arithmetic at all: a DATE is midnight, so the time field is
// zero at every scale and only the day number is computed. Worth a kernel rather
// than the row path because ONE unresolvable column drops the whole chunk off the
// columnar path, so a single `date -> datetime2` column would have deoptimised
// every other column in the table.
template <int TIMESIZE, bool HAS_SEL, class POS>
void ScatterDateAsDt2(POS pos, bool all_valid, idx_t row_begin, idx_t rows, const UnifiedVectorFormat &fmt) {
	const int32_t *src = reinterpret_cast<const int32_t *>(fmt.data);
	const SelectionVector *sel = fmt.sel;
	for (idx_t r = 0; r < rows; r++) {
		uint8_t *out = pos.At(r);
		const idx_t idx = HAS_SEL ? sel->get_index_unsafe(row_begin + r) : row_begin + r;
		if (!all_valid && !fmt.validity.RowIsValid(idx)) {
			*out = 0x00;
			pos.Advance(r, 1);
			continue;
		}
		const uint32_t days = static_cast<uint32_t>(src[idx] + DAYS_FROM_0001_TO_EPOCH);
		out[0] = TIMESIZE + 3;
		std::memset(out + 1, 0, TIMESIZE);
		std::memcpy(out + 1 + TIMESIZE, &days, 3);
		pos.Advance(r, 1 + TIMESIZE + 3);
	}
}

}  // namespace

template <class POS>
static void ScatterChunkPos(POS pos, bool all_valid, idx_t row_begin, idx_t rows, Vector &in,
							const UnifiedVectorFormat &fmt, const mssql::BCPColumnMetadata &col) {
	if (col.duckdb_type.id() == LogicalTypeId::DATE) {
		if (fmt.sel->IsSet()) {
			ScatterDate<true>(pos, all_valid, row_begin, rows, fmt);
		} else {
			ScatterDate<false>(pos, all_valid, row_begin, rows, fmt);
		}
		return;
	}

	if (col.duckdb_type.id() == LogicalTypeId::TIME) {
		// dtime_t is microseconds; the target's scale decides the tick unit.
		const int64_t target_per_sec = Pow10(col.scale);
		const bool up = target_per_sec >= 1000000;
		const int64_t f = up ? target_per_sec / 1000000 : 1000000 / target_per_sec;
		const int64_t h = up ? 0 : f / 2;
		const int64_t per_day = target_per_sec * SECONDS_PER_DAY;
		const uint8_t tb = tds::encoding::BCPRowEncoder::GetTimeByteSize(col.scale);
		const bool has_sel = fmt.sel->IsSet();
		if (tb == 3) {
			if (up && has_sel) {
				ScatterTime<3, true, true>(pos, all_valid, row_begin, rows, fmt, f, h, per_day);
			} else if (up) {
				ScatterTime<3, true, false>(pos, all_valid, row_begin, rows, fmt, f, h, per_day);
			} else if (has_sel) {
				ScatterTime<3, false, true>(pos, all_valid, row_begin, rows, fmt, f, h, per_day);
			} else {
				ScatterTime<3, false, false>(pos, all_valid, row_begin, rows, fmt, f, h, per_day);
			}
		} else if (tb == 4) {
			if (up && has_sel) {
				ScatterTime<4, true, true>(pos, all_valid, row_begin, rows, fmt, f, h, per_day);
			} else if (up) {
				ScatterTime<4, true, false>(pos, all_valid, row_begin, rows, fmt, f, h, per_day);
			} else if (has_sel) {
				ScatterTime<4, false, true>(pos, all_valid, row_begin, rows, fmt, f, h, per_day);
			} else {
				ScatterTime<4, false, false>(pos, all_valid, row_begin, rows, fmt, f, h, per_day);
			}
		} else {
			if (up && has_sel) {
				ScatterTime<5, true, true>(pos, all_valid, row_begin, rows, fmt, f, h, per_day);
			} else if (up) {
				ScatterTime<5, true, false>(pos, all_valid, row_begin, rows, fmt, f, h, per_day);
			} else if (has_sel) {
				ScatterTime<5, false, true>(pos, all_valid, row_begin, rows, fmt, f, h, per_day);
			} else {
				ScatterTime<5, false, false>(pos, all_valid, row_begin, rows, fmt, f, h, per_day);
			}
		}
		return;
	}

	if (in.GetType().id() == LogicalTypeId::DATE) {
		// See ScatterDateAsDt2. Handled before TicksPerSecondFor, which has no
		// answer for a DATE and would throw.
		const uint8_t tsz = tds::encoding::BCPRowEncoder::GetTimeByteSize(col.scale);
		const bool sel_set = fmt.sel->IsSet();
		if (tsz == 3) {
			sel_set ? ScatterDateAsDt2<3, true>(pos, all_valid, row_begin, rows, fmt)
					: ScatterDateAsDt2<3, false>(pos, all_valid, row_begin, rows, fmt);
		} else if (tsz == 4) {
			sel_set ? ScatterDateAsDt2<4, true>(pos, all_valid, row_begin, rows, fmt)
					: ScatterDateAsDt2<4, false>(pos, all_valid, row_begin, rows, fmt);
		} else {
			sel_set ? ScatterDateAsDt2<5, true>(pos, all_valid, row_begin, rows, fmt)
					: ScatterDateAsDt2<5, false>(pos, all_valid, row_begin, rows, fmt);
		}
		return;
	}

	const int64_t source_per_sec = TicksPerSecondFor(in.GetType().id());
	const int64_t per_day = source_per_sec * SECONDS_PER_DAY;
	const int64_t target_per_sec = Pow10(col.scale);
	const bool upscale = target_per_sec >= source_per_sec;
	const int64_t factor = upscale ? target_per_sec / source_per_sec : source_per_sec / target_per_sec;
	const int64_t half = upscale ? 0 : factor / 2;
	const int64_t per_day_target = target_per_sec * SECONDS_PER_DAY;
	const uint8_t time_size = tds::encoding::BCPRowEncoder::GetTimeByteSize(col.scale);
	const uint8_t total = static_cast<uint8_t>(time_size + 3);

	if (col.duckdb_type.id() == LogicalTypeId::TIMESTAMP_TZ) {
		const uint8_t dto_total = static_cast<uint8_t>(time_size + 3 + 2);
		const bool has_sel = fmt.sel->IsSet();
		if (time_size == 3) {
			if (upscale && has_sel) {
				ScatterDtOffset<true, 3, true>(pos, all_valid, row_begin, rows, fmt, per_day, factor, half,
											   per_day_target, dto_total);
			} else if (upscale) {
				ScatterDtOffset<true, 3, false>(pos, all_valid, row_begin, rows, fmt, per_day, factor, half,
												per_day_target, dto_total);
			} else if (has_sel) {
				ScatterDtOffset<false, 3, true>(pos, all_valid, row_begin, rows, fmt, per_day, factor, half,
												per_day_target, dto_total);
			} else {
				ScatterDtOffset<false, 3, false>(pos, all_valid, row_begin, rows, fmt, per_day, factor, half,
												 per_day_target, dto_total);
			}
		} else if (time_size == 4) {
			if (upscale && has_sel) {
				ScatterDtOffset<true, 4, true>(pos, all_valid, row_begin, rows, fmt, per_day, factor, half,
											   per_day_target, dto_total);
			} else if (upscale) {
				ScatterDtOffset<true, 4, false>(pos, all_valid, row_begin, rows, fmt, per_day, factor, half,
												per_day_target, dto_total);
			} else if (has_sel) {
				ScatterDtOffset<false, 4, true>(pos, all_valid, row_begin, rows, fmt, per_day, factor, half,
												per_day_target, dto_total);
			} else {
				ScatterDtOffset<false, 4, false>(pos, all_valid, row_begin, rows, fmt, per_day, factor, half,
												 per_day_target, dto_total);
			}
		} else {
			if (upscale && has_sel) {
				ScatterDtOffset<true, 5, true>(pos, all_valid, row_begin, rows, fmt, per_day, factor, half,
											   per_day_target, dto_total);
			} else if (upscale) {
				ScatterDtOffset<true, 5, false>(pos, all_valid, row_begin, rows, fmt, per_day, factor, half,
												per_day_target, dto_total);
			} else if (has_sel) {
				ScatterDtOffset<false, 5, true>(pos, all_valid, row_begin, rows, fmt, per_day, factor, half,
												per_day_target, dto_total);
			} else {
				ScatterDtOffset<false, 5, false>(pos, all_valid, row_begin, rows, fmt, per_day, factor, half,
												 per_day_target, dto_total);
			}
		}
		return;
	}

#define MSSQL_DT2_ARM(up, tb) \
	ScatterDt2Sel<up, tb>(pos, all_valid, row_begin, rows, fmt, per_day, factor, half, per_day_target, total)
	if (upscale) {
		switch (time_size) {
		case 3:
			MSSQL_DT2_ARM(true, 3);
			return;
		case 4:
			MSSQL_DT2_ARM(true, 4);
			return;
		default:
			MSSQL_DT2_ARM(true, 5);
			return;
		}
	}
	switch (time_size) {
	case 3:
		MSSQL_DT2_ARM(false, 3);
		return;
	case 4:
		MSSQL_DT2_ARM(false, 4);
		return;
	default:
		MSSQL_DT2_ARM(false, 5);
		return;
	}
#undef MSSQL_DT2_ARM
}

void ScatterChunkStrided(uint8_t *dst, size_t stride, idx_t row_begin, idx_t rows, Vector &in,
						 const UnifiedVectorFormat &fmt, const mssql::BCPColumnMetadata &col) {
	// Strided implies all-valid: a NULL would have made the rows differ in length.
	ScatterChunkPos(StridePos{dst, stride}, /*all_valid=*/true, row_begin, rows, in, fmt, col);
}

void ScatterChunkCursor(uint8_t *dst, size_t *cursor, idx_t row_begin, idx_t rows, Vector &in,
						const UnifiedVectorFormat &fmt, const mssql::BCPColumnMetadata &col, bool all_valid) {
	ScatterChunkPos(CursorPos{dst, cursor, row_begin}, all_valid, row_begin, rows, in, fmt, col);
}

//===----------------------------------------------------------------------===//
// DecodeFromTds
//===----------------------------------------------------------------------===//

// Convert a DATETIME2 wire payload (variable-length time portion + 3-byte date)
// directly into the target DuckDB TIMESTAMP_* variant's native int64 unit,
// bypassing the µs-intermediate that ConvertDatetime2 hard-codes. This is
// lossless whenever target precision ≥ wire precision (DATETIME2(7) → TIMESTAMP_NS
// preserves every 100-ns tick; DATETIME2(3) → TIMESTAMP_MS preserves every ms).
//
// Returns false (leaving out_native untouched) when the value falls outside the
// target variant's int64 range. SQL Server's datetime2 domain runs to
// 9999-12-31, but TIMESTAMP_NS (int64 nanoseconds) only reaches ~2262-04-11, so
// the far-future valid-to sentinel that temporal tables use (9999-12-31
// 23:59:59.9999999) overflows. Caller emits SQL NULL rather than letting the
// multiply silently wrap to a nonsense past instant (issue #168).
bool Datetime2WireToNativeUnit(const uint8_t *data, size_t time_len, uint8_t wire_scale, LogicalTypeId target_type,
							   int64_t &out_native) {
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

	// Overflow-checked: out_native = unix_days * SECONDS_PER_DAY * target_per_sec + sub_day_ticks.
	// ticks_per_day is at most 86400 * 1e9 ≈ 8.64e13 (fits int64); the unbounded
	// factor is unix_days (±~2.9M over the full [0001,9999] datetime2 domain),
	// which overflows int64 only for TIMESTAMP_NS outside ~[1677,2262].
	int64_t ticks_per_day = SECONDS_PER_DAY * target_per_sec;
	if (unix_days > std::numeric_limits<int64_t>::max() / ticks_per_day ||
		unix_days < std::numeric_limits<int64_t>::min() / ticks_per_day) {
		return false;
	}
	int64_t days_ticks = static_cast<int64_t>(unix_days) * ticks_per_day;
	// sub_day_ticks is always >= 0, so only positive-direction add overflow is possible.
	if (days_ticks > std::numeric_limits<int64_t>::max() - sub_day_ticks) {
		return false;
	}
	out_native = days_ticks + sub_day_ticks;
	return true;
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
		FlatVector::GetDataMutable<date_t>(out)[row] = d;
		return;
	}
	case TDS_TYPE_TIME: {
		dtime_t t = tds::encoding::DateTimeEncoding::ConvertTime(bytes.data(), col.scale);
		FlatVector::GetDataMutable<dtime_t>(out)[row] = t;
		return;
	}
	case TDS_TYPE_DATETIME: {
		// DATETIME wire (~3 ms precision) always decodes to TIMESTAMP (µs).
		timestamp_t ts = tds::encoding::DateTimeEncoding::ConvertDatetime(bytes.data());
		FlatVector::GetDataMutable<timestamp_t>(out)[row] = ts;
		return;
	}
	case TDS_TYPE_SMALLDATETIME: {
		timestamp_t ts = tds::encoding::DateTimeEncoding::ConvertSmallDatetime(bytes.data());
		FlatVector::GetDataMutable<timestamp_t>(out)[row] = ts;
		return;
	}
	case TDS_TYPE_DATETIME2: {
		// Convert wire bytes directly into the target's native unit (TIMESTAMP_S /
		// TIMESTAMP_MS / TIMESTAMP / TIMESTAMP_NS) so the µs-bottleneck doesn't
		// drop precision on DATETIME2(7) → TIMESTAMP_NS round-trips.
		size_t time_len = Datetime2TimeByteLen(col.scale);
		int64_t native;
		if (!Datetime2WireToNativeUnit(bytes.data(), time_len, col.scale, out.GetType().id(), native)) {
			// Value outside the target TIMESTAMP variant's range — e.g. a
			// datetime2(7) far-future value (9999-12-31 temporal-table ValidTo
			// sentinel) mapped to TIMESTAMP_NS, whose domain ends at ~2262.
			// Emit SQL NULL instead of a silently-wrapped wrong instant (issue #168).
			FlatVector::SetNull(out, row, true);
			return;
		}
		FlatVector::GetDataMutable<timestamp_t>(out)[row] = timestamp_t(native);
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
		FlatVector::GetDataMutable<timestamp_t>(out)[row] = ts;
		return;
	}
	case TDS_TYPE_DATETIMEOFFSET: {
		// DuckDB has no nanosecond TZ type; catalog always maps DATETIMEOFFSET
		// to TIMESTAMP_TZ (µs). Existing decoder returns UTC µs which fits.
		timestamp_t ts = tds::encoding::DateTimeEncoding::ConvertDatetimeOffset(bytes.data(), col.scale);
		FlatVector::GetDataMutable<timestamp_t>(out)[row] = ts;
		return;
	}
	default:
		throw InvalidInputException("codec::datetime::DecodeFromTds: unexpected TDS type 0x%02X", col.type_id);
	}
}

//===----------------------------------------------------------------------===//
// DecodeChunkFromStaging — one kernel per column, no dispatch in the row loop
//===----------------------------------------------------------------------===//

void DecodeChunkFromStaging(const staging::ColumnStaging &st, idx_t count, const tds::ColumnMetadata &col,
							Vector &out) {
	const uint8_t *const base = st.buffer.data();
	const uint32_t stride = st.stride;

	switch (col.type_id) {
	case TDS_TYPE_DATE: {
		date_t *result = FlatVector::GetDataMutable<date_t>(out);
		for (idx_t row = 0; row < count; row++) {
			result[row] = tds::encoding::DateTimeEncoding::ConvertDate(base + row * stride);
		}
		return;
	}
	case TDS_TYPE_TIME: {
		dtime_t *result = FlatVector::GetDataMutable<dtime_t>(out);
		for (idx_t row = 0; row < count; row++) {
			result[row] = tds::encoding::DateTimeEncoding::ConvertTime(base + row * stride, col.scale);
		}
		return;
	}
	case TDS_TYPE_DATETIME: {
		timestamp_t *result = FlatVector::GetDataMutable<timestamp_t>(out);
		for (idx_t row = 0; row < count; row++) {
			result[row] = tds::encoding::DateTimeEncoding::ConvertDatetime(base + row * stride);
		}
		return;
	}
	case TDS_TYPE_SMALLDATETIME: {
		timestamp_t *result = FlatVector::GetDataMutable<timestamp_t>(out);
		for (idx_t row = 0; row < count; row++) {
			result[row] = tds::encoding::DateTimeEncoding::ConvertSmallDatetime(base + row * stride);
		}
		return;
	}
	case TDS_TYPE_DATETIMEN: {
		// Length-dispatched in the per-value path, but the width is a property of
		// the COLUMN, so the choice is made once here instead of per value.
		timestamp_t *result = FlatVector::GetDataMutable<timestamp_t>(out);
		if (stride == 8) {
			for (idx_t row = 0; row < count; row++) {
				result[row] = tds::encoding::DateTimeEncoding::ConvertDatetime(base + row * stride);
			}
		} else {
			for (idx_t row = 0; row < count; row++) {
				result[row] = tds::encoding::DateTimeEncoding::ConvertSmallDatetime(base + row * stride);
			}
		}
		return;
	}
	case TDS_TYPE_DATETIME2: {
		// The one temporal kernel that is not total: a datetime2 beyond the
		// target variant's range becomes SQL NULL (issue #168), so this loop has
		// to know which rows carry real values rather than a stale slot.
		timestamp_t *result = FlatVector::GetDataMutable<timestamp_t>(out);
		const size_t time_len = Datetime2TimeByteLen(col.scale);
		const LogicalTypeId target = out.GetType().id();
		for (idx_t row = 0; row < count; row++) {
			if (!st.IsValid(row)) {
				continue;
			}
			int64_t native;
			if (!Datetime2WireToNativeUnit(base + row * stride, time_len, col.scale, target, native)) {
				FlatVector::SetNull(out, row, true);
				continue;
			}
			result[row] = timestamp_t(native);
		}
		return;
	}
	case TDS_TYPE_DATETIMEOFFSET: {
		timestamp_t *result = FlatVector::GetDataMutable<timestamp_t>(out);
		for (idx_t row = 0; row < count; row++) {
			result[row] = tds::encoding::DateTimeEncoding::ConvertDatetimeOffset(base + row * stride, col.scale);
		}
		return;
	}
	default:
		throw InvalidInputException("codec::datetime::DecodeChunkFromStaging: unexpected TDS type 0x%02X", col.type_id);
	}
}

//===----------------------------------------------------------------------===//
// EncodeToBcp — Vector overload
//===----------------------------------------------------------------------===//

void EncodeToBcp(Vector &in, const UnifiedVectorFormat &fmt, idx_t row, const mssql::BCPColumnMetadata &col,
				 duckdb::vector<uint8_t> &buf) {
	// The raw int64 in the vector is counted in the SOURCE type's unit; the wire
	// form and its scale come from the target column. Those are two different
	// types and only coincide when the column metadata was built from the source
	// types — which is why CTAS was correct and COPY was not (issue #231). COPY
	// re-reads the created table from sys.columns, where every datetime2 maps
	// back to TIMESTAMP whatever its scale, so a TIMESTAMP_MS value was read as
	// microseconds: 2024-01-15 arrived as 1970-01-20.
	const LogicalTypeId source_id = in.GetType().id();
	switch (col.duckdb_type.id()) {
	case LogicalTypeId::DATE:
		tds::encoding::BCPRowEncoder::EncodeDate(buf, FormatValue<date_t>(fmt, row));
		return;
	case LogicalTypeId::TIME:
		tds::encoding::BCPRowEncoder::EncodeTime(buf, FormatValue<dtime_t>(fmt, row), col.scale);
		return;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_SEC: {
		uint64_t time_value;
		uint32_t date_value;
		if (source_id == LogicalTypeId::DATE) {
			// A DATE is midnight of its day: no sub-day component to scale, and
			// nothing for ComputeDatetime2Components to do (it would also throw,
			// having no ticks-per-second for a DATE).
			time_value = 0;
			date_value = static_cast<uint32_t>(FormatValue<date_t>(fmt, row).days + DAYS_FROM_0001_TO_EPOCH);
			tds::encoding::BCPRowEncoder::EncodeDatetime2Raw(buf, time_value, date_value, col.scale);
			return;
		}
		ComputeDatetime2Components(FormatValue<timestamp_t>(fmt, row).value, source_id, col.scale, time_value,
								   date_value);
		tds::encoding::BCPRowEncoder::EncodeDatetime2Raw(buf, time_value, date_value, col.scale);
		return;
	}
	case LogicalTypeId::TIMESTAMP_TZ: {
		// DuckDB stores TIMESTAMP_TZ as UTC µs; offset 0 on the wire.
		uint64_t time_value;
		uint32_t date_value;
		ComputeDatetime2Components(FormatValue<timestamp_t>(fmt, row).value, source_id, col.scale, time_value,
								   date_value);
		tds::encoding::BCPRowEncoder::EncodeDatetimeOffsetRaw(buf, time_value, date_value, 0, col.scale);
		return;
	}
	default:
		throw NotImplementedException("codec::datetime::EncodeToBcp: unexpected DuckDB type '%s'",
									  col.duckdb_type.ToString());
	}
}

void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf) {
	EncodeToBcpViaFormat(EncodeToBcp, in, row, col, buf);
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
		// Same source-vs-target distinction as the fmt overload above (#231);
		// here the source unit comes from the Value's own type.
		ComputeDatetime2Components(TimestampValue::Get(value).value, value.type().id(), col.scale, time_value,
								   date_value);
		tds::encoding::BCPRowEncoder::EncodeDatetime2Raw(buf, time_value, date_value, col.scale);
		return;
	}
	case LogicalTypeId::TIMESTAMP_TZ: {
		uint64_t time_value;
		uint32_t date_value;
		ComputeDatetime2Components(TimestampValue::Get(value).value, value.type().id(), col.scale, time_value,
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
		return "TIME(6)";  // µs — DuckDB native, exact match (see DATETIME2 below)
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

std::string RenderAsString(const uint8_t *bytes, size_t size, const tds::ColumnMetadata &col) {
	switch (col.type_id) {
	case TDS_TYPE_DATE: {
		date_t d = tds::encoding::DateTimeEncoding::ConvertDate(bytes);
		return FormatDateText(d);
	}
	case TDS_TYPE_TIME: {
		dtime_t t = tds::encoding::DateTimeEncoding::ConvertTime(bytes, col.scale);
		return FormatTimeText(t);
	}
	case TDS_TYPE_DATETIME: {
		timestamp_t ts = tds::encoding::DateTimeEncoding::ConvertDatetime(bytes);
		return FormatTimestampText(ts);
	}
	case TDS_TYPE_SMALLDATETIME: {
		timestamp_t ts = tds::encoding::DateTimeEncoding::ConvertSmallDatetime(bytes);
		return FormatTimestampText(ts);
	}
	case TDS_TYPE_DATETIME2: {
		timestamp_t ts = tds::encoding::DateTimeEncoding::ConvertDatetime2(bytes, col.scale);
		return FormatTimestampText(ts);
	}
	case TDS_TYPE_DATETIMEN: {
		timestamp_t ts;
		if (size == 8) {
			ts = tds::encoding::DateTimeEncoding::ConvertDatetime(bytes);
		} else if (size == 4) {
			ts = tds::encoding::DateTimeEncoding::ConvertSmallDatetime(bytes);
		} else {
			throw InvalidInputException("Invalid DATETIMEN length: %d", size);
		}
		return FormatTimestampText(ts);
	}
	case TDS_TYPE_DATETIMEOFFSET: {
		timestamp_t ts = tds::encoding::DateTimeEncoding::ConvertDatetimeOffset(bytes, col.scale);
		// Wire stores UTC + display offset; render UTC text + +00:00 so
		// downstream string consumers see an unambiguous instant.
		return FormatTimestampText(ts) + FormatTzOffset(0);
	}
	default:
		throw InvalidInputException("codec::datetime::RenderAsString: unexpected TDS type 0x%02X", col.type_id);
	}
}

std::string RenderAsString(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col) {
	return RenderAsString(bytes.data(), bytes.size(), col);
}

}  // namespace datetime
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
