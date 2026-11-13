#include "backup/backup_function_common.hpp"

#include "backup/backup_bytes.hpp"
#include "backup/backup_collation.hpp"
#include "catalog/mssql_column_info.hpp"
#include "codec/code_page/code_page.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "tds/encoding/datetime_encoding.hpp"
#include "utf8proc_wrapper.hpp"

#include <algorithm>
#include <cctype>
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
		// Every page the server stores a varchar in has a table, double-byte ones included: a backup has
		// no server to convert it, so the client does (spec 092's direction for the backup reader).
		const auto *page = codec::code_page::FindCodePage(static_cast<int32_t>(collation->code_page));
		if (!page) {
			throw FormatError("backup: " + what + " is a varchar of code page " + std::to_string(collation->code_page) +
							  ", which has no table");
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

namespace {

// The type column `c` is read as, its readability aside: converted, or its stored bytes (`raw`).
ColumnReading TypeReading(const CatalogColumn &c) {
	ColumnReading r;
	const std::string type = c.system_type_name.empty()
								 ? (c.type_name.empty() ? "type " + std::to_string(c.user_type_id) : c.type_name)
								 : c.system_type_name;
	auto raw = [&](const std::string &why) {
		r.raw = true;
		r.type = LogicalType::BLOB;
		r.note = "its stored bytes, not converted: " + why;
		return r;
	};
	switch (c.system_type_id) {
	case XTYPE_SQL_VARIANT:
		return raw("sql_variant (its base type, properties and value) is converted later, with the TDS path's");
	case XTYPE_XML:
		return raw("xml (stored as binary XML) is converted to text later, with the TDS path's");
	case XTYPE_CLR:
		return raw(type + " (its CLR serialization) is converted later, with the TDS path's");
	default:
		break;
	}
	if (c.system_type_name.empty()) {
		return raw(type + " is a type the reader does not know");
	}
	if (c.system_type_name == "json" || c.system_type_name == "vector") {
		return raw(type + " is converted later, with the TDS path's");
	}
	// Only a storage type the record decoder knows is converted: the kernels are keyed by the TDS id the
	// xtype is passed as, and an xtype that is no storage type (a TDS-only id -- INTN, BITN, the legacy
	// forms -- in a crafted catalog) would be decoded as a form its bytes are not. Any other is its bytes
	// (review of #552).
	if (!KnownFixedXtype(c.system_type_id) && !KnownVariableXtype(c.system_type_id)) {
		return raw("xtype " + std::to_string(c.system_type_id) + " is not a storage type the reader decodes");
	}
	// A code-page text is converted through its collation's page: one the reader has no table for, or no
	// collation at all, is its bytes -- never bytes of another page in a VARCHAR, which is UTF-8 (#224;
	// review of #552).
	const bool code_page_text =
		c.system_type_id == XTYPE_VARCHAR || c.system_type_id == XTYPE_CHAR || c.system_type_id == XTYPE_TEXT;
	if (code_page_text) {
		const CollationRow *collation = c.collation_id ? CollationById(static_cast<int32_t>(c.collation_id)) : nullptr;
		if (!collation) {
			return raw(c.collation_id ? "its collation (CollationId " + std::to_string(c.collation_id) +
											") is newer than the code-page tables"
									  : std::string("it has no collation"));
		}
		if (collation->code_page != 65001 &&
			!codec::code_page::FindCodePage(static_cast<int32_t>(collation->code_page))) {
			return raw("its code page " + std::to_string(collation->code_page) + " has no table");
		}
	}
	// The precision and scale are the file's: LogicalType::DECIMAL asserts what they must be (an
	// assertion, not an exception), so a declaration no server writes is caught here. By the name too,
	// which the type map dispatches on and the file's own sysscalartypes supplies (review of #549).
	std::string name = c.system_type_name;
	std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return std::tolower(ch); });
	const bool decimal = c.system_type_id == XTYPE_DECIMAL || c.system_type_id == XTYPE_NUMERIC || name == "decimal" ||
						 name == "numeric";
	const bool scaled = c.system_type_id == XTYPE_TIME || c.system_type_id == XTYPE_DATETIME2 ||
						c.system_type_id == XTYPE_DATETIMEOFFSET || name == "time" || name == "datetime2" ||
						name == "datetimeoffset";
	if ((decimal && (c.precision < 1 || c.precision > 38 || c.scale > c.precision)) || (scaled && c.scale > 7)) {
		return raw(type + " is declared with precision " + std::to_string(c.precision) + " and scale " +
				   std::to_string(c.scale) + ", which no SQL Server type has");
	}
	try {
		// datetime2(7) as TIMESTAMP_NS until spec 093 T8 hooks mssql_datetime2_7_as into the backup scan.
		r.type = MSSQLColumnInfo::MapSQLServerTypeToDuckDB(c.system_type_name, c.max_length, c.precision, c.scale,
														   MSSQLDatetime2Read::Nanoseconds);
	} catch (std::exception &e) {
		return raw(type + " has no DuckDB type here (" + std::string(e.what()) + ")");
	}
	return r;
}

}  // namespace

