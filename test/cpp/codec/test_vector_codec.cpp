// Spec 093 T3 (D4): a SQL Server vector(n) as a DuckDB FLOAT[n]. The bytes measured in V1 / V4 (the
// VECTORTYPE value and the stored value are the same), the JSON texts of V2 (each element %.7e), the
// header refusals, float16 both ways (every finite float16 round-trips; rounding to nearest even; the
// overflow), the COLMETADATA and row framing of 0xF5, the read into an ARRAY(FLOAT, n) vector from both
// forms, the statement path's literal, the bulk wire's value, the DDL and the BCP column metadata.

#include "codec/sql_vector/vector_bytes.hpp"
#include "codec/sql_vector/vector_codec.hpp"
#include "codec/staging/column_ops.hpp"
#include "copy/target_resolver.hpp"
#include "duckdb.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "tds/encoding/type_converter.hpp"
#include "tds/tds_column_metadata.hpp"
#include "tds/tds_row_reader.hpp"
#include "tds/tds_token_parser.hpp"
#include "tds/tds_types.hpp"

#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

using namespace duckdb;
using namespace duckdb::mssql::codec::sql_vector;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	if (!ok) {
		std::cout << "  FAIL " << what << std::endl;
		failures++;
	}
}

std::vector<uint8_t> Hex(const std::string &hex) {
	std::vector<uint8_t> out;
	for (size_t i = 0; i + 1 < hex.size(); i += 2) {
		out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
	}
	return out;
}

uint32_t Bits(float f) {
	uint32_t b;
	std::memcpy(&b, &f, 4);
	return b;
}

std::string Throws(const std::function<void()> &fn) {
	try {
		fn();
	} catch (std::exception &e) {
		return ErrorData(e).RawMessage();
	}
	return std::string();
}

tds::ColumnMetadata WireColumn(uint16_t dims, uint8_t element_type) {
	tds::ColumnMetadata col;
	col.name = "v";
	col.type_id = tds::TDS_TYPE_VECTOR;
	col.vector_element_type = element_type;
	col.max_length = static_cast<uint16_t>(8 + dims * (element_type == 1 ? 2 : 4));
	col.user_type = tds::VECTOR_USER_TYPE;
	col.precision = col.scale = 0;
	col.collation = 0;
	col.collation_sort_id = 0;
	col.flags = 1;
	return col;
}

tds::ColumnMetadata TextColumn(uint8_t type_id) {
	tds::ColumnMetadata col;
	col.name = "v";
	col.type_id = type_id;
	col.max_length = 0xFFFF;
	col.user_type = tds::VECTOR_USER_TYPE;
	col.precision = col.scale = 0;
	col.collation = 0x26000409;	 // Latin1_General_100_BIN2_UTF8, as V1 measured
	col.collation_sort_id = 0;
	col.flags = 1;
	return col;
}

std::vector<uint8_t> Ascii(const std::string &s) {
	return std::vector<uint8_t>(s.begin(), s.end());
}

std::vector<uint8_t> Utf16(const std::string &s) {
	std::vector<uint8_t> out;
	for (char c : s) {
		out.push_back(static_cast<uint8_t>(c));
		out.push_back(0);
	}
	return out;
}

Value FloatArray(const std::vector<float> &values) {
	vector<Value> children;
	for (float v : values) {
		children.push_back(Value::FLOAT(v));
	}
	return Value::ARRAY(LogicalType::FLOAT, std::move(children));
}

// --- the bytes (DuckDB-free half) ---

