// Spec 093 T2: a DuckDB VARIANT written into a sql_variant (D3's write table). Each value is written as
// the bulk wire's SSVARIANT envelope and read back through T1's reader (variant_codec, checked against
// the server in S1): the round trip's tag and value are D3's write table composed with its read table --
// the tags whose base type reads back as itself keep their value, the others widen as the table says
// (INT8 -> smallint -> INT16, UINT16 -> int -> INT32, UINT32 -> bigint -> INT64, the wide integers ->
// decimal(38,0)), and nanoseconds round to 100 ns. The statement path's literal of the same values, the
// refusals by name, and the collation name-to-wire helper the bulk path declares string values with.

#include "codec/code_page/code_page.hpp"
#include "codec/variant/variant_codec.hpp"
#include "codec/variant/variant_write.hpp"
#include "copy/target_resolver.hpp"
#include "duckdb.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"
#include "tds/tds_column_metadata.hpp"
#include "tds/tds_types.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

using namespace duckdb;
using namespace duckdb::mssql;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	if (!ok) {
		std::cout << "  FAIL " << what << std::endl;
		failures++;
	}
}

Value VariantOf(Connection &con, const std::string &expr) {
	auto result = con.Query("SELECT (" + expr + ")::VARIANT");
	if (result->HasError()) {
		std::cout << "  query failed: " << expr << ": " << result->GetError() << std::endl;
		failures++;
		return Value();
	}
	auto chunk = result->Fetch();
	return chunk->GetValue(0, 0);
}

mssql::BCPColumnMetadata VariantColumn() {
	return mssql::BCPColumnMetadata::FromServerColumn("v", "sql_variant", 8016, 0, 0, true, "");
}

//! The value written as an envelope and read back by T1: its tag and its text ("" with `error` set when
//! writing was refused).
struct RoundTrip {
	std::string tag;
	std::string text;
	std::string error;
	std::vector<uint8_t> wire;
};

RoundTrip WriteAndRead(const Value &v) {
	RoundTrip out;
	vector<uint8_t> wire;
	try {
		codec::variant::EncodeValueToBcp(v, VariantColumn(), wire);
	} catch (std::exception &e) {
		out.error = ErrorData(e).RawMessage();
		return out;
	}
	out.wire.assign(wire.begin(), wire.end());
	const uint32_t length = wire[0] | (wire[1] << 8) | (wire[2] << 16) | (static_cast<uint32_t>(wire[3]) << 24);
	if (length == 0) {
		out.tag = "NULL";
		return out;
	}
	Check(length + 4 == wire.size(), "the u32 length is the envelope's");
	std::vector<uint8_t> envelope(wire.begin() + 4, wire.end());
	tds::ColumnMetadata col;
	col.name = "v";
	col.type_id = tds::TDS_TYPE_SQL_VARIANT;
	Vector back(LogicalType::VARIANT(), 1);
	codec::variant::DecodeFromTds(envelope, col, back, 0, nullptr);
	RecursiveUnifiedVectorFormat format;
	Vector::RecursiveToUnifiedFormat(back, format);
	const UnifiedVariantVectorData data(format);
	out.tag = EnumUtil::ToString(data.GetTypeId(0, 0));
	out.text = back.GetValue(0).ToString();
	return out;
}

void Case(Connection &con, const std::string &expr, const std::string &tag, const std::string &text,
		  const std::string &literal) {
	const Value v = VariantOf(con, expr);
	const auto rt = WriteAndRead(v);
	Check(rt.error.empty(), expr + ": written (" + rt.error + ")");
	Check(rt.tag == tag, expr + ": reads back as " + tag + ", got " + rt.tag);
	Check(rt.text == text, expr + ": reads back as '" + text + "', got '" + rt.text + "'");
	if (!literal.empty()) {
		const auto got = codec::variant::FormatLiteral(v, "v");
		Check(got == literal, expr + ": literal " + literal + ", got " + got);
	}
}