ColumnReading ReadingOf(const CatalogColumn &c) {
	// What is not readable still says the type it is declared as (raw included).
	ColumnReading r = TypeReading(c);
	auto not_readable = [&](const std::string &why) {
		r.readable = false;
		r.note = why;
		return r;
	};
	if (c.is_computed && !c.is_persisted) {
		return not_readable("a computed column that is not persisted has no stored value");
	}
	if (c.is_sparse || c.is_column_set) {
		return not_readable("a sparse column's value is in the row's sparse vector, read from spec 086");
	}
	if (c.system_type_id == XTYPE_TEXT || c.system_type_id == XTYPE_NTEXT || c.system_type_id == XTYPE_IMAGE) {
		return not_readable("a text, ntext or image value is read from spec 085 PR 3 (the large values)");
	}
	return r;
}

//! A type whose in-row value is variable-length, so the server can push it off row (row-overflow).
static bool IsVariable(const CatalogColumn &c) {
	switch (c.system_type_id) {
	case XTYPE_VARCHAR:
	case XTYPE_NVARCHAR:
	case XTYPE_VARBINARY:
	case XTYPE_SQL_VARIANT:
	case XTYPE_CLR:
		return true;
	default:
		return false;
	}
}

//! A row-overflow pointer's in-row size: a value no longer than it is never pushed off row.
static constexpr uint64_t OFF_ROW_POINTER = 24;

//! Whether `t`'s declared columns can make a row pass the 8,060 bytes a page holds in row, so a
//! variable value can be pushed off row (the server moves the widest value off row then, which can
//! be any of them: review of task 16d, the documentation names no order). Counted generously, so
//! `converted` is never promised for a value that can go off row: a MAX column's in-row share (8,000
//! bytes), a text / ntext / image pointer (16), a sparse column in the sparse vector (its width and
//! 4), and a record's overhead -- header 4, column count 2, the null bitmap, the variable columns'
//! count 2 and an offset of 2 each, a uniquifier (6) and a version tag (14). From the declared
//! columns: a dropped column's bytes, kept in old records until a rebuild, are not counted.
static bool RowCanOverflow(const CatalogTable &t) {
	uint64_t bytes = 4 + 2 + 2 + 6 + 14;
	uint64_t columns = 0;
	for (const auto &c : t.columns) {
		if (c.is_computed && !c.is_persisted) {
			continue;
		}
		if (c.is_sparse || c.is_column_set) {
			bytes += 4 + (c.max_length < 0 ? 8000 : static_cast<uint64_t>(c.max_length));
			continue;
		}
		columns++;
		bytes += c.max_length < 0 ? 8000 : static_cast<uint64_t>(c.max_length);
		if (IsVariable(c) || c.max_length < 0) {
			bytes += 2;
		}
	}
	bytes += (columns + 7) / 8;
	return bytes > 8060;
}

ColumnState StateOf(const CatalogTable &t, const CatalogColumn &c) {
	const ColumnReading r = ReadingOf(c);
	ColumnState s;
	auto join = [](const std::string &a, const std::string &b) { return a.empty() ? b : b.empty() ? a : a + "; " + b; };
	const bool lob =
		c.system_type_id == XTYPE_TEXT || c.system_type_id == XTYPE_NTEXT || c.system_type_id == XTYPE_IMAGE;
	std::string off_row;
	if (!lob && c.max_length < 0) {
		off_row =
			"a value stored off row (past its in-row share, or out of row by the table's option) is refused by "
			"name until spec 085 PR 3; one in row is read";
	} else if (!lob && IsVariable(c) && static_cast<uint64_t>(c.max_length) > OFF_ROW_POINTER && RowCanOverflow(t)) {
		off_row =
			"the table's row can pass 8,060 bytes, so a value can be pushed off row (row-overflow): refused by "
			"name until spec 085 PR 3; one in row is read";
	}
	if (c.is_computed && !c.is_persisted) {
		s.state = "not_stored";
		s.reason = r.note;
	} else if (c.is_sparse || c.is_column_set) {
		s.state = "storage_format";
		s.reason = r.note;
	} else if (lob) {
		s.state = "off_row_pending";
		s.reason = r.note;
	} else if (r.raw) {
		s.state = "stored_bytes";
		s.reason = join(r.note, off_row);
	} else if (!off_row.empty()) {
		s.state = "off_row_pending";
		s.reason = off_row;
	}
	if (!t.decodable) {
		s.reason = join(s.reason, "the table is not read: " + t.reason);
	}
	return s;
}

Value LsnValue(const Lsn &lsn) {
	// HEADERONLY's form: vlf, then block in 10 digits, then slot in 5 (< 10^25).
	const hugeint_t value =
		hugeint_t(static_cast<int64_t>(lsn.vlf)) * hugeint_t(static_cast<int64_t>(1000000000000000LL)) +
		hugeint_t(static_cast<int64_t>(lsn.block)) * hugeint_t(static_cast<int64_t>(100000)) +
		hugeint_t(static_cast<int64_t>(lsn.slot));
	return Value::DECIMAL(value, 25, 0);
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