void TestBytes() {
	// V1 / V4: vector(3) [1,2,3] and vector(3, float16) [1,2,3].
	const auto f32 = Hex("a9010300000000000000803f0000004000004040");
	const auto f16 =
		Hex("a90103000100000000"
			"3c00400042");
	Check(CheckValue(f32.data(), f32.size(), 3, 0) == VectorStatus::Ok, "V1 float32 [1,2,3]: valid");
	std::vector<float> out(3);
	WidenElements(f32.data(), 3, 0, out.data());
	Check(out[0] == 1 && out[1] == 2 && out[2] == 3, "V1 float32 [1,2,3]: elements");
	Check(CheckValue(f16.data(), f16.size(), 3, 1) == VectorStatus::Ok, "V4 float16 [1,2,3]: valid");
	WidenElements(f16.data(), 3, 1, out.data());
	Check(out[0] == 1 && out[1] == 2 && out[2] == 3, "V4 float16 [1,2,3]: widened");

	Check(CheckValue(f32.data(), 16, 3, 0) == VectorStatus::WrongLength, "16 bytes for a vector(3): wrong length");
	auto bad = f32;
	bad[0] = 0xAA;
	Check(CheckValue(bad.data(), bad.size(), 3, 0) == VectorStatus::BadHeader, "no A9: bad header");
	bad = f32;
	bad[6] = 1;
	Check(CheckValue(bad.data(), bad.size(), 3, 0) == VectorStatus::BadHeader, "a reserved byte set: bad header");
	bad = f32;
	bad[2] = 4;
	Check(CheckValue(bad.data(), bad.size(), 3, 0) == VectorStatus::WrongDimensions, "dims 4 in a vector(3)");
	bad = f32;
	bad[4] = 1;
	Check(CheckValue(bad.data(), bad.size(), 3, 0) == VectorStatus::WrongElementType,
		  "element type 1 in a float32 column (V6's 42206 case)");

	// The encoder writes V1's bytes.
	std::vector<uint8_t> written(ValueBytes(3, 0));
	uint16_t bad_element = 0;
	const float one_two_three[] = {1, 2, 3};
	Check(EncodeValue(one_two_three, 3, 0, written.data(), bad_element) == VectorStatus::Ok && written == f32,
		  "EncodeValue float32: V1's bytes");
	std::vector<uint8_t> written16(ValueBytes(3, 1));
	Check(EncodeValue(one_two_three, 3, 1, written16.data(), bad_element) == VectorStatus::Ok && written16 == f16,
		  "EncodeValue float16: V4's bytes");
	const float nan_value[] = {1, NAN, 3};
	Check(EncodeValue(nan_value, 3, 0, written.data(), bad_element) == VectorStatus::NotFinite && bad_element == 1,
		  "EncodeValue: NaN refused at element 2");
	const float big[] = {1, 2, 70000};
	Check(EncodeValue(big, 3, 1, written16.data(), bad_element) == VectorStatus::Float16Overflow && bad_element == 2,
		  "EncodeValue: 70000 past float16 refused at element 3");
}

void TestFloat16() {
	// Every finite float16 round-trips through float exactly.
	int mismatches = 0;
	for (uint32_t h = 0; h < 0x10000; h++) {
		const auto half = static_cast<uint16_t>(h);
		if ((half & 0x7C00) == 0x7C00) {
			continue;  // infinities, NaNs
		}
		uint16_t back = 0;
		if (!FloatToHalf(HalfToFloat(half), back) || back != half) {
			mismatches++;
		}
	}
	Check(mismatches == 0, "every finite float16 round-trips, mismatches " + std::to_string(mismatches));
	uint16_t h = 0;
	Check(FloatToHalf(65504.0f, h) && h == 0x7BFF, "65504 is the largest float16");
	Check(FloatToHalf(65510.0f, h) && h == 0x7BFF, "65510 rounds to 65504");
	Check(FloatToHalf(65519.0f, h) && h == 0x7BFF, "65519 rounds to 65504");
	Check(FloatToHalf(-65510.0f, h) && h == 0xFBFF, "-65510 rounds to -65504");
	Check(!FloatToHalf(65520.0f, h), "65520 rounds past 65504: refused");
	Check(!FloatToHalf(INFINITY, h) && !FloatToHalf(NAN, h), "inf / NaN refused");
	Check(FloatToHalf(1.0f + 1.0f / 2048, h) && h == 0x3C00, "1 + 2^-11 ties to even: 1");
	Check(FloatToHalf(1.0f + 3.0f / 2048, h) && h == 0x3C02, "1 + 3 x 2^-11 ties to even: 1 + 2^-9");
	Check(FloatToHalf(std::ldexp(1.0f, -24), h) && h == 0x0001, "2^-24: the smallest subnormal");
	Check(FloatToHalf(std::ldexp(1.0f, -25), h) && h == 0x0000, "2^-25 ties to even: 0");
	Check(FloatToHalf(std::ldexp(1.5f, -25), h) && h == 0x0001, "1.5 x 2^-25: rounds up to 2^-24");
	Check(FloatToHalf(-0.0f, h) && h == 0x8000, "-0 keeps its sign");
	Check(HalfToFloat(0x2E66) == 0.0999755859375f, "float16 0x2e66 is 0.0999755859375");
}