void Refused(Connection &con, const std::string &expr, const std::string &needle) {
	const Value v = VariantOf(con, expr);
	const auto rt = WriteAndRead(v);
	Check(!rt.error.empty() && rt.error.find(needle) != std::string::npos &&
			  rt.error.find("column \"v\"") != std::string::npos,
		  expr + ": refused naming the column and '" + needle + "', got '" + rt.error + "'");
	bool literal_refused = false;
	try {
		codec::variant::FormatLiteral(v, "v");
	} catch (InvalidInputException &e) {
		literal_refused = std::string(ErrorData(e).RawMessage()).find(needle) != std::string::npos;
	}
	Check(literal_refused, expr + ": the literal refused too");
}

}  // namespace

int main() {
	DuckDB db(nullptr);
	Connection con(db);

	// The tags whose base type reads back as itself.
	Case(con, "true", "BOOL_TRUE", "true", "CAST(CAST(1 AS bit) AS sql_variant)");
	Case(con, "false", "BOOL_FALSE", "false", "CAST(CAST(0 AS bit) AS sql_variant)");
	Case(con, "200::UTINYINT", "UINT8", "200", "CAST(CAST(200 AS tinyint) AS sql_variant)");
	Case(con, "(-32768)::SMALLINT", "INT16", "-32768", "CAST(CAST(-32768 AS smallint) AS sql_variant)");
	Case(con, "(-2147483648)::INTEGER", "INT32", "-2147483648", "CAST(CAST(-2147483648 AS int) AS sql_variant)");
	Case(con, "9223372036854775807::BIGINT", "INT64", "9223372036854775807",
		 "CAST(CAST(9223372036854775807 AS bigint) AS sql_variant)");
	// A real / float literal: the double in 17 significant digits, so the cast lands on the value itself --
	// FLT_MAX's shortest text (3.4028235e+38) would read as a double past it.
	Case(con, "1.5::FLOAT", "FLOAT", "1.5", "CAST(CAST(1.5000000000000000E+00 AS real) AS sql_variant)");
	Case(con, "3.4028235e38::FLOAT", "FLOAT", "3.4028235e+38",
		 "CAST(CAST(3.4028234663852886E+38 AS real) AS sql_variant)");
	Case(con, "(-1.25e300)::DOUBLE", "DOUBLE", "-1.25e+300",
		 "CAST(CAST(-1.2500000000000001E+300 AS float) AS sql_variant)");
	Case(con, "0.1::DOUBLE", "DOUBLE", "0.1", "CAST(CAST(1.0000000000000001E-01 AS float) AS sql_variant)");
	Case(con, "1.7976931348623157e308::DOUBLE", "DOUBLE", "1.7976931348623157e+308",
		 "CAST(CAST(1.7976931348623157E+308 AS float) AS sql_variant)");
	Case(con, "'4.9406564584124654e-324'::DOUBLE", "DOUBLE", "5e-324",
		 "CAST(CAST(4.9406564584124654E-324 AS float) AS sql_variant)");
	Case(con, "'-0.0'::DOUBLE", "DOUBLE", "-0.0", "CAST(CAST(-0.0000000000000000E+00 AS float) AS sql_variant)");
	Case(con, "(-1234567.89)::DECIMAL(9,2)", "DECIMAL", "-1234567.89",
		 "CAST(CAST(-1234567.89 AS decimal(9,2)) AS sql_variant)");
	// A narrow decimal's literal (a width below 18: an int16 / int32 mantissa).
	Case(con, "(-12.5)::DECIMAL(4,1)", "DECIMAL", "-12.5", "CAST(CAST(-12.5 AS decimal(4,1)) AS sql_variant)");
	Case(con, "0.5::DECIMAL(1,1)", "DECIMAL", ".5", "CAST(CAST(.5 AS decimal(1,1)) AS sql_variant)");  // DuckDB's text
	Case(con, "12345678901234567890123456789012345678::DECIMAL(38,0)", "DECIMAL",
		 "12345678901234567890123456789012345678", "");
	Case(con, "DATE '0001-01-01'", "DATE", "0001-01-01", "CAST(CAST('0001-01-01' AS date) AS sql_variant)");
	Case(con, "DATE '9999-12-31'", "DATE", "9999-12-31", "");
	Case(con, "TIME '23:59:59.999999'", "TIME_MICROS", "23:59:59.999999",
		 "CAST(CAST('23:59:59.999999' AS time(6)) AS sql_variant)");
	Case(con, "TIMESTAMP_S '2026-01-02 03:04:05'", "TIMESTAMP_SEC", "2026-01-02 03:04:05",
		 "CAST(CAST('2026-01-02T03:04:05' AS datetime2(0)) AS sql_variant)");
	Case(con, "TIMESTAMP_MS '2026-01-02 03:04:05.123'", "TIMESTAMP_MILIS", "2026-01-02 03:04:05.123",
		 "CAST(CAST('2026-01-02T03:04:05.123' AS datetime2(3)) AS sql_variant)");
	Case(con, "TIMESTAMP '9999-12-31 23:59:59.999999'", "TIMESTAMP_MICROS", "9999-12-31 23:59:59.999999",
		 "CAST(CAST('9999-12-31T23:59:59.999999' AS datetime2(6)) AS sql_variant)");
	Case(con, "TIMESTAMPTZ '2026-01-01 10:00:00.123456+05:30'", "TIMESTAMP_MICROS_TZ", "2026-01-01 04:30:00.123456+00",
		 "CAST(CAST('2026-01-01T04:30:00.123456+00:00' AS datetimeoffset(6)) AS sql_variant)");
	Case(con, "'6f9619ff-8b86-d011-b42d-00c04fc964ff'::UUID", "UUID", "6f9619ff-8b86-d011-b42d-00c04fc964ff",
		 "CAST(CAST('6f9619ff-8b86-d011-b42d-00c04fc964ff' AS uniqueidentifier) AS sql_variant)");
	Case(con, "'\\x01\\x02\\x03'::BLOB", "BLOB", "\\x01\\x02\\x03",
		 "CAST(CAST(0x010203 AS varbinary(3)) AS sql_variant)");
	Case(con, "''::BLOB", "BLOB", "", "CAST(CAST(0x AS varbinary(1)) AS sql_variant)");
	Case(con, "'дом 😀 it''s'", "VARCHAR", "дом 😀 it's", "CAST(CAST(N'дом 😀 it''s' AS nvarchar(11)) AS sql_variant)");
	Case(con, "''", "VARCHAR", "", "CAST(CAST(N'' AS nvarchar(1)) AS sql_variant)");

	// Nanoseconds: 100 ns, rounded.
	Case(con, "TIMESTAMP_NS '2026-01-01 00:00:00.123456789'", "TIMESTAMP_NANOS", "2026-01-01 00:00:00.1234568",
		 "CAST(CAST('2026-01-01T00:00:00.1234568' AS datetime2(7)) AS sql_variant)");
	Case(con, "'2026-01-01 23:59:59.99999995'::TIMESTAMP_NS", "TIMESTAMP_NANOS", "2026-01-02 00:00:00", "");
	Case(con, "'10:00:00.12345678'::TIME_NS", "TIME_NANOS", "10:00:00.1234568",
		 "CAST(CAST('10:00:00.1234568' AS time(7)) AS sql_variant)");

	// The tags that widen (D3's write table composed with its read table).
	Case(con, "(-128)::TINYINT", "INT16", "-128", "CAST(CAST(-128 AS smallint) AS sql_variant)");
	Case(con, "65535::USMALLINT", "INT32", "65535", "CAST(CAST(65535 AS int) AS sql_variant)");
	Case(con, "4294967295::UINTEGER", "INT64", "4294967295", "CAST(CAST(4294967295 AS bigint) AS sql_variant)");
	Case(con, "18446744073709551615::UBIGINT", "DECIMAL", "18446744073709551615",
		 "CAST(CAST(18446744073709551615 AS decimal(38,0)) AS sql_variant)");
	Case(con, "(-99999999999999999999999999999999999999)::HUGEINT", "DECIMAL",
		 "-99999999999999999999999999999999999999", "");
	Case(con, "99999999999999999999999999999999999999::UHUGEINT", "DECIMAL", "99999999999999999999999999999999999999",
		 "");

	// A string past the 4000 nvarchar units: varchar when ASCII, refused otherwise.
	Case(con, "'" + std::string(5000, 'a') + "'", "VARCHAR", std::string(5000, 'a'), "");
	{
		const auto literal = codec::variant::FormatLiteral(VariantOf(con, "'" + std::string(4001, 'a') + "'"), "v");
		Check(literal.find(" AS varchar(4001)) AS sql_variant)") != std::string::npos &&
				  literal.rfind("CAST(CAST('aaa", 0) == 0,
			  "an ASCII string of 4001 characters is a varchar(4001) literal, no N prefix");
	}

	// NULL: a length of 0.
	{
		const auto rt = WriteAndRead(Value(LogicalType::VARIANT()));
		Check(rt.tag == "NULL" && rt.wire.size() == 4, "a NULL is a length of 0");
		Check(codec::variant::FormatLiteral(Value(LogicalType::VARIANT()), "v") == "NULL", "a NULL literal");
	}

	// Refused by name.
	Refused(con, "CAST('[1, 2]' AS INTEGER[])", "ARRAY");
	Refused(con, "CAST('{a: 1}' AS STRUCT(a INTEGER))", "OBJECT");
	Refused(con, "CAST('1 day' AS INTERVAL)", "INTERVAL");
	Refused(con, "'NaN'::DOUBLE", "NaN");
	Refused(con, "'inf'::FLOAT", "NaN or infinity");
	Refused(con, "170141183460469231731687303715884105727::HUGEINT", "38 digits");
	{
		std::string e;
		for (int i = 0; i < 4001; i++) {
			e += "é";
		}
		Refused(con, "'" + e + "'", "4001 UTF-16 units");
		Refused(con, "'" + std::string(8001, 'b') + "'::BLOB", "8001 bytes");
	}
	Refused(con, "TIMESTAMP 'infinity'", "TIMESTAMP_MICROS");
	Refused(con, "DATE '-infinity'", "0001-01-01 to 9999-12-31");
	Refused(con, "TIME '24:00:00'", "23:59:59.9999999");
	Refused(con, "TIMETZ '10:00:00+01'", "TIME_MICROS_TZ");

	// The envelope byte for byte against S1's measured ones (specs/093-type-kernels/measured/S1.md): a
	// string's properties are its collation, then its maximum length in bytes (S1 row 46, an nvarchar under
	// Latin1_General_100_CI_AS_SC: e7 07 0904d02000 then the length; here nvarchar(3), 0600), a decimal its
	// precision and scale then the sign and the 16-byte mantissa (row 10), an int no property (row 5).
	{
		auto hex = [](const vector<uint8_t> &bytes) {
			static const char *digits = "0123456789abcdef";
			std::string out;
			for (auto b : bytes) {
				out += digits[b >> 4];
				out += digits[b & 15];
			}
			return out;
		};
		const auto sc = mssql::BCPColumnMetadata::FromServerColumn("v", "sql_variant", 8016, 0, 0, true,
																   "Latin1_General_100_CI_AS_SC");
		vector<uint8_t> wire;
		codec::variant::EncodeValueToBcp(VariantOf(con, "'😀x'"), sc, wire);
		Check(hex(wire) == "0f000000e7070904d0200006003dd800de7800",
			  "an nvarchar envelope: collation, then max length (S1 row 46), got " + hex(wire));
		wire.clear();
		codec::variant::EncodeValueToBcp(VariantOf(con, "(-1234567.89)::DECIMAL(9,2)"), VariantColumn(), wire);
		Check(hex(wire) == "150000006a0209020015cd5b07000000000000000000000000",
			  "a decimal(9,2) envelope (S1 row 10), got " + hex(wire));
		wire.clear();
		codec::variant::EncodeValueToBcp(VariantOf(con, "(-2147483648)::INTEGER"), VariantColumn(), wire);
		Check(hex(wire) ==
				  "060000003800"
				  "00000080",
			  "an int envelope (S1 row 5), got " + hex(wire));
		wire.clear();
		codec::variant::EncodeValueToBcp(VariantOf(con, "'" + std::string(4001, 'a') + "'"), sc, wire);
		Check(wire.size() == 4 + 2 + 7 + 4001 && wire[4] == 0xA7 && wire[5] == 7 && wire[6] == 0x09 &&
				  wire[11] == 0xA1 && wire[12] == 0x0F,
			  "a varchar envelope: collation, then max length 4001 (0x0FA1)");
		// An nvarchar's UTF-16 at either parity of the buffer: its header is 13 bytes, so after 0, 1, 2 or 3
		// bytes already in the buffer the units start at an odd or an even offset; the bytes are the same.
		const std::string text = "дом 😀 a string longer than the inline twelve bytes";
		vector<uint8_t> alone;
		codec::variant::EncodeValueToBcp(VariantOf(con, "'" + text + "'"), sc, alone);
		for (idx_t prefix = 1; prefix <= 3; prefix++) {
			vector<uint8_t> after(prefix, 0xEE);
			codec::variant::EncodeValueToBcp(VariantOf(con, "'" + text + "'"), sc, after);
			Check(
				after.size() == prefix + alone.size() && std::equal(alone.begin(), alone.end(), after.begin() + prefix),
				"an nvarchar envelope after " + std::to_string(prefix) + " byte(s): the same bytes");
		}
	}

	// A VARCHAR inside a VARIANT whose bytes are not UTF-8 (a code-page column read under client_transcode
	// false makes such values, #224): refused by name, not measured as UTF-16. DuckDB validates a string it
	// is given, so the byte is put into the VARIANT's own data after the fact (a 20-byte string: not inlined,
	// its string_t points into that data).
	{
		Vector vec(VariantOf(con, "'abcdefghijklmnopqrst'"), count_t(1));
		RecursiveUnifiedVectorFormat format;
		Vector::RecursiveToUnifiedFormat(vec, format);
		const UnifiedVariantVectorData data(format);
		const string_t s = VariantUtils::DecodeStringData(data, 0, 0);
		const_cast<char *>(s.GetData())[3] = static_cast<char>(0xFF);
		vector<uint8_t> wire;
		std::string error;
		try {
			codec::variant::EncodeToBcp(data, 0, VariantColumn(), wire);
		} catch (InvalidInputException &e) {
			error = ErrorData(e).RawMessage();
		}
		Check(error.find("not valid UTF-8") != std::string::npos && error.find("column \"v\"") != std::string::npos &&
				  error.find("VARCHAR") != std::string::npos,
			  "a VARCHAR of invalid UTF-8 refused naming the column and the tag, got '" + error + "'");
	}

	// The collation helper: the wire's bytes of names COLMETADATA carries (the extension's own constants).
	{
		uint8_t wire[5];
		const std::string sql = "SQL_Latin1_General_CP1_CI_AS";
		Check(codec::code_page::WireCollationForName(sql.data(), sql.size(), wire) && wire[0] == 0x09 &&
				  wire[1] == 0x04 && wire[2] == 0xD0 && wire[3] == 0x00 && wire[4] == 0x34,
			  "SQL_Latin1_General_CP1_CI_AS is 09 04 D0 00 34");
		const std::string utf8 = "Latin1_General_100_CI_AS_SC_UTF8";
		Check(codec::code_page::WireCollationForName(utf8.data(), utf8.size(), wire) && wire[0] == 0x09 &&
				  wire[1] == 0x04 && wire[2] == 0xD0 && wire[3] == 0x24 && wire[4] == 0x00,
			  "Latin1_General_100_CI_AS_SC_UTF8 is 09 04 D0 24 00");
		const std::string bin2 = "Latin1_General_100_BIN2";
		Check(codec::code_page::WireCollationForName(bin2.data(), bin2.size(), wire) && wire[2] == 0x00 &&
				  wire[3] == 0x22,
			  "a BIN2 collation: the BIN2 flag alone, version 2");
		const std::string none = "No_Such_Collation";
		Check(!codec::code_page::WireCollationForName(none.data(), none.size(), wire), "an unknown name: none");
		// A sql_variant column declared under one carries its bytes; under none, the default.
		const auto col =
			mssql::BCPColumnMetadata::FromServerColumn("v", "sql_variant", 8016, 0, 0, true, "Cyrillic_General_CI_AS");
		Check(col.tds_type_token == tds::TDS_TYPE_SQL_VARIANT, "a sql_variant column: SSVARIANT");
		Check(col.duckdb_type.id() == LogicalTypeId::VARIANT, "a sql_variant column: fed by a VARIANT");
		Check(col.GetSQLServerTypeDeclaration() == "sql_variant", "a sql_variant column: declared sql_variant");
		Check(col.GetLengthPrefixSize() == 4, "a sql_variant column: LONGLEN");
		Check(!col.bulk_unsupported, "a sql_variant column: on the bulk wire");
		Check(col.collation[0] == 0x19 && col.collation[1] == 0x04, "Cyrillic_General_CI_AS's LCID 1049 in its bytes");
		// A column CTAS / COPY creates from a VARIANT: the database's collation; none, the default bytes.
		const auto created = mssql::TargetResolver::GenerateColumnMetadata(
			{LogicalType::VARIANT(), LogicalType::VARCHAR}, {"v", "s"}, "", false, "Cyrillic_General_CI_AS");
		Check(created[0].IsSqlVariant() && created[0].collation[0] == 0x19 && created[0].collation[1] == 0x04,
			  "a created sql_variant column: the database's collation");
		Check(created[1].collation[0] == 0x09, "a created nvarchar column: not the VARIANT's collation");
		// A collation the build's table lacks (or none): Latin1_General_100_BIN2_UTF8's bytes -- 0409, the
		// BIN2 and UTF8 flags (0x20 | 0x40), version 2, SortId 0 -- on every path that names one.
		uint8_t bin2_utf8[5];
		const std::string bin2_utf8_name = "Latin1_General_100_BIN2_UTF8";
		Check(codec::code_page::WireCollationForName(bin2_utf8_name.data(), bin2_utf8_name.size(), bin2_utf8) &&
				  bin2_utf8[0] == 0x09 && bin2_utf8[1] == 0x04 && bin2_utf8[2] == 0x00 && bin2_utf8[3] == 0x26 &&
				  bin2_utf8[4] == 0x00,
			  "Latin1_General_100_BIN2_UTF8 is 09 04 00 26 00");
		auto is_bin2_utf8 = [&](const std::array<uint8_t, 5> &bytes) {
			return std::equal(bytes.begin(), bytes.end(), bin2_utf8);
		};
		Check(is_bin2_utf8(codec::variant::StringCollationBytes("No_Such_Collation")) &&
				  is_bin2_utf8(codec::variant::StringCollationBytes("")),
			  "a name not in the table, or none: Latin1_General_100_BIN2_UTF8's bytes");
		Check(!is_bin2_utf8(codec::variant::StringCollationBytes("Cyrillic_General_CI_AS")),
			  "a name in the table: its own bytes");
		const auto plain = mssql::TargetResolver::GenerateColumnMetadata({LogicalType::VARIANT()}, {"v"});
		Check(is_bin2_utf8(plain[0].collation), "a created sql_variant column with no database collation: BIN2_UTF8");
		const auto unknown = mssql::TargetResolver::GenerateColumnMetadata({LogicalType::VARIANT()}, {"v"}, "", false,
																		   "No_Such_Collation");
		Check(is_bin2_utf8(unknown[0].collation),
			  "a created sql_variant column, a collation not in the table: BIN2_UTF8");
		const auto existing =
			mssql::BCPColumnMetadata::FromServerColumn("v", "sql_variant", 8016, 0, 0, true, "No_Such_Collation");
		Check(is_bin2_utf8(existing.collation),
			  "an existing sql_variant column, a collation not in the table: BIN2_UTF8");
		// The envelope carries them.
		vector<uint8_t> wire_unknown;
		codec::variant::EncodeValueToBcp(VariantOf(con, "'x'"), existing, wire_unknown);
		Check(wire_unknown.size() > 11 && std::equal(bin2_utf8, bin2_utf8 + 5, wire_unknown.begin() + 6),
			  "an nvarchar envelope under a collation not in the table: BIN2_UTF8's bytes");
	}

	if (failures) {
		std::cerr << failures << " check(s) failed\n";
		return 1;
	}
	std::cout << "test_variant_write: all checks passed\n";
	return 0;
}
