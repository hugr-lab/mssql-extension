#include "codec/variant/variant_envelope.hpp"

#include "codec/code_page/code_page.hpp"
#include "simdutf.h"
#include "tds/tds_types.hpp"

#include <cstring>
#include <limits>
#include <vector>

namespace duckdb {
namespace mssql {
namespace codec {
namespace variant {

namespace {

constexpr int64_t DAYS_FROM_0001_TO_EPOCH = 719162;	 // 0001-01-01 .. 1970-01-01
constexpr int64_t DAYS_FROM_1900_TO_EPOCH = 25567;	 // 1900-01-01 .. 1970-01-01
constexpr int64_t SECONDS_PER_DAY = 86400;
constexpr int64_t MICROS_PER_DAY = SECONDS_PER_DAY * 1000000;
// A decimal's mantissa: 16 bytes at every precision on 2022 (S1); the column form's 4 / 8 / 12 are
// taken too, so a server that sends those reads as well.
constexpr uint8_t DECIMAL_MANTISSA_BYTES = 16;

uint64_t Load(const uint8_t *p, size_t bytes) {
	uint64_t v = 0;
	for (size_t i = 0; i < bytes; i++) {
		v |= static_cast<uint64_t>(p[i]) << (8 * i);
	}
	return v;
}

template <class T>
void Put(std::string &blob, T v) {
	char bytes[sizeof(T)];
	std::memcpy(bytes, &v, sizeof(T));	// little-endian host, as the read path assumes throughout
	blob.append(bytes, sizeof(T));
}

void PutVarint(std::string &blob, uint32_t v) {
	do {
		uint8_t byte = v & 127;
		v >>= 7;
		if (v != 0) {
			byte |= 128;
		}
		blob.push_back(static_cast<char>(byte));
	} while (v != 0);
}

void PutString(std::string &blob, const char *data, size_t size) {
	PutVarint(blob, static_cast<uint32_t>(size));  // size is within a variant's 8009 bytes
	blob.append(data, size);
}

//! The time part's bytes at a time / datetime2 / datetimeoffset scale (TDS: 3, 4 or 5).
size_t TimeBytes(uint8_t scale) {
	return scale <= 2 ? 3 : scale <= 4 ? 4 : 5;
}

int64_t Pow10(uint8_t n) {
	int64_t v = 1;
	for (uint8_t i = 0; i < n; i++) {
		v *= 10;
	}
	return v;
}

//! 10^p as a 128-bit (hi, lo) pair, for the decimal's range check without __int128 (MSVC has none).
void Pow10Wide(uint8_t p, uint64_t &hi, uint64_t &lo) {
	hi = 0;
	lo = 1;
	for (uint8_t i = 0; i < p; i++) {
		// (hi, lo) *= 10, as (hi, lo) * 8 + (hi, lo) * 2
		const uint64_t lo8 = lo << 3, hi8 = (hi << 3) | (lo >> 61);
		const uint64_t lo2 = lo << 1, hi2 = (hi << 1) | (lo >> 63);
		lo = lo8 + lo2;
		hi = hi8 + hi2 + (lo < lo8 ? 1 : 0);
	}
}

//! A decimal / numeric: sign byte (1 = positive) and a 16-byte mantissa, below 10^precision.
bool EncodeDecimal(uint8_t precision, uint8_t scale, const uint8_t *value, size_t bytes, std::string &blob,
				   std::string &why) {
	const bool positive = value[0] != 0;
	const size_t mantissa = bytes - 1;	// 4, 8, 12 or 16 (SplitTdsVariant)
	const uint64_t lo = Load(value + 1, mantissa < 8 ? mantissa : 8);
	const uint64_t hi = mantissa > 8 ? Load(value + 9, mantissa - 8) : 0;
	uint64_t max_hi, max_lo;
	Pow10Wide(precision, max_hi, max_lo);
	if (hi > max_hi || (hi == max_hi && lo >= max_lo)) {
		why = "a decimal(" + std::to_string(precision) + "," + std::to_string(scale) + ") mantissa past its precision";
		return false;
	}
	PutVarint(blob, precision);
	PutVarint(blob, scale);
	if (precision <= 18) {
		const int64_t v = positive ? static_cast<int64_t>(lo) : -static_cast<int64_t>(lo);	// lo < 10^18
		if (precision <= 4) {
			Put<int16_t>(blob, static_cast<int16_t>(v));
		} else if (precision <= 9) {
			Put<int32_t>(blob, static_cast<int32_t>(v));
		} else {
			Put<int64_t>(blob, v);
		}
		return true;
	}
	// A hugeint: lower (u64) then upper (i64), two's complement for a negative.
	uint64_t out_lo = lo, out_hi = hi;
	if (!positive) {
		out_lo = ~lo + 1;
		out_hi = ~hi + (out_lo == 0 ? 1 : 0);
	}
	Put<uint64_t>(blob, out_lo);
	Put<uint64_t>(blob, out_hi);
	return true;
}

//! A money value as DECIMAL: money is the high i32 then the low u32 of an i64 in 1/10000.
void EncodeMoney(const uint8_t *value, size_t bytes, std::string &blob) {
	if (bytes == 8) {
		const int64_t high = static_cast<int32_t>(Load(value, 4));
		const int64_t v = static_cast<int64_t>((static_cast<uint64_t>(high) << 32) | Load(value + 4, 4));
		PutVarint(blob, 19);
		PutVarint(blob, 4);
		Put<uint64_t>(blob, static_cast<uint64_t>(v));
		Put<int64_t>(blob, v < 0 ? -1 : 0);	 // the hugeint's upper half: width 19 is past int64's 18
	} else {
		PutVarint(blob, 10);
		PutVarint(blob, 4);
		Put<int64_t>(blob, static_cast<int32_t>(Load(value, 4)));
	}
}

//! A time part (`TimeBytes(scale)` bytes, 10^-scale s ticks since midnight) in nanoseconds.
int64_t TimeNanos(const uint8_t *value, uint8_t scale) {
	return static_cast<int64_t>(Load(value, TimeBytes(scale))) * Pow10(static_cast<uint8_t>(9 - scale));
}

//! A date (3 bytes, days since 0001-01-01) as days since 1970-01-01.
int64_t UnixDays(const uint8_t *value) {
	return static_cast<int64_t>(Load(value, 3)) - DAYS_FROM_0001_TO_EPOCH;
}

//! A datetime2 / datetimeoffset instant (`nanos` into the day `days`) at the column map's unit for its
//! scale: seconds (0), milliseconds (1-3), microseconds (4-6), nanoseconds (7) when they fit int64
//! (1677-2262), else microseconds with the 100 ns digit dropped (DQ-44).
void EncodeInstant(int64_t days, int64_t nanos, uint8_t scale, bool tz, std::string &blob, VariantTag &tag) {
	// int64 nanoseconds, checked as the column read checks it (Datetime2WireToNativeUnit): about
	// 1677-09-21 .. 2262-04-11, so a value reads as TIMESTAMP_NANOS here exactly when its column would.
	constexpr int64_t NANOS_PER_DAY = SECONDS_PER_DAY * 1000000000;
	if (scale == 7 && days <= std::numeric_limits<int64_t>::max() / NANOS_PER_DAY &&
		days >= std::numeric_limits<int64_t>::min() / NANOS_PER_DAY &&
		days * NANOS_PER_DAY <= std::numeric_limits<int64_t>::max() - nanos) {
		tag = tz ? VariantTag::TIMESTAMP_NANOS_TZ : VariantTag::TIMESTAMP_NANOS;
		Put<int64_t>(blob, days * NANOS_PER_DAY + nanos);
		return;
	}
	if (tz || scale >= 4) {
		tag = tz ? VariantTag::TIMESTAMP_MICROS_TZ : VariantTag::TIMESTAMP_MICROS;
		Put<int64_t>(blob, days * MICROS_PER_DAY + nanos / 1000);
	} else if (scale == 0) {
		tag = VariantTag::TIMESTAMP_SEC;
		Put<int64_t>(blob, days * SECONDS_PER_DAY + nanos / 1000000000);
	} else {
		tag = VariantTag::TIMESTAMP_MILIS;
		Put<int64_t>(blob, days * SECONDS_PER_DAY * 1000 + nanos / 1000000);
	}
}

//! UTF-16LE to UTF-8, an unpaired surrogate as U+FFFD (the column kernel's rule).
void Utf16ToUtf8(const uint8_t *data, size_t bytes, std::string &out) {
	const size_t units = bytes / 2;
	if (units == 0) {
		out.clear();
		return;
	}
	std::vector<char16_t> buffer(units);
	std::memcpy(buffer.data(), data, units * 2);
	out.resize(units * 3 + 4);
	if (simdutf::validate_utf16le(buffer.data(), units)) {
		out.resize(simdutf::convert_valid_utf16le_to_utf8(buffer.data(), units, &out[0]));
		return;
	}
	size_t n = 0;
	for (size_t i = 0; i < units; i++) {
		uint32_t cp = buffer[i];
		if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units && buffer[i + 1] >= 0xDC00 && buffer[i + 1] <= 0xDFFF) {
			cp = 0x10000 + ((cp - 0xD800) << 10) + (buffer[i + 1] - 0xDC00);
			i++;
		} else if (cp >= 0xD800 && cp <= 0xDFFF) {
			cp = 0xFFFD;
		}
		if (cp < 0x80) {
			out[n++] = static_cast<char>(cp);
		} else if (cp < 0x800) {
			out[n++] = static_cast<char>(0xC0 | (cp >> 6));
			out[n++] = static_cast<char>(0x80 | (cp & 0x3F));
		} else if (cp < 0x10000) {
			out[n++] = static_cast<char>(0xE0 | (cp >> 12));
			out[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			out[n++] = static_cast<char>(0x80 | (cp & 0x3F));
		} else {
			out[n++] = static_cast<char>(0xF0 | (cp >> 18));
			out[n++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
			out[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			out[n++] = static_cast<char>(0x80 | (cp & 0x3F));
		}
	}
	out.resize(n);
}

void TrimSpaces(std::string &s) {
	size_t n = s.size();
	while (n > 0 && s[n - 1] == ' ') {
		n--;
	}
	s.resize(n);
}

//! Properties a base type carries, and the value bytes a fixed one has (0: variable).
struct BaseShape {
	uint8_t property_bytes;
	size_t fixed_value_bytes;
	bool known;
};

BaseShape ShapeOf(uint8_t base_type) {
	using namespace tds;
	switch (base_type) {
	case TDS_TYPE_BIT:
	case TDS_TYPE_TINYINT:
		return {0, 1, true};
	case TDS_TYPE_SMALLINT:
		return {0, 2, true};
	case TDS_TYPE_INT:
	case TDS_TYPE_REAL:
	case TDS_TYPE_SMALLMONEY:
	case TDS_TYPE_SMALLDATETIME:
		return {0, 4, true};
	case TDS_TYPE_BIGINT:
	case TDS_TYPE_FLOAT:
	case TDS_TYPE_MONEY:
	case TDS_TYPE_DATETIME:
		return {0, 8, true};
	case TDS_TYPE_DATE:
		return {0, 3, true};
	case TDS_TYPE_UNIQUEIDENTIFIER:
		return {0, 16, true};
	case TDS_TYPE_DECIMAL:
	case TDS_TYPE_NUMERIC:
		return {2, 0, true};  // 1 + 4, 8, 12 or 16 (SplitTdsVariant)
	case TDS_TYPE_TIME:
	case TDS_TYPE_DATETIME2:
	case TDS_TYPE_DATETIMEOFFSET:
		return {1, 0, true};
	case TDS_TYPE_BIGBINARY:
	case TDS_TYPE_BIGVARBINARY:
		return {2, 0, true};
	case TDS_TYPE_BIGCHAR:
	case TDS_TYPE_BIGVARCHAR:
	case TDS_TYPE_NCHAR:
	case TDS_TYPE_NVARCHAR:
		return {7, 0, true};
	default:
		return {0, 0, false};
	}
}

}  // namespace

bool SplitTdsVariant(const uint8_t *data, size_t size, WireVariant &out, std::string &why) {
	if (size < 2) {
		why = "a sql_variant value of " + std::to_string(size) + " bytes, shorter than its header";
		return false;
	}
	out.base_type = data[0];
	out.property_bytes = data[1];
	const BaseShape shape = ShapeOf(out.base_type);
	if (!shape.known) {
		why = "a sql_variant of base type 0x" + std::string(1, "0123456789ABCDEF"[out.base_type >> 4]) +
			  std::string(1, "0123456789ABCDEF"[out.base_type & 15]) + ", which sql_variant cannot hold";
		return false;
	}
	if (out.property_bytes != shape.property_bytes || size < 2u + out.property_bytes) {
		why = "a sql_variant whose base type has " + std::to_string(shape.property_bytes) + " property bytes, not " +
			  std::to_string(out.property_bytes);
		return false;
	}
	out.properties = data + 2;
	out.value = data + 2 + out.property_bytes;
	out.value_bytes = size - 2 - out.property_bytes;
	size_t expected = shape.fixed_value_bytes;
	if (out.property_bytes == 1) {	// time / datetime2 / datetimeoffset: the scale
		const uint8_t scale = out.properties[0];
		if (scale > 7) {
			why = "a sql_variant time value of scale " + std::to_string(scale);
			return false;
		}
		expected = TimeBytes(scale) + (out.base_type == tds::TDS_TYPE_TIME ? 0 : 3) +
				   (out.base_type == tds::TDS_TYPE_DATETIMEOFFSET ? 2 : 0);
	}
	if (out.property_bytes == 2 && (out.base_type == tds::TDS_TYPE_DECIMAL || out.base_type == tds::TDS_TYPE_NUMERIC) &&
		(out.value_bytes < 5 || out.value_bytes > 1u + DECIMAL_MANTISSA_BYTES || (out.value_bytes - 1) % 4 != 0)) {
		why = "a sql_variant decimal of " + std::to_string(out.value_bytes) + " bytes, not a sign and 4, 8, 12 or 16";
		return false;
	}
	if (expected != 0 && out.value_bytes != expected) {
		why = "a sql_variant value of " + std::to_string(out.value_bytes) + " bytes where its base type has " +
			  std::to_string(expected);
		return false;
	}
	if ((out.base_type == tds::TDS_TYPE_NCHAR || out.base_type == tds::TDS_TYPE_NVARCHAR) && out.value_bytes % 2) {
		why = "a sql_variant UTF-16 value of an odd " + std::to_string(out.value_bytes) + " bytes";
		return false;
	}
	return true;
}

bool EncodeVariant(const WireVariant &v, std::string &blob, VariantTag &tag, EncodeNote &note, std::string &why) {
	using namespace tds;
	const uint8_t *p = v.value;
	switch (v.base_type) {
	case TDS_TYPE_BIT:
		tag = p[0] ? VariantTag::BOOL_TRUE : VariantTag::BOOL_FALSE;
		return true;
	case TDS_TYPE_TINYINT:
		tag = VariantTag::UINT8;
		blob.push_back(static_cast<char>(p[0]));
		return true;
	case TDS_TYPE_SMALLINT:
		tag = VariantTag::INT16;
		blob.append(reinterpret_cast<const char *>(p), 2);
		return true;
	case TDS_TYPE_INT:
		tag = VariantTag::INT32;
		blob.append(reinterpret_cast<const char *>(p), 4);
		return true;
	case TDS_TYPE_BIGINT:
		tag = VariantTag::INT64;
		blob.append(reinterpret_cast<const char *>(p), 8);
		return true;
	case TDS_TYPE_REAL:
		tag = VariantTag::FLOAT;
		blob.append(reinterpret_cast<const char *>(p), 4);
		return true;
	case TDS_TYPE_FLOAT:
		tag = VariantTag::DOUBLE;
		blob.append(reinterpret_cast<const char *>(p), 8);
		return true;
	case TDS_TYPE_DECIMAL:
	case TDS_TYPE_NUMERIC: {
		const uint8_t precision = v.properties[0], scale = v.properties[1];
		if (precision < 1 || precision > 38 || scale > precision) {
			why = "a sql_variant decimal(" + std::to_string(precision) + "," + std::to_string(scale) + ")";
			return false;
		}
		tag = VariantTag::DECIMAL;
		return EncodeDecimal(precision, scale, p, v.value_bytes, blob, why);
	}
	case TDS_TYPE_MONEY:
	case TDS_TYPE_SMALLMONEY:
		tag = VariantTag::DECIMAL;
		EncodeMoney(p, v.value_bytes, blob);
		return true;
	case TDS_TYPE_DATE:
		tag = VariantTag::DATE;
		Put<int32_t>(blob, static_cast<int32_t>(UnixDays(p)));
		return true;
	case TDS_TYPE_TIME: {
		const uint8_t scale = v.properties[0];
		const int64_t nanos = TimeNanos(p, scale);
		if (nanos >= SECONDS_PER_DAY * 1000000000) {
			why = "a sql_variant time past 24:00";
			return false;
		}
		if (scale == 7) {
			tag = VariantTag::TIME_NANOS;
			Put<int64_t>(blob, nanos);
		} else {
			tag = VariantTag::TIME_MICROS;
			Put<int64_t>(blob, nanos / 1000);
		}
		return true;
	}
	case TDS_TYPE_DATETIME2:
	case TDS_TYPE_DATETIMEOFFSET: {
		const uint8_t scale = v.properties[0];
		const size_t time_bytes = TimeBytes(scale);
		const int64_t nanos = TimeNanos(p, scale);
		if (nanos >= SECONDS_PER_DAY * 1000000000) {
			why = "a sql_variant datetime2 whose time is past 24:00";
			return false;
		}
		// A datetimeoffset holds UTC; its offset (the last two bytes) is not part of the instant.
		EncodeInstant(UnixDays(p + time_bytes), nanos, scale, v.base_type == TDS_TYPE_DATETIMEOFFSET, blob, tag);
		return true;
	}
	case TDS_TYPE_DATETIME: {
		// days since 1900-01-01 (i32), then 1/300 s ticks since midnight (u32), read as the column is:
		// ticks * 1000000 / 300, truncated.
		const int64_t days = static_cast<int32_t>(Load(p, 4));
		const int64_t ticks = static_cast<int64_t>(Load(p + 4, 4));
		// datetime holds 1753-01-01 .. 9999-12-31, ticks within a day: anything else is not a datetime
		// (and could overflow the microseconds).
		if (days < -53690 || days > 2958463 || ticks >= 300 * SECONDS_PER_DAY) {
			why = "a sql_variant datetime outside 1753-9999";
			return false;
		}
		tag = VariantTag::TIMESTAMP_MICROS;
		Put<int64_t>(blob, (days - DAYS_FROM_1900_TO_EPOCH) * MICROS_PER_DAY + ticks * 10000 / 3);
		return true;
	}
	case TDS_TYPE_SMALLDATETIME: {
		const int64_t days = static_cast<int64_t>(Load(p, 2)), minutes = static_cast<int64_t>(Load(p + 2, 2));
		tag = VariantTag::TIMESTAMP_MICROS;
		Put<int64_t>(blob, (days - DAYS_FROM_1900_TO_EPOCH) * MICROS_PER_DAY + minutes * 60 * 1000000);
		return true;
	}
	case TDS_TYPE_UNIQUEIDENTIFIER: {
		// Data1..Data3 little-endian, Data4 as is; DuckDB's UUID is the canonical (big-endian) bytes as a
		// hugeint, the top bit flipped (GuidEncoding::ConvertGuid).
		const uint64_t upper = (Load(p, 4) << 32) | (Load(p + 4, 2) << 16) | Load(p + 6, 2);
		uint64_t lower = 0;
		for (size_t i = 8; i < 16; i++) {
			lower = (lower << 8) | p[i];
		}
		tag = VariantTag::UUID;
		Put<uint64_t>(blob, lower);
		Put<uint64_t>(blob, upper ^ (uint64_t(1) << 63));
		return true;
	}
	case TDS_TYPE_BIGBINARY:
	case TDS_TYPE_BIGVARBINARY:
		tag = VariantTag::BLOB;
		PutString(blob, reinterpret_cast<const char *>(p), v.value_bytes);
		return true;
	case TDS_TYPE_NCHAR:
	case TDS_TYPE_NVARCHAR: {
		std::string text;
		Utf16ToUtf8(p, v.value_bytes, text);
		if (v.base_type == TDS_TYPE_NCHAR) {
			TrimSpaces(text);
		}
		tag = VariantTag::VARCHAR;
		PutString(blob, text.data(), text.size());
		return true;
	}
	case TDS_TYPE_BIGCHAR:
	case TDS_TYPE_BIGVARCHAR: {
		// The 5-byte collation: LCID (20 bits) and flags in the first four, SortId in the fifth.
		const uint32_t collation = static_cast<uint32_t>(Load(v.properties, 4));
		const bool utf8 = (collation & 0x04000000u) != 0;
		// UTF-8 by the collation's flag, as the column read decides it (ColumnMetadata::IsUtf8Collation);
		// a code page by the lookup otherwise.
		const int32_t page_number =
			utf8 ? code_page::CODE_PAGE_UTF8
				 : code_page::CodePageForWireCollation(collation & 0xFFFFFu, false, v.properties[4]);
		std::string text;
		if (page_number == code_page::CODE_PAGE_UTF8) {
			if (!simdutf::validate_utf8(reinterpret_cast<const char *>(p), v.value_bytes)) {
				note.invalid_utf8 = true;
				tag = VariantTag::BLOB;
				PutString(blob, reinterpret_cast<const char *>(p), v.value_bytes);
				return true;
			}
			text.assign(reinterpret_cast<const char *>(p), v.value_bytes);
		} else {
			const auto *page = page_number > 0 ? code_page::FindCodePage(page_number) : nullptr;
			if (!page) {
				note.unknown_collation = true;
				tag = VariantTag::BLOB;
				PutString(blob, reinterpret_cast<const char *>(p), v.value_bytes);
				return true;
			}
			text.resize(code_page::Utf8Bound(v.value_bytes));
			text.resize(code_page::DecodeToUtf8(*page, p, v.value_bytes, &text[0]));
		}
		if (v.base_type == TDS_TYPE_BIGCHAR) {
			TrimSpaces(text);
		}
		tag = VariantTag::VARCHAR;
		PutString(blob, text.data(), text.size());
		return true;
	}
	default:
		why = "a sql_variant of a base type it cannot hold";
		return false;
	}
}

}  // namespace variant
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