void TestText() {
	std::string scratch;
	std::vector<float> out(3);
	// V2's texts: float32.
	auto text = Ascii("[1.0000000e+000,2.0000000e+000,3.0000000e+000]");
	Check(ParseText(text.data(), text.size(), false, 3, out.data(), scratch) == VectorStatus::Ok && out[0] == 1 &&
			  out[1] == 2 && out[2] == 3,
		  "V2 [1,2,3]");
	text = Ascii("[1.0000000e-001,0.0000000e+000,3.4028235e+038]");
	Check(ParseText(text.data(), text.size(), false, 3, out.data(), scratch) == VectorStatus::Ok &&
			  Bits(out[0]) == 0x3dcccccd && Bits(out[1]) == 0 && Bits(out[2]) == 0x7f7fffff,
		  "V2 [0.1, 0, FLT_MAX]: the stored bits");
	text = Ascii("[1.4012985e-045,-1.1754944e-038,1.1754942e-038]");
	Check(ParseText(text.data(), text.size(), false, 3, out.data(), scratch) == VectorStatus::Ok &&
			  Bits(out[0]) == 0x00000001 && Bits(out[1]) == 0x80800000 && Bits(out[2]) == 0x007fffff,
		  "V2 denormals: the stored bits");
	// V2: float32 text is inexact -- the server prints -103.217316 and its neighbour alike.
	text = Ascii("[-1.0321732e+002,0.0,1.0]");
	Check(ParseText(text.data(), text.size(), false, 3, out.data(), scratch) == VectorStatus::Ok &&
			  (Bits(out[0]) == 0xc2ce6f44 || Bits(out[0]) == 0xc2ce6f45),
		  "V2: -1.0321732e+002 reads as one of the two neighbours it prints for");
	// V2's float16 text is exact.
	text = Ascii("[9.9975586e-002,0.0000000e+000,6.5504000e+004]");
	uint16_t h0 = 0, h1 = 0, h2 = 0;
	Check(ParseText(text.data(), text.size(), false, 3, out.data(), scratch) == VectorStatus::Ok &&
			  FloatToHalf(out[0], h0) && h0 == 0x2E66 && FloatToHalf(out[1], h1) && h1 == 0 &&
			  FloatToHalf(out[2], h2) && h2 == 0x7BFF,
		  "V2 float16 [0.1, 0, 65504]: exact");
	// UTF-16 (a session without UTF8SUPPORT).
	auto wide = Utf16(" [ 1e0 , 2.5 ,-3 ] ");
	Check(ParseText(wide.data(), wide.size(), true, 3, out.data(), scratch) == VectorStatus::Ok && out[0] == 1 &&
			  out[1] == 2.5f && out[2] == -3,
		  "UTF-16 text, spaces around");
	// Refusals.
	const char *bad_syntax[] = {"", "1,2,3", "[1,2,3", "[1,,3]", "[1;2;3]", "[1,2,3]x", "[a,b,c]", "[]"};
	for (auto *s : bad_syntax) {
		auto b = Ascii(s);
		const auto status = ParseText(b.data(), b.size(), false, 3, out.data(), scratch);
		Check(status == VectorStatus::TextSyntax || status == VectorStatus::TextCount,
			  std::string("refused: '") + s + "'");
	}
	text = Ascii("[1,2]");
	Check(ParseText(text.data(), text.size(), false, 3, out.data(), scratch) == VectorStatus::TextCount,
		  "two elements for three: count");
	text = Ascii("[1,2,3,4]");
	Check(ParseText(text.data(), text.size(), false, 3, out.data(), scratch) == VectorStatus::TextCount,
		  "four elements for three: count");
	auto non_ascii = Utf16("[1,2,3]");
	non_ascii[2] = 0x31;
	non_ascii[3] = 0x04;  // U+0431
	Check(ParseText(non_ascii.data(), non_ascii.size(), true, 3, out.data(), scratch) == VectorStatus::TextSyntax,
		  "a UTF-16 unit above 0x7F: syntax");
}

