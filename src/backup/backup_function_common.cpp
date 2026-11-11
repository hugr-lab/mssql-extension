#include "backup/backup_function_common.hpp"

#include "backup/backup_bytes.hpp"
#include "backup/backup_collation.hpp"
#include "catalog/mssql_column_info.hpp"
#include "codec/code_page/code_page.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "tds/encoding/datetime_encoding.hpp"
#include "utf8proc_wrapper.hpp"

#include <cstdio>
#include <cstring>

namespace duckdb {
namespace mssql {
namespace backup {

vector<std::string> PathsOf(const Value &value, const char *function) {
	vector<std::string> paths;
	if (value.IsNull()) {
		throw InvalidInputException("%s: the path is NULL", function);
	}
	if (value.type().id() == LogicalTypeId::LIST) {
		for (const auto &child : ListValue::GetChildren(value)) {
			if (child.IsNull()) {
				throw InvalidInputException("%s: a NULL in the list of paths", function);
			}
			paths.push_back(child.ToString());
		}
		if (paths.empty()) {
			throw InvalidInputException("%s: an empty list of paths", function);
		}
	} else {
		paths.push_back(value.ToString());
	}
	return paths;
}

Value RecordDatetimeValue(const std::string &record_form) {
	if (record_form.size() != 8) {
		return Value(LogicalType::TIMESTAMP);
	}
	uint8_t wire[8];
	std::memcpy(wire, record_form.data() + 4, 4);
	std::memcpy(wire + 4, record_form.data(), 4);
	return Value::TIMESTAMP(tds::encoding::DateTimeEncoding::ConvertDatetime(wire));
}

Value RecordSmallDatetimeValue(const std::string &record_form) {
	if (record_form.size() != 4) {
		return Value(LogicalType::TIMESTAMP);
	}
	uint8_t wire[4];
	std::memcpy(wire, record_form.data() + 2, 2);
	std::memcpy(wire + 2, record_form.data(), 2);
	return Value::TIMESTAMP(tds::encoding::DateTimeEncoding::ConvertSmallDatetime(wire));
}

Value GuidValue(const std::string &bytes, const std::string &what) {
	if (bytes.size() != 16) {
		throw FormatError("backup: " + what + " holds " + std::to_string(bytes.size()) +
						  " bytes, not a uniqueidentifier's 16");
	}
	const uint8_t *b = reinterpret_cast<const uint8_t *>(bytes.data());
	char text[37];
	snprintf(text, sizeof(text), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[3], b[2],
			 b[1], b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
	return Value(text).DefaultCastAs(LogicalType::UUID);
}

namespace {

constexpr int64_t DAYS_0001_TO_EPOCH = 719162;
constexpr int64_t SECONDS_PER_DAY = 86400;
constexpr int64_t DAYS_0001_TO_9999_12_31 = 3652058;  // date's last day, counted from 0001-01-01

//! The bytes a time of `scale` takes (3, 4 or 5), as in TDS.
size_t TimeBytes(uint8_t scale) {
	return scale <= 2 ? 3 : scale <= 4 ? 4 : 5;
}

uint64_t LittleEndian(const uint8_t *data, size_t n) {
	uint64_t v = 0;
	for (size_t i = 0; i < n; i++) {
		v |= static_cast<uint64_t>(data[i]) << (8 * i);
	}
	return v;
}

int64_t Pow10(uint8_t n) {
	int64_t p = 1;
	for (uint8_t i = 0; i < n; i++) {
		p *= 10;
	}
	return p;
}

// The days of a date / datetime2 / datetimeoffset (3 bytes since 0001-01-01), within the type's
// range (0001-01-01 .. 9999-12-31), as days since 1970-01-01.
int64_t Days(const uint8_t *b, const std::string &what) {
	const int64_t days = static_cast<int64_t>(LittleEndian(b, 3));
	if (days > DAYS_0001_TO_9999_12_31) {
		throw FormatError("backup: " + what + " is a date past 9999-12-31 (day " + std::to_string(days) + ")");
	}
	return days - DAYS_0001_TO_EPOCH;
}

// The units of 10^-scale s since midnight of a time part (`n` bytes), within one day; at scale 7 a
// value with a 100 ns digit is refused, not cut to the microsecond the DuckDB types hold (a partition
// boundary cut so would put the rows between the two in the wrong partition; review of #551).
int64_t TimeUnits(const uint8_t *b, size_t n, uint8_t scale, const std::string &what) {
	const int64_t units = static_cast<int64_t>(LittleEndian(b, n));
	if (units >= SECONDS_PER_DAY * Pow10(scale)) {
		throw FormatError("backup: " + what + " is a time past midnight (" + std::to_string(units) + " units of 10^-" +
						  std::to_string(scale) + " s)");
	}
	return units;
}

void Require(const SqlVariant &v, size_t size, const std::string &what) {
	if (v.value.size() != size) {
		throw FormatError("backup: " + what + " holds " + std::to_string(v.value.size()) + " bytes of xtype " +
						  std::to_string(v.xtype) + ", which takes " + std::to_string(size));
	}
}

}  // namespace

Value SqlVariantValue(const SqlVariant &v, const std::string &what) {
	const uint8_t *b = reinterpret_cast<const uint8_t *>(v.value.data());
	const ByteReader r(b, v.value.size(), what.c_str());
	switch (v.xtype) {
	case 48:  // tinyint
		Require(v, 1, what);
		return Value::UTINYINT(r.U8(0));
	case 52:  // smallint
		Require(v, 2, what);
		return Value::SMALLINT(r.I16(0));
	case 56:  // int
		Require(v, 4, what);
		return Value::INTEGER(r.I32(0));
	case 127:  // bigint
		Require(v, 8, what);
		return Value::BIGINT(r.I64(0));
	case 104:  // bit
		Require(v, 1, what);
		return Value::BOOLEAN(r.U8(0) != 0);
	case 59: {	// real
		Require(v, 4, what);
		float f;
		std::memcpy(&f, b, 4);
		return Value::FLOAT(f);
	}
	case 62: {	// float
		Require(v, 8, what);
		double d;
		std::memcpy(&d, b, 8);
		return Value::DOUBLE(d);
	}
	case 60:  // money: i64 of 1/10,000
		Require(v, 8, what);
		return Value::DECIMAL(r.I64(0), 19, 4);
	case 122:  // smallmoney: i32 of 1/10,000
		Require(v, 4, what);
		return Value::DECIMAL(static_cast<int64_t>(r.I32(0)), 10, 4);
	case 106:	 // decimal
	case 108: {	 // numeric: a sign byte (1 positive), then the magnitude, little-endian
		if (v.value.size() < 2 || v.value.size() > 17 || v.precision < 1 || v.precision > 38 || v.scale > v.precision) {
			throw FormatError("backup: " + what + " is a decimal of " + std::to_string(v.value.size()) +
							  " bytes, precision " + std::to_string(v.precision) + ", scale " +
							  std::to_string(v.scale));
		}
		const size_t n = v.value.size() - 1;
		hugeint_t magnitude(0);
		magnitude.lower = LittleEndian(b + 1, std::min<size_t>(n, 8));
		magnitude.upper = n > 8 ? static_cast<int64_t>(LittleEndian(b + 9, n - 8)) : 0;
		if (magnitude.upper < 0) {
			throw FormatError("backup: " + what + " is a decimal past 38 digits");
		}
		// The magnitude must fit the precision: a larger one is a crafted value, refused (D13), never cut.
		if (magnitude >= Hugeint::POWERS_OF_TEN[v.precision]) {
			throw FormatError("backup: " + what + " is a decimal(" + std::to_string(v.precision) + ", " +
							  std::to_string(v.scale) + ") whose value has more than " + std::to_string(v.precision) +
							  " digits");
		}
		const hugeint_t value = b[0] == 1 ? magnitude : -magnitude;
		if (v.precision <= 18) {
			return Value::DECIMAL(Hugeint::Cast<int64_t>(value), v.precision, v.scale);
		}
		return Value::DECIMAL(value, v.precision, v.scale);
	}
	case 40:  // date: 3 bytes of days since 0001-01-01
		Require(v, 3, what);
		return Value::DATE(date_t(static_cast<int32_t>(Days(b, what))));
	case 41: {	// time: units of 10^-scale s since midnight
		if (v.scale > 7) {
			throw FormatError("backup: " + what + " is a time of scale " + std::to_string(v.scale));
		}
		Require(v, TimeBytes(v.scale), what);
		const int64_t units = TimeUnits(b, v.value.size(), v.scale, what);
		if (v.scale == 7 && units % 10) {
			throw FormatError("backup: " + what + " is a time(7) with a 100 ns digit, which a TIME cannot hold");
		}
		const int64_t micros = v.scale <= 6 ? units * Pow10(6 - v.scale) : units / 10;
		return Value::TIME(dtime_t(micros));
	}
	case 42:	// datetime2: the time, then 3 bytes of days since 0001-01-01
	case 43: {	// datetimeoffset: the UTC time and date, then i16 minutes of offset
		if (v.scale > 7) {
			throw FormatError("backup: " + what + " is of scale " + std::to_string(v.scale));
		}
		const size_t time_bytes = TimeBytes(v.scale);
		Require(v, time_bytes + 3 + (v.xtype == 43 ? 2 : 0), what);
		const int64_t units = TimeUnits(b, time_bytes, v.scale, what);
		const int64_t days = Days(b + time_bytes, what);
		if (v.xtype == 43) {
			const int16_t offset = static_cast<int16_t>(LittleEndian(b + time_bytes + 3, 2));
			if (offset < -840 || offset > 840) {
				throw FormatError("backup: " + what + " is a datetimeoffset of offset " + std::to_string(offset) +
								  " minutes, outside -14:00..+14:00");
			}
			if (v.scale == 7 && units % 10) {
				throw FormatError("backup: " + what +
								  " is a datetimeoffset(7) with a 100 ns digit, which a TIMESTAMP_TZ cannot hold");
			}
		}
		// TIMESTAMP_NS holds about 1677-2262 (int64 nanoseconds): a datetime2(7) value past it -- a
		// 9999-12-31 sentinel, 0001-01-01 -- is refused by name (D13), where TYPE mssql's read gives NULL.
		if (v.xtype == 42 && v.scale == 7 && (days > 106750 || days < -106751)) {
			throw FormatError("backup: " + what +
							  " is a datetime2(7) outside 1677-2262, which a TIMESTAMP_NS "
							  "cannot hold");
		}
		const int64_t micros_of_day = v.scale <= 6 ? units * Pow10(6 - v.scale) : units / 10;
		if (v.xtype == 43) {  // TIMESTAMP_TZ, µs at the UTC instant (D6)
			return Value::TIMESTAMPTZ(timestamp_tz_t(days * SECONDS_PER_DAY * 1000000 + micros_of_day));
		}
		if (v.scale == 0) {
			return Value::TIMESTAMPSEC(timestamp_sec_t(days * SECONDS_PER_DAY + units));
		}
		if (v.scale <= 3) {
			return Value::TIMESTAMPMS(timestamp_ms_t(days * SECONDS_PER_DAY * 1000 + units * Pow10(3 - v.scale)));
		}
		if (v.scale <= 6) {
			return Value::TIMESTAMP(timestamp_t(days * SECONDS_PER_DAY * 1000000 + micros_of_day));
		}
		return Value::TIMESTAMPNS(timestamp_ns_t(days * SECONDS_PER_DAY * 1000000000 + units * 100));
	}
	case 61:  // datetime, in record form
		Require(v, 8, what);
		return RecordDatetimeValue(v.value);
	case 58:  // smalldatetime, in record form
		Require(v, 4, what);
		return RecordSmallDatetimeValue(v.value);
	case 36:  // uniqueidentifier
		return GuidValue(v.value, what);
	case 165:  // varbinary
	case 173:  // binary
		return Value::BLOB(reinterpret_cast<const_data_ptr_t>(v.value.data()), v.value.size());
	case 231:  // nvarchar
	case 239:  // nchar
		if (v.value.size() % 2) {
			throw FormatError("backup: " + what + " holds an odd number of bytes of UTF-16");
		}
		// A surrogate without its pair is refused: Utf16LeToUtf8 would write U+FFFD for it, a guess.
		for (size_t i = 0; i + 1 < v.value.size(); i += 2) {
			const uint16_t unit = static_cast<uint16_t>(b[i] | (b[i + 1] << 8));
			const bool high = unit >= 0xD800 && unit <= 0xDBFF;
			const bool low = unit >= 0xDC00 && unit <= 0xDFFF;
			const uint16_t next = i + 3 < v.value.size() ? static_cast<uint16_t>(b[i + 2] | (b[i + 3] << 8)) : 0;
			if (low || (high && !(next >= 0xDC00 && next <= 0xDFFF))) {
				throw FormatError("backup: " + what + " is UTF-16 with an unpaired surrogate at byte " +
								  std::to_string(i));
			}
			i += high ? 2 : 0;
		}
		return Value(Utf16LeToUtf8(b, v.value.size(), false));
	case 167:	 // varchar
	case 175: {	 // char: the bytes of its collation's code page
		const CollationRow *collation = CollationById(static_cast<int32_t>(v.collation));
		if (!collation) {
			throw FormatError("backup: " + what + " is a varchar of collation " + std::to_string(v.collation) +
							  ", which the reader does not know");
		}
		if (collation->code_page == 65001) {
			if (!Utf8Proc::IsValid(v.value.data(), v.value.size())) {
				throw FormatError("backup: " + what + " is a UTF-8 varchar that is not UTF-8");
			}
			return Value(v.value);
		}
		const auto *page = codec::code_page::FindCodePage(static_cast<int32_t>(collation->code_page));
		if (!page || codec::code_page::IsDoubleByte(*page)) {
			throw FormatError("backup: " + what + " is a varchar of code page " + std::to_string(collation->code_page) +
							  ", which is not read (spec 086)");
		}
		std::string out(codec::code_page::Utf8Bound(v.value.size()), '\0');
		out.resize(codec::code_page::DecodeToUtf8(*page, b, v.value.size(), &out[0]));
		return Value(out);
	}
	default:
		throw FormatError("backup: " + what + " is a sql_variant of xtype " + std::to_string(v.xtype) +
						  ", which the reader does not read");
	}
}

std::string ColumnRefusal(const CatalogColumn &c) {
	const CollationRow *collation = c.collation_id ? CollationById(static_cast<int32_t>(c.collation_id)) : nullptr;
	const uint32_t code_page = collation ? collation->code_page : 0;
	switch (c.system_type_id) {
	case 98:
		return "sql_variant is not read (spec 086)";
	case 241:
		return "xml is not read (spec 086)";
	case 240:
		return (c.system_type_name.empty() ? "the CLR type " + c.type_name : c.system_type_name) +
			   " is not read (spec 086)";
	case 34:
	case 35:
	case 99:
		return "text, ntext and image are not read yet";
	default:
		break;
	}
	if (c.system_type_name.empty() || c.system_type_name == "json" || c.system_type_name == "vector") {
		return (c.type_name.empty() ? "type " + std::to_string(c.user_type_id) : c.type_name) +
			   " is not read (spec 086)";
	}
	const bool code_page_text = c.system_type_id == 167 || c.system_type_id == 175 || c.system_type_id == 35;
	if (code_page_text && c.collation_id != 0 && !CollationById(static_cast<int32_t>(c.collation_id))) {
		return "its collation (CollationId " + std::to_string(c.collation_id) + ") is not one the reader knows";
	}
	if ((c.system_type_id == 167 || c.system_type_id == 175) &&
		(code_page == 932 || code_page == 936 || code_page == 949 || code_page == 950)) {
		return "a varchar of double-byte code page " + std::to_string(code_page) + " is not read (spec 086)";
	}
	if (c.is_computed && !c.is_persisted) {
		return "a computed column that is not persisted is not stored";
	}
	if (c.is_sparse || c.is_column_set) {
		return "sparse columns are not read (spec 086)";
	}
	return std::string();
}

LogicalType ColumnDuckDBType(const CatalogColumn &c) {
	return MSSQLColumnInfo::MapSQLServerTypeToDuckDB(c.system_type_name, c.max_length, c.precision, c.scale);
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
