#include "codec/variant/variant_write.hpp"

#include "codec/code_page/code_page.hpp"
#include "codec/collation_defaults.hpp"
#include "codec/datetime_codec.hpp"
#include "codec/literal_format.hpp"
#include "copy/target_resolver.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"
#include "simdutf.h"
#include "tds/encoding/bcp_row_encoder.hpp"
#include "tds/encoding/utf16.hpp"
#include "tds/tds_types.hpp"

#include <cmath>
#include <cstring>

namespace duckdb {
namespace mssql {
namespace codec {
namespace variant {

std::array<uint8_t, 5> StringCollationBytes(const std::string &database_collation) {
	std::array<uint8_t, 5> bytes;
	if (!database_collation.empty() &&
		code_page::WireCollationForName(database_collation.data(), database_collation.size(), bytes.data())) {
		return bytes;
	}
	// The fallback, read from the same table once (the name is in it: asserted by test_variant_write).
	static const std::array<uint8_t, 5> FALLBACK = [] {
		std::array<uint8_t, 5> b{};
		const std::string name = MSSQL_DEFAULT_UTF8_COLLATION;
		if (!code_page::WireCollationForName(name.data(), name.size(), b.data())) {
			throw InternalException("MSSQL: %s is not in the build's collation table", name);
		}
		return b;
	}();
	return FALLBACK;
}

namespace {

constexpr int64_t DAYS_FROM_0001_TO_EPOCH = 719162;
constexpr int64_t SECONDS_PER_DAY = 86400;
constexpr size_t MAX_BINARY_BYTES = 8000;
constexpr size_t MAX_NVARCHAR_UNITS = 4000;

//! One VARIANT value in the form it is written: its sql_variant base type and properties, the value as
//! the envelope carries it, and the T-SQL spelling of both for the statement path.
struct WriteForm {
	uint8_t base_type = 0;
	uint8_t properties[7] = {};
	uint8_t property_bytes = 0;
	//! A fixed-width value's envelope bytes (at most 17: a decimal's sign and mantissa).
	char fixed[24] = {};
	uint8_t fixed_size = 0;
	//! A string / blob value: the VARIANT's own bytes, copied once, into the envelope -- held as a string_t
	//! (a short one is inlined in it, so a pointer to it would not outlive FormOf); `utf16` when they are
	//! UTF-8 written as `units` UTF-16LE units (an nvarchar).
	string_t str = string_t(static_cast<uint32_t>(0));
	bool utf16 = false;
	size_t units = 0;
	//! The statement path's spelling, built only when it is asked for (FormatLiteral).
	bool literal_wanted = false;
	std::string type_text;	// "int", "decimal(38,0)", "nvarchar(12)", ...
	std::string literal;
};

[[noreturn]] void Refuse(const std::string &column, VariantLogicalType tag, const std::string &why) {
	throw InvalidInputException(
		"MSSQL: column \"%s\": a VARIANT value of type %s cannot be written to a SQL Server sql_variant: %s", column,
		EnumUtil::ToString(tag), why);
}

void PutByte(WriteForm &w, uint8_t b) {
	w.fixed[w.fixed_size++] = static_cast<char>(b);
}

template <typename T>
void PutLE(WriteForm &w, T v) {
	std::memcpy(w.fixed + w.fixed_size, &v, sizeof(T));	 // little-endian, as every target is
	w.fixed_size += sizeof(T);
}

void PutLE(WriteForm &w, uint64_t v, size_t bytes) {
	for (size_t i = 0; i < bytes; i++) {
		PutByte(w, static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
	}
}

//! TDS time bytes by scale: 3 for 0-2, 4 for 3-4, 5 for 5-7.
size_t TimeBytes(uint8_t scale) {
	return scale <= 2 ? 3 : (scale <= 4 ? 4 : 5);
}

std::string Digits(uint64_t v, size_t width) {
	std::string s = std::to_string(v);
	return s.size() >= width ? s : std::string(width - s.size(), '0') + s;
}

//! 'HH:MM:SS[.fraction]' of `units` (10^scale a second) since midnight.
std::string TimeText(uint64_t units, uint8_t scale) {
	uint64_t per_sec = 1;
	for (uint8_t i = 0; i < scale; i++) {
		per_sec *= 10;
	}
	const uint64_t secs = units / per_sec;
	std::string s = Digits(secs / 3600, 2) + ":" + Digits(secs / 60 % 60, 2) + ":" + Digits(secs % 60, 2);
	if (scale > 0) {
		s += "." + Digits(units % per_sec, scale);
	}
	return s;
}

std::string DateText(uint32_t days_from_0001) {
	int32_t year, month, day;
	Date::Convert(date_t(static_cast<int32_t>(static_cast<int64_t>(days_from_0001) - DAYS_FROM_0001_TO_EPOCH)), year,
				  month, day);
	return Digits(static_cast<uint64_t>(year), 4) + "-" + Digits(static_cast<uint64_t>(month), 2) + "-" +
		   Digits(static_cast<uint64_t>(day), 2);
}

void SetScale(WriteForm &w, uint8_t base, uint8_t scale) {
	w.base_type = base;
	w.properties[0] = scale;
	w.property_bytes = 1;
}

//! A datetime2 / datetimeoffset value of `ticks` in `unit`'s ticks since the epoch, at `scale`.
void Timestamp(WriteForm &w, int64_t ticks, LogicalTypeId unit, uint8_t scale, bool offset, bool literal) {
	uint64_t time_value;
	uint32_t date_value;
	datetime::ComputeDatetime2Components(ticks, unit, scale, time_value, date_value);
	SetScale(w, offset ? tds::TDS_TYPE_DATETIMEOFFSET : tds::TDS_TYPE_DATETIME2, scale);
	PutLE(w, time_value, TimeBytes(scale));
	PutLE(w, date_value, 3);
	if (offset) {
		PutLE(w, 0, 2);
	}
	if (literal) {
		w.type_text = std::string(offset ? "datetimeoffset(" : "datetime2(") + std::to_string(scale) + ")";
		w.literal = "'" + DateText(date_value) + "T" + TimeText(time_value, scale) + (offset ? "+00:00'" : "'");
	}
}

//! A decimal(precision, scale) of unscaled `v`: the sign byte (1 positive) and the 16-byte magnitude.
void PutDecimal(WriteForm &w, const hugeint_t &v, uint8_t precision, uint8_t scale) {
	w.base_type = tds::TDS_TYPE_DECIMAL;
	w.properties[0] = precision;
	w.properties[1] = scale;
	w.property_bytes = 2;
	const bool negative = v < hugeint_t(0);
	const hugeint_t magnitude = negative ? -v : v;
	PutByte(w, negative ? 0 : 1);
	PutLE<uint64_t>(w, magnitude.lower);
	PutLE<uint64_t>(w, static_cast<uint64_t>(magnitude.upper));
	if (w.literal_wanted) {
		w.type_text = "decimal(" + std::to_string(precision) + "," + std::to_string(scale) + ")";
	}
}

//! An integer past bigint: decimal(38,0) when it has at most 38 digits.
void WideInteger(WriteForm &w, const hugeint_t &v, const std::string &text, const std::string &column,
				 VariantLogicalType tag, bool literal) {
	if (v >= Hugeint::POWERS_OF_TEN[38] || v <= -Hugeint::POWERS_OF_TEN[38]) {
		Refuse(column, tag, "its value " + text + " has more than the 38 digits decimal(38,0) holds");
	}
	PutDecimal(w, v, 38, 0);
	if (literal) {
		w.literal = text;
	}
}

void Fixed(WriteForm &w, uint8_t base, const char *type_text) {
	w.base_type = base;
	if (w.literal_wanted) {
		w.type_text = type_text;
	}
}

std::string Quoted(const char *data, size_t size, bool national) {
	std::string s = national ? "N'" : "'";
	for (size_t i = 0; i < size; i++) {
		s += data[i];
		if (data[i] == '\'') {
			s += '\'';
		}
	}
	return s + "'";
}

//! One value to write: where it is in the VARIANT vector, its tag and its bytes, and what a refusal names.
struct Source {
	const UnifiedVariantVectorData &v;
	idx_t row;
	VariantLogicalType tag;
	const_data_ptr_t ptr;
	const std::string &column;
	const std::array<uint8_t, 5> &collation;
	//! Fill w.literal too (the statement path).
	bool literal;
};

//! bit
void FormOfBool(const Source &s, WriteForm &w) {
	const bool value = s.tag == VariantLogicalType::BOOL_TRUE;
	Fixed(w, tds::TDS_TYPE_BIT, "bit");
	PutByte(w, value ? 1 : 0);
	if (s.literal) {
		w.literal = value ? "1" : "0";
	}
}

//! INT8 / INT16 as smallint (tinyint is 0-255 on the server).
void FormOfSmallInt(const Source &s, WriteForm &w) {
	const int16_t x = s.tag == VariantLogicalType::INT8 ? Load<int8_t>(s.ptr) : Load<int16_t>(s.ptr);
	Fixed(w, tds::TDS_TYPE_SMALLINT, "smallint");
	PutLE<int16_t>(w, x);
	if (s.literal) {
		w.literal = std::to_string(x);
	}
}

//! UINT8 as tinyint
void FormOfTinyInt(const Source &s, WriteForm &w) {
	const uint8_t x = Load<uint8_t>(s.ptr);
	Fixed(w, tds::TDS_TYPE_TINYINT, "tinyint");
	PutByte(w, x);
	if (s.literal) {
		w.literal = std::to_string(x);
	}
}

//! INT32 / UINT16 as int
void FormOfInt(const Source &s, WriteForm &w) {
	const int32_t x = s.tag == VariantLogicalType::INT32 ? Load<int32_t>(s.ptr) : Load<uint16_t>(s.ptr);
	Fixed(w, tds::TDS_TYPE_INT, "int");
	PutLE<int32_t>(w, x);
	if (s.literal) {
		w.literal = std::to_string(x);
	}
}

//! INT64 / UINT32 as bigint
void FormOfBigInt(const Source &s, WriteForm &w) {
	const int64_t x = s.tag == VariantLogicalType::INT64 ? Load<int64_t>(s.ptr) : Load<uint32_t>(s.ptr);
	Fixed(w, tds::TDS_TYPE_BIGINT, "bigint");
	PutLE<int64_t>(w, x);
	if (s.literal) {
		w.literal = std::to_string(x);
	}
}

//! UINT64 / INT128 / UINT128 as decimal(38,0), when it fits.
void FormOfWideInteger(const Source &s, WriteForm &w) {
	if (s.tag == VariantLogicalType::UINT64) {
		const uint64_t x = Load<uint64_t>(s.ptr);
		hugeint_t h;  // hugeint_t(uint64_t) would take the int64_t constructor
		h.lower = x;
		h.upper = 0;
		WideInteger(w, h, std::to_string(x), s.column, s.tag, s.literal);
		return;
	}
	if (s.tag == VariantLogicalType::INT128) {
		const hugeint_t x = Load<hugeint_t>(s.ptr);
		WideInteger(w, x, Hugeint::ToString(x), s.column, s.tag, s.literal);
		return;
	}
	const uhugeint_t x = Load<uhugeint_t>(s.ptr);
	if (x >= uhugeint_t(Hugeint::POWERS_OF_TEN[38])) {
		Refuse(s.column, s.tag,
			   "its value " + Uhugeint::ToString(x) + " has more than the 38 digits decimal(38,0) holds");
	}
	hugeint_t h;  // below 10^38 < 2^127: the same bits
	h.lower = x.lower;
	h.upper = static_cast<int64_t>(x.upper);
	WideInteger(w, h, Uhugeint::ToString(x), s.column, s.tag, s.literal);
}

//! FLOAT / DOUBLE as real / float
void FormOfFloat(const Source &s, WriteForm &w) {
	const bool is_float = s.tag == VariantLogicalType::FLOAT;
	const double x = is_float ? static_cast<double>(Load<float>(s.ptr)) : Load<double>(s.ptr);
	if (!std::isfinite(x)) {
		Refuse(s.column, s.tag, "SQL Server's real and float hold no NaN or infinity");
	}
	if (is_float) {
		Fixed(w, tds::TDS_TYPE_REAL, "real");
		PutLE<float>(w, Load<float>(s.ptr));
	} else {
		Fixed(w, tds::TDS_TYPE_FLOAT, "float");
		PutLE<double>(w, x);
	}
	if (s.literal) {
		// The double (a float widens exactly) in 17 significant digits, which read back as it, in
		// scientific notation -- a float literal in T-SQL --, so CAST(... AS real / float) lands on the
		// value itself. A float's shortest text can read as a double past FLT_MAX (3.4028235e+38).
		// duckdb_fmt, not snprintf: a host's LC_NUMERIC would make the point a comma.
		w.literal = StringUtil::Format("%.16E", x);
	}
}

//! DECIMAL(w,s) as decimal(w,s)
void FormOfDecimal(const Source &s, WriteForm &w) {
	const auto d = VariantUtils::DecodeDecimalData(s.v, s.row, 0);
	hugeint_t unscaled;
	switch (d.GetPhysicalType()) {
	case PhysicalType::INT16:
		unscaled = Load<int16_t>(d.value_ptr);
		break;
	case PhysicalType::INT32:
		unscaled = Load<int32_t>(d.value_ptr);
		break;
	case PhysicalType::INT64:
		unscaled = Load<int64_t>(d.value_ptr);
		break;
	default:
		unscaled = Load<hugeint_t>(d.value_ptr);
		break;
	}
	const uint8_t width = static_cast<uint8_t>(d.width), scale = static_cast<uint8_t>(d.scale);
	if (width < 1 || width > 38 || scale > width) {
		Refuse(s.column, s.tag, "decimal(" + std::to_string(d.width) + "," + std::to_string(d.scale) + ")");
	}
	PutDecimal(w, unscaled, width, scale);
	if (s.literal) {
		w.literal = Decimal::ToString(unscaled, width, scale);
	}
}

//! DATE as date
void FormOfDate(const Source &s, WriteForm &w) {
	const int64_t days = Load<int32_t>(s.ptr);
	if (days < tds::encoding::BCPRowEncoder::SERVER_MIN_EPOCH_DAY ||
		days > tds::encoding::BCPRowEncoder::SERVER_MAX_EPOCH_DAY) {
		Refuse(s.column, s.tag, "SQL Server's date holds 0001-01-01 to 9999-12-31");
	}
	Fixed(w, tds::TDS_TYPE_DATE, "date");
	const auto from_0001 = static_cast<uint32_t>(days + DAYS_FROM_0001_TO_EPOCH);
	PutLE(w, from_0001, 3);
	if (s.literal) {
		w.literal = "'" + DateText(from_0001) + "'";
	}
}

//! TIME_MICROS / TIME_NANOS as time(6) / time(7)
void FormOfTime(const Source &s, WriteForm &w) {
	const bool nanos = s.tag == VariantLogicalType::TIME_NANOS;
	const uint8_t scale = nanos ? 7 : 6;
	const int64_t raw = nanos ? Load<dtime_ns_t>(s.ptr).value : Load<dtime_t>(s.ptr).value;
	// 100 ns from nanoseconds, rounded as the datetime2 path rounds.
	const int64_t units = nanos ? (raw + 50) / 100 : raw;
	const int64_t per_day = SECONDS_PER_DAY * (nanos ? 10000000 : 1000000);
	if (raw < 0 || units >= per_day) {
		Refuse(s.column, s.tag, "SQL Server's time holds 00:00:00 to 23:59:59.9999999");
	}
	SetScale(w, tds::TDS_TYPE_TIME, scale);
	PutLE(w, static_cast<uint64_t>(units), TimeBytes(scale));
	if (s.literal) {
		w.type_text = nanos ? "time(7)" : "time(6)";
		w.literal = "'" + TimeText(static_cast<uint64_t>(units), scale) + "'";
	}
}

//! The six timestamp tags as datetime2(0 / 3 / 6 / 7), or datetimeoffset(6 / 7) at +00:00 for the _TZ ones.
void FormOfTimestamp(const Source &s, WriteForm &w) {
	const int64_t ticks = Load<int64_t>(s.ptr);
	LogicalTypeId unit = LogicalTypeId::TIMESTAMP;
	uint8_t scale = 6;
	if (s.tag == VariantLogicalType::TIMESTAMP_SEC) {
		unit = LogicalTypeId::TIMESTAMP_SEC;
		scale = 0;
	} else if (s.tag == VariantLogicalType::TIMESTAMP_MILIS) {
		unit = LogicalTypeId::TIMESTAMP_MS;
		scale = 3;
	} else if (s.tag == VariantLogicalType::TIMESTAMP_NANOS || s.tag == VariantLogicalType::TIMESTAMP_NANOS_TZ) {
		unit = LogicalTypeId::TIMESTAMP_NS;
		scale = 7;
	}
	const bool offset =
		s.tag == VariantLogicalType::TIMESTAMP_MICROS_TZ || s.tag == VariantLogicalType::TIMESTAMP_NANOS_TZ;
	try {
		Timestamp(w, ticks, unit, scale, offset, s.literal);
	} catch (InvalidInputException &e) {
		Refuse(s.column, s.tag, ErrorData(e).RawMessage());
	}
}

//! UUID as uniqueidentifier
void FormOfUuid(const Source &s, WriteForm &w) {
	const hugeint_t x = Load<hugeint_t>(s.ptr);
	Fixed(w, tds::TDS_TYPE_UNIQUEIDENTIFIER, "uniqueidentifier");
	// DuckDB's UUID: the canonical big-endian bytes, the top bit flipped. The wire: Data1..Data3
	// little-endian, Data4 as is (the inverse of the read, variant_envelope).
	const uint64_t upper = static_cast<uint64_t>(x.upper) ^ (uint64_t(1) << 63);
	const uint64_t lower = x.lower;
	PutLE(w, upper >> 32, 4);
	PutLE(w, (upper >> 16) & 0xFFFF, 2);
	PutLE(w, upper & 0xFFFF, 2);
	for (int i = 7; i >= 0; i--) {
		PutByte(w, static_cast<uint8_t>((lower >> (8 * i)) & 0xFF));
	}
	if (s.literal) {
		w.literal = "'" + UUID::ToString(x) + "'";
	}
}

//! BLOB as varbinary(n), n <= 8000
void FormOfBlob(const Source &s, WriteForm &w) {
	const string_t blob = VariantUtils::DecodeStringData(s.v, s.row, 0);
	if (blob.GetSize() > MAX_BINARY_BYTES) {
		Refuse(s.column, s.tag,
			   "a blob of " + std::to_string(blob.GetSize()) + " bytes, past the 8000 a sql_variant's varbinary holds");
	}
	const auto n = static_cast<uint16_t>(MaxValue<idx_t>(1, blob.GetSize()));
	w.base_type = tds::TDS_TYPE_BIGVARBINARY;
	w.properties[0] = static_cast<uint8_t>(n & 0xFF);
	w.properties[1] = static_cast<uint8_t>(n >> 8);
	w.property_bytes = 2;
	w.str = blob;
	if (s.literal) {
		w.type_text = "varbinary(" + std::to_string(n) + ")";
		static const char *HEX = "0123456789ABCDEF";
		w.literal = "0x";
		for (idx_t i = 0; i < blob.GetSize(); i++) {
			const auto b = static_cast<uint8_t>(blob.GetData()[i]);
			w.literal += HEX[b >> 4];
			w.literal += HEX[b & 15];
		}
	}
}

//! VARCHAR as nvarchar(n) within 4000 UTF-16 units, else as varchar(n) when it is ASCII within 8000.
void FormOfString(const Source &s, WriteForm &w) {
	const string_t text = VariantUtils::DecodeStringData(s.v, s.row, 0);
	// Validated once, as the string encoder does: a VARCHAR can hold bytes that are not UTF-8 (a
	// code-page column read under client_transcode false, #224), and its UTF-16 length is then none.
	bool valid_utf8 = false;
	const size_t units = tds::encoding::Utf16LEByteLengthView(text.GetData(), text.GetSize(), valid_utf8) / 2;
	if (!valid_utf8) {
		Refuse(s.column, s.tag, "its bytes are not valid UTF-8");
	}
	if (units <= MAX_NVARCHAR_UNITS) {
		const auto n = static_cast<uint16_t>(MaxValue<size_t>(1, units));
		w.base_type = tds::TDS_TYPE_NVARCHAR;
		const auto bytes = static_cast<uint16_t>(2 * n);
		w.properties[5] = static_cast<uint8_t>(bytes & 0xFF);
		w.properties[6] = static_cast<uint8_t>(bytes >> 8);
		w.str = text;
		w.utf16 = true;
		w.units = units;
		if (s.literal) {
			w.type_text = "nvarchar(" + std::to_string(n) + ")";
		}
	} else if (units <= MAX_BINARY_BYTES && simdutf::validate_ascii(text.GetData(), text.GetSize())) {
		const auto n = static_cast<uint16_t>(units);
		w.base_type = tds::TDS_TYPE_BIGVARCHAR;
		w.properties[5] = static_cast<uint8_t>(n & 0xFF);
		w.properties[6] = static_cast<uint8_t>(n >> 8);
		w.str = text;
		if (s.literal) {
			w.type_text = "varchar(" + std::to_string(n) + ")";
		}
	} else {
		Refuse(s.column, s.tag,
			   "a string of " + std::to_string(units) +
				   " UTF-16 units, past the 4000 a sql_variant's nvarchar holds (and not ASCII within the 8000 its "
				   "varchar does)");
	}
	// The collation, then the maximum length in bytes (S1: e7 07 0904d02000 1400 for an nvarchar(20)).
	std::memcpy(w.properties, s.collation.data(), 5);
	w.property_bytes = 7;
	if (s.literal) {
		w.literal = Quoted(text.GetData(), text.GetSize(), w.base_type == tds::TDS_TYPE_NVARCHAR);
	}
}

//! How a tag's value is put in its write form; null for a tag sql_variant holds no value of.
using FormFunction = void (*)(const Source &, WriteForm &);

//! D3's write table (the header lists it), by tag.
struct TagForm {
	VariantLogicalType tag;
	FormFunction form;
};
constexpr TagForm TAG_FORMS[] = {{VariantLogicalType::BOOL_TRUE, FormOfBool},
								 {VariantLogicalType::BOOL_FALSE, FormOfBool},
								 {VariantLogicalType::INT8, FormOfSmallInt},
								 {VariantLogicalType::INT16, FormOfSmallInt},
								 {VariantLogicalType::UINT8, FormOfTinyInt},
								 {VariantLogicalType::INT32, FormOfInt},
								 {VariantLogicalType::UINT16, FormOfInt},
								 {VariantLogicalType::INT64, FormOfBigInt},
								 {VariantLogicalType::UINT32, FormOfBigInt},
								 {VariantLogicalType::UINT64, FormOfWideInteger},
								 {VariantLogicalType::INT128, FormOfWideInteger},
								 {VariantLogicalType::UINT128, FormOfWideInteger},
								 {VariantLogicalType::FLOAT, FormOfFloat},
								 {VariantLogicalType::DOUBLE, FormOfFloat},
								 {VariantLogicalType::DECIMAL, FormOfDecimal},
								 {VariantLogicalType::DATE, FormOfDate},
								 {VariantLogicalType::TIME_MICROS, FormOfTime},
								 {VariantLogicalType::TIME_NANOS, FormOfTime},
								 {VariantLogicalType::TIMESTAMP_SEC, FormOfTimestamp},
								 {VariantLogicalType::TIMESTAMP_MILIS, FormOfTimestamp},
								 {VariantLogicalType::TIMESTAMP_MICROS, FormOfTimestamp},
								 {VariantLogicalType::TIMESTAMP_NANOS, FormOfTimestamp},
								 {VariantLogicalType::TIMESTAMP_MICROS_TZ, FormOfTimestamp},
								 {VariantLogicalType::TIMESTAMP_NANOS_TZ, FormOfTimestamp},
								 {VariantLogicalType::UUID, FormOfUuid},
								 {VariantLogicalType::BLOB, FormOfBlob},
								 {VariantLogicalType::VARCHAR, FormOfString}};

//! TAG_FORMS by the tag's own value, so a value's form is one lookup (a row of a bulk load pays it).
using FormsByTag = std::array<FormFunction, static_cast<size_t>(VariantLogicalType::ENUM_SIZE)>;
const FormsByTag &Forms() {
	static const FormsByTag FORMS = [] {
		FormsByTag forms{};
		for (const auto &entry : TAG_FORMS) {
			forms[static_cast<size_t>(entry.tag)] = entry.form;
		}
		return forms;
	}();
	return FORMS;
}

//! The write form of value `values_idx` (the root, 0) of row `row`. False for a NULL (the row's or a
//! VARIANT NULL). `literal`: fill w.literal too.
bool FormOf(const UnifiedVariantVectorData &v, idx_t row, const std::string &column,
			const std::array<uint8_t, 5> &collation, bool literal, WriteForm &w) {
	w.literal_wanted = literal;
	if (!v.RowIsValid(row)) {
		return false;
	}
	const auto tag = v.GetTypeId(row, 0);
	if (tag == VariantLogicalType::VARIANT_NULL) {
		return false;
	}
	const auto &forms = Forms();
	const auto index = static_cast<size_t>(tag);
	const FormFunction form = index < forms.size() ? forms[index] : nullptr;
	if (!form) {
		Refuse(column, tag, "sql_variant holds no value of it");
	}
	const auto *ptr = const_data_ptr_cast(v.GetData(row).GetData()) + v.GetByteOffset(row, 0);
	form(Source{v, row, tag, ptr, column, collation, literal}, w);
	return true;
}

void AppendEnvelope(const WriteForm &w, vector<uint8_t> &out) {
	const size_t value_bytes = w.fixed_size + (w.utf16 ? 2 * w.units : w.str.GetSize());
	const uint32_t total = static_cast<uint32_t>(2 + w.property_bytes + value_bytes);
	for (int i = 0; i < 4; i++) {
		out.push_back(static_cast<uint8_t>((total >> (8 * i)) & 0xFF));
	}
	out.push_back(w.base_type);
	out.push_back(w.property_bytes);
	out.insert(out.end(), w.properties, w.properties + w.property_bytes);
	out.insert(out.end(), w.fixed, w.fixed + w.fixed_size);
	if (w.utf16) {
		const size_t at = out.size();
		out.resize(at + 2 * w.units);
		if (w.units > 0) {
			// Validated by FormOf; the destination may be at any offset (the helper takes an odd one).
			tds::encoding::Utf16LEEncodeValidDirect(w.str.GetData(), w.str.GetSize(), out.data() + at);
		}
	} else if (w.str.GetSize() > 0) {
		out.insert(out.end(), w.str.GetData(), w.str.GetData() + w.str.GetSize());
	}
}

void AppendNull(vector<uint8_t> &out) {
	out.insert(out.end(), 4, 0);
}

}  // namespace

std::string FormatLiteral(const Value &value, const std::string &column) {
	if (value.IsNull()) {
		return "NULL";
	}
	Vector vec(value, count_t(1));
	RecursiveUnifiedVectorFormat format;
	Vector::RecursiveToUnifiedFormat(vec, format);
	const UnifiedVariantVectorData data(format);
	WriteForm w;
	// The statement path declares no collation: a string value takes the database's default.
	if (!FormOf(data, 0, column, StringCollationBytes(std::string()), true, w)) {
		return "NULL";
	}
	return "CAST(CAST(" + w.literal + " AS " + w.type_text + ") AS sql_variant)";
}

void EncodeToBcp(const UnifiedVariantVectorData &data, idx_t row, const BCPColumnMetadata &col, vector<uint8_t> &out) {
	WriteForm w;
	if (!FormOf(data, row, col.ReportedName(), col.collation, false, w)) {
		AppendNull(out);
		return;
	}
	AppendEnvelope(w, out);
}

void EncodeValueToBcp(const Value &value, const BCPColumnMetadata &col, vector<uint8_t> &out) {
	if (value.IsNull()) {
		AppendNull(out);
		return;
	}
	Vector vec(value, count_t(1));
	RecursiveUnifiedVectorFormat format;
	Vector::RecursiveToUnifiedFormat(vec, format);
	const UnifiedVariantVectorData data(format);
	EncodeToBcp(data, 0, col, out);
}

}  // namespace variant
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