// --- the TDS framing and the DuckDB half ---

void TestColMetadata() {
	// V1: `ff000000 0900 f5 1400 00 01 7600` -- UserType 255, flags 0x0009, VECTORTYPE, max length 20,
	// element type 0, the name `v`; behind a column count of 1.
	const auto token =
		Hex("0100"
			"ff000000"
			"0900"
			"f5"
			"1400"
			"00"
			"01"
			"7600");
	std::vector<tds::ColumnMetadata> columns;
	size_t consumed = 0;
	Check(tds::ColumnMetadataParser::Parse(token.data(), token.size(), consumed, columns) && columns.size() == 1 &&
			  consumed == token.size(),
		  "V1's COLMETADATA parses");
	if (columns.size() == 1) {
		const auto &c = columns[0];
		Check(c.type_id == tds::TDS_TYPE_VECTOR && c.max_length == 20 && c.vector_element_type == 0 &&
				  c.user_type == tds::VECTOR_USER_TYPE && c.name == "v" && c.VectorDimensions() == 3,
			  "V1's column: VECTORTYPE, 20 bytes, float32, UserType 255, 3 dims");
		Check(tds::encoding::TypeConverter::GetDuckDBType(c) == FloatArrayType(3), "a raw scan types it FLOAT[3]");
	}
	// vector(1998): `f5 401f 00`.
	const auto big =
		Hex("0100"
			"ff000000"
			"0900"
			"f5"
			"401f"
			"00"
			"01"
			"7600");
	Check(tds::ColumnMetadataParser::Parse(big.data(), big.size(), consumed, columns) &&
			  columns[0].VectorDimensions() == 1998,
		  "vector(1998): 8000 bytes, 1998 dims");
	auto bad_type =
		Hex("0100"
			"ff000000"
			"0900"
			"f5"
			"1400"
			"02"
			"01"
			"7600");
	Check(
		!Throws([&] { tds::ColumnMetadataParser::Parse(bad_type.data(), bad_type.size(), consumed, columns); }).empty(),
		"element type 2: malformed");
	auto bad_len =
		Hex("0100"
			"ff000000"
			"0900"
			"f5"
			"0900"
			"00"
			"01"
			"7600");
	Check(!Throws([&] { tds::ColumnMetadataParser::Parse(bad_len.data(), bad_len.size(), consumed, columns); }).empty(),
		  "max length 9: malformed");
	// The JSON form (V1: `a7 ffff 0904002600`, UserType 255): a varchar(max) a raw scan reads as VARCHAR.
	const auto text =
		Hex("0100"
			"ff000000"
			"0900"
			"a7"
			"ffff"
			"0904002600"
			"01"
			"7600");
	Check(tds::ColumnMetadataParser::Parse(text.data(), text.size(), consumed, columns) && columns[0].IsVectorText() &&
			  !columns[0].IsVectorWire() &&
			  tds::encoding::TypeConverter::GetDuckDBType(columns[0]) == LogicalType::VARCHAR,
		  "the JSON form: UserType 255 kept, VARCHAR for a raw scan");

	// The row framing: u16 length + bytes, 0xFFFF NULL.
	std::vector<tds::ColumnMetadata> one = {WireColumn(3, 0)};
	tds::RowReader reader(one);
	const auto row = Hex("1400a9010300000000000000803f0000004000004040");
	tds::RowData data;
	size_t used = 0;
	Check(reader.ReadRow(row.data(), row.size(), used, data) && used == row.size() && !data.null_mask[0] &&
			  data.values[0].size() == 20,
		  "a VECTORTYPE value: 2 + 20 bytes");
	const auto null_row = Hex("ffff");
	Check(reader.ReadRow(null_row.data(), null_row.size(), used, data) && used == 2 && data.null_mask[0],
		  "a VECTORTYPE NULL: 0xFFFF");
}

void TestRead() {
	// The per-value path, both forms, into FLOAT[3].
	Vector out(FloatArrayType(3), 4);
	const auto f32 = Hex("a9010300000000000000803f0000004000004040");
	DecodeFromTds(f32, WireColumn(3, 0), out, 0);
	DecodeFromTds(Ascii("[4.0000000e+000,5.0000000e+000,6.0000000e+000]"), TextColumn(tds::TDS_TYPE_BIGVARCHAR), out,
				  1);
	DecodeFromTds(Utf16("[7,8,9]"), TextColumn(tds::TDS_TYPE_NVARCHAR), out, 2);
	const auto f16 =
		Hex("a90103000100000000"
			"3c00400042");
	DecodeFromTds(f16, WireColumn(3, 1), out, 3);
	const float *e = FlatVector::GetData<float>(ArrayVector::GetChild(out));
	Check(e[0] == 1 && e[2] == 3 && e[3] == 4 && e[5] == 6 && e[6] == 7 && e[8] == 9 && e[9] == 1 && e[11] == 3,
		  "the per-value path: bytes, UTF-8 text, UTF-16 text, float16 bytes");
	const auto wrong = Hex("a9010400000000000000803f0000004000004040");
	const auto error = Throws([&] { DecodeFromTds(wrong, WireColumn(3, 0), out, 0); });
	Check(error.find("column \"v\"") != std::string::npos && error.find("FLOAT[3]") != std::string::npos,
		  "a value of another shape refused naming the column, got '" + error + "'");
	Check(!Throws([&] { DecodeFromTds(Ascii("[1,2]"), TextColumn(tds::TDS_TYPE_BIGVARCHAR), out, 0); }).empty(),
		  "a text of two elements for FLOAT[3] refused");

	// The staged path chooses the Vector kernel for both forms, and the text form only into FLOAT[n].
	using namespace duckdb::mssql::codec::staging;
	Check(ResolveColumnOps(WireColumn(3, 0), FloatArrayType(3)).kernel == FinalizeKernel::Vector,
		  "VECTORTYPE into FLOAT[3]: the Vector kernel");
	Check(ResolveColumnOps(TextColumn(tds::TDS_TYPE_BIGVARCHAR), FloatArrayType(3)).kernel == FinalizeKernel::Vector,
		  "the JSON form into FLOAT[3]: the Vector kernel");
	Check(ResolveColumnOps(TextColumn(tds::TDS_TYPE_BIGVARCHAR), LogicalType::VARCHAR).kernel != FinalizeKernel::Vector,
		  "the JSON form into VARCHAR (a raw scan): the text, not the kernel");
}

void TestWrite() {
	// The statement path: 9 significant digits, which parse back to the same float32.
	const auto literal = FormatLiteral(FloatArray({1.0f, 0.1f, 3.4028235e38f}), "v");
	Check(literal.rfind("N'[", 0) == 0 && literal.substr(literal.size() - 2) == "]'",
		  "a literal N'[...]', got " + literal);
	{
		std::string scratch;
		std::vector<float> back(3);
		const std::string inner = literal.substr(2, literal.size() - 3);
		auto bytes = Ascii(inner);
		Check(ParseText(bytes.data(), bytes.size(), false, 3, back.data(), scratch) == VectorStatus::Ok &&
				  back[0] == 1.0f && back[1] == 0.1f && back[2] == 3.4028235e38f,
			  "the literal's digits read back as the float32s, got " + literal);
	}
	Check(FormatLiteral(Value(FloatArrayType(3)), "v") == "NULL", "a NULL value: NULL");
	auto with_null = Value::ARRAY(LogicalType::FLOAT, {Value::FLOAT(1), Value(LogicalType::FLOAT), Value::FLOAT(3)});
	auto e = Throws([&] { FormatLiteral(with_null, "v"); });
	Check(e.find("column \"v\"") != std::string::npos && e.find("element 2") != std::string::npos,
		  "a NULL element refused by name, got '" + e + "'");
	e = Throws([&] { FormatLiteral(FloatArray({1, NAN, 3}), "v"); });
	Check(e.find("NaN") != std::string::npos, "a NaN refused, got '" + e + "'");

	// The bulk wire: a u16 length and the bytes.
	const auto f32_col = mssql::BCPColumnMetadata::FromServerColumn("v", "vector", 20, 0, 0, true, "");
	const auto f16_col = mssql::BCPColumnMetadata::FromServerColumn("h", "vector", 14, 0, 1, true, "");
	Check(f32_col.IsVector() && f32_col.vector_dims == 3 && f32_col.vector_element_type == 0 &&
			  f32_col.tds_type_token == tds::TDS_TYPE_BIGVARBINARY && f32_col.max_length == 20 &&
			  f32_col.GetSQLServerTypeDeclaration() == "vector(3)" && !f32_col.bulk_unsupported &&
			  f32_col.duckdb_type == FloatArrayType(3),
		  "an existing vector(3): varbinary(20) under a declared vector(3)");
	Check(f16_col.IsVector() && f16_col.vector_dims == 3 && f16_col.vector_element_type == 1 &&
			  f16_col.GetSQLServerTypeDeclaration() == "vector(3, float16)",
		  "an existing vector(3, float16): declared so, " + f16_col.GetSQLServerTypeDeclaration());
	duckdb::vector<uint8_t> wire;
	EncodeValueToBcp(FloatArray({1, 2, 3}), f32_col, wire);
	Check(std::vector<uint8_t>(wire.begin(), wire.end()) == Hex("1400a9010300000000000000803f0000004000004040"),
		  "a float32 value on the wire: V6's case 7");
	wire.clear();
	EncodeValueToBcp(FloatArray({1, 2, 3}), f16_col, wire);
	Check(std::vector<uint8_t>(wire.begin(), wire.end()) == Hex("0e00a90103000100000000"
																"3c00400042"),
		  "a float16 value on the wire: V6's case 17");
	wire.clear();
	EncodeValueToBcp(Value(FloatArrayType(3)), f32_col, wire);
	Check(wire.size() == 2 && wire[0] == 0xFF && wire[1] == 0xFF, "a NULL: 0xFFFF");
	e = Throws([&] {
		duckdb::vector<uint8_t> w;
		EncodeValueToBcp(FloatArray({1, 2, 70000}), f16_col, w);
	});
	Check(e.find("column \"h\"") != std::string::npos && e.find("element 3") != std::string::npos &&
			  e.find("65504") != std::string::npos,
		  "a float16 overflow refused by name, got '" + e + "'");

	// CTAS / COPY create vector(n) from FLOAT[n], and refuse the rest by name.
	Check(FormatDdlTypeName(FloatArrayType(3)) == "vector(3)", "FLOAT[3] creates vector(3)");
	e = Throws([&] { FormatDdlTypeName(LogicalType::ARRAY(LogicalType::DOUBLE, 3)); });
	Check(e.find("::FLOAT[3]") != std::string::npos, "DOUBLE[3] refused with the cast named, got '" + e + "'");
	Check(!Throws([&] { FormatDdlTypeName(FloatArrayType(1999)); }).empty(), "FLOAT[1999] refused");
	Check(!Throws([&] { FormatDdlTypeName(LogicalType::ARRAY(LogicalType::INTEGER, 3)); }).empty(),
		  "INTEGER[3] refused");
	const auto created = mssql::TargetResolver::GenerateColumnMetadata({FloatArrayType(4)}, {"v"});
	Check(created.size() == 1 && created[0].IsVector() && created[0].vector_dims == 4 &&
			  created[0].vector_element_type == 0 && created[0].max_length == 24 &&
			  created[0].GetSQLServerTypeDeclaration() == "vector(4)",
		  "a created FLOAT[4] column: vector(4) on the wire as varbinary(24)");
}

}  // namespace

int main() {
	TestBytes();
	TestFloat16();
	TestText();
	TestColMetadata();
	TestRead();
	TestWrite();
	if (failures) {
		std::cerr << failures << " check(s) failed\n";
		return 1;
	}
	std::cout << "test_vector_codec: all checks passed\n";
	return 0;
}
