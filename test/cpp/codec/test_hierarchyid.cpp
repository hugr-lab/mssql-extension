// Spec 093 T4 (D5): hierarchyid's MS-SSCLRT 2.2.2 bit string as its ToString() text.
//
// The kernel (codec/clr/hierarchyid.cpp) against the server's own bytes (G2: the root, /1/2/, /1.5/-2/),
// against the bit layout of each label range at its boundaries, and as a round trip text -> bytes -> text
// over H1's shapes (depth 20, negative and fractional labels, every range's first and last value, at the end
// of a level and inside one); malformed values each refused with their own status. Then the read path: a
// stream's hierarchyid column through the RowStager's chunk kernel and the per-value TypeConverter, NULLs,
// the zero-byte root and a malformed value named by its column. No SQL Server; the server's check of every
// H1 value is test/sql/types/hierarchyid_read.test.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "codec/clr/hierarchyid.hpp"
#include "codec/clr/hierarchyid_codec.hpp"
#include "codec/staging/row_stager.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "tds/encoding/type_converter.hpp"
#include "tds/tds_column_metadata.hpp"

using duckdb::mssql::codec::hierarchyid::Decode;
using duckdb::mssql::codec::hierarchyid::Encode;
using duckdb::mssql::codec::hierarchyid::HierarchyIdStatus;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

std::vector<uint8_t> FromHex(const std::string &h) {
	std::vector<uint8_t> b;
	for (size_t i = 0; i + 1 < h.size(); i += 2) {
		b.push_back(static_cast<uint8_t>(std::stoi(h.substr(i, 2), nullptr, 16)));
	}
	return b;
}

std::string ToHex(const std::vector<uint8_t> &b) {
	static const char *digits = "0123456789ABCDEF";
	std::string h;
	for (uint8_t c : b) {
		h += digits[c >> 4];
		h += digits[c & 15];
	}
	return h;
}

//! A bit string ("0110...") as bytes, padded with zero bits to a byte, as the server stores it.
std::vector<uint8_t> FromBits(const std::string &bits) {
	std::vector<uint8_t> b((bits.size() + 7) / 8, 0);
	for (size_t i = 0; i < bits.size(); i++) {
		if (bits[i] == '1') {
			b[i / 8] |= static_cast<uint8_t>(0x80 >> (i % 8));
		}
	}
	return b;
}

std::string DecodeText(const std::vector<uint8_t> &b, HierarchyIdStatus *status = nullptr) {
	std::string out;
	const auto s = Decode(b.data(), b.size(), out);
	if (status) {
		*status = s;
	}
	return s == HierarchyIdStatus::Ok ? out : "<" + std::string(DescribeStatus(s)) + ">";
}

bool RoundTrips(const std::string &text, std::string *why = nullptr) {
	std::vector<uint8_t> bytes;
	const auto encoded = Encode(text, bytes);
	if (encoded != HierarchyIdStatus::Ok) {
		if (why) {
			*why = "encode: " + std::string(DescribeStatus(encoded));
		}
		return false;
	}
	const std::string back = DecodeText(bytes);
	if (back != text && why) {
		*why = "decoded " + back + " from " + ToHex(bytes);
	}
	return back == text;
}

void TestServerBytes() {
	std::cout << "The server's bytes (G2, SQL Server 2022)" << std::endl;
	Check(DecodeText({}) == "/", "the root: zero bytes, /");
	Check(DecodeText(FromHex("5B40")) == "/1/2/", "0x5B40: /1/2/");
	Check(DecodeText(FromHex("6467D0")) == "/1.5/-2/", "0x6467D0: /1.5/-2/");
	std::vector<uint8_t> b;
	Check(Encode("/1/2/", b) == HierarchyIdStatus::Ok && ToHex(b) == "5B40", "encode /1/2/: 5B40");
	Check(Encode("/1.5/-2/", b) == HierarchyIdStatus::Ok && ToHex(b) == "6467D0", "encode /1.5/-2/: 6467D0");
	Check(Encode("/", b) == HierarchyIdStatus::Ok && b.empty(), "encode /: zero bytes");
}

//! One label of each range, written out bit by bit from MS-SSCLRT 2.2.2's patterns: the decoder must
//! read the prefix, the value bits around the fixed bits, and T.
void TestLayouts() {
	std::cout << "Each label range's bit layout" << std::endl;
	struct Case {
		const char *bits;
		const char *text;
	};
	const Case cases[] = {
		{"01"
		 "00"
		 "1",
		 "/0/"},
		{"01"
		 "11"
		 "1",
		 "/3/"},
		{"100"
		 "00"
		 "1",
		 "/4/"},
		{"100"
		 "11"
		 "1",
		 "/7/"},
		{"101"
		 "000"
		 "1",
		 "/8/"},
		{"101"
		 "111"
		 "1",
		 "/15/"},
		// 16 + 0b000000, the pattern xx0x1xxxT
		{"110"
		 "00"
		 "0"
		 "0"
		 "1"
		 "000"
		 "1",
		 "/16/"},
		{"110"
		 "11"
		 "0"
		 "1"
		 "1"
		 "111"
		 "1",
		 "/79/"},
		// 80: xxx0xxx0x1xxxT
		{"1110"
		 "000"
		 "0"
		 "000"
		 "0"
		 "0"
		 "1"
		 "000"
		 "1",
		 "/80/"},
		{"1110"
		 "111"
		 "0"
		 "111"
		 "0"
		 "1"
		 "1"
		 "111"
		 "1",
		 "/1103/"},
		// 1104: xxxxx0xxx0x1xxxT
		{"11110"
		 "00000"
		 "0"
		 "000"
		 "0"
		 "0"
		 "1"
		 "000"
		 "1",
		 "/1104/"},
		{"11110"
		 "11111"
		 "0"
		 "111"
		 "0"
		 "1"
		 "1"
		 "111"
		 "1",
		 "/5199/"},
		// -8 .. -1: 00111 xxxT
		{"00111"
		 "000"
		 "1",
		 "/-8/"},
		{"00111"
		 "111"
		 "1",
		 "/-1/"},
		// -72 .. -9: 0010 xx0x1xxxT
		{"0010"
		 "00"
		 "0"
		 "0"
		 "1"
		 "000"
		 "1",
		 "/-72/"},
		{"0010"
		 "11"
		 "0"
		 "1"
		 "1"
		 "111"
		 "1",
		 "/-9/"},
		// -4168 .. -73: 000110 xxxxx0xxx0x1xxxT
		{"000110"
		 "00000"
		 "0"
		 "000"
		 "0"
		 "0"
		 "1"
		 "000"
		 "1",
		 "/-4168/"},
		{"000110"
		 "11111"
		 "0"
		 "111"
		 "0"
		 "1"
		 "1"
		 "111"
		 "1",
		 "/-73/"},
		// A continued level: /0.0/ is stored 1, then 0 ending the level.
		{"01"
		 "01"
		 "0"
		 "01"
		 "00"
		 "1",
		 "/0.0/"},
		// /-1.0/: -1 continued is stored 0 (01 00 0), then 0.
		{"01"
		 "00"
		 "0"
		 "01"
		 "00"
		 "1",
		 "/-1.0/"},
	};
	for (const auto &c : cases) {
		const auto bytes = FromBits(c.bits);
		const std::string got = DecodeText(bytes);
		std::vector<uint8_t> encoded;
		Encode(c.text, encoded);
		Check(got == c.text && encoded == bytes, std::string(c.text) + " is " + ToHex(bytes) +
													 (got == c.text ? "" : ", decoded " + got) +
													 (encoded == bytes ? "" : ", encoded " + ToHex(encoded)));
	}
}

void TestRoundTrips() {
	std::cout << "H1's shapes, text -> bytes -> text" << std::endl;
	// Every range's first and last value, ending a level and continuing one (a continuing label is stored
	// as value + 1, so its own range is one lower).
	const int64_t bounds[] = {0,
							  3,
							  4,
							  7,
							  8,
							  15,
							  16,
							  79,
							  80,
							  1103,
							  1104,
							  5199,
							  5200,
							  4294972495LL,
							  4294972496LL,
							  281479271683151LL,
							  -1,
							  -8,
							  -9,
							  -72,
							  -73,
							  -4168,
							  -4169,
							  -4294971464LL,
							  -4294971465LL,
							  -281479271682120LL};
	int bad = 0;
	for (int64_t v : bounds) {
		const std::string s = std::to_string(v);
		for (const std::string &text : {"/" + s + "/", "/" + std::to_string(v - 1) + ".1/", "/1/" + s + "/2/"}) {
			std::string why;
			if (!RoundTrips(text, &why)) {
				bad++;
				std::cout << "    " << text << ": " << why << std::endl;
			}
		}
	}
	Check(bad == 0, "every range boundary, at a level's end and inside one");
	// Past the ends: a label one beyond the largest / smallest is out of range, as a continued one is.
	std::vector<uint8_t> b;
	Check(Encode("/281479271683152/", b) == HierarchyIdStatus::OutOfRange, "281479271683152 is out of range");
	Check(Encode("/-281479271682121/", b) == HierarchyIdStatus::OutOfRange, "-281479271682121 is out of range");
	Check(Encode("/281479271683151.1/", b) == HierarchyIdStatus::OutOfRange,
		  "281479271683151 inside a level (stored + 1) is out of range");
	Check(RoundTrips("/281479271683150.1/"), "281479271683150 inside a level is the last there");
	Check(RoundTrips("/-281479271682121.1/"), "-281479271682121 inside a level is the first there");

	std::string depth20 = "/";
	for (int i = 1; i <= 20; i++) {
		depth20 += std::to_string(i * 37 - 100) + "/";
	}
	Check(RoundTrips(depth20), "depth 20: " + depth20);
	for (const char *text : {"/-1/", "/1.5/", "/1.-2/", "/0.0.0.1/", "/-5.-7.3/", "/1/1.1/1.1.1/", "/3.17.-80/"}) {
		Check(RoundTrips(text), std::string("negative and fractional: ") + text);
	}
	// The largest label is 60 bits (a 6-bit prefix, 48 value bits, 5 fixed bits and T): 118 of them are
	// 7080 bits, 885 bytes; 119 are 893 bytes, past the 892 a hierarchyid holds.
	std::string deep = "/";
	for (int i = 0; i < 118; i++) {
		deep += "281479271683151/";
	}
	Check(RoundTrips(deep), "118 labels of 60 bits: 885 bytes");
	Check(Encode(deep + "281479271683151/", b) == HierarchyIdStatus::TooLong, "119 of them: past 892 bytes");
}

void TestMalformed() {
	std::cout << "Malformed values, each refused" << std::endl;
	HierarchyIdStatus s;
	DecodeText(std::vector<uint8_t>(893, 0x58), &s);
	Check(s == HierarchyIdStatus::TooLong, "893 bytes: too long");
	DecodeText(FromHex("00"), &s);
	Check(s == HierarchyIdStatus::BadPadding, "a zero byte: padding of eight bits");
	DecodeText(FromHex("5800"), &s);
	Check(s == HierarchyIdStatus::BadPadding, "/1/ then a zero byte: eleven bits of padding");
	DecodeText(FromHex("08"), &s);
	Check(s == HierarchyIdStatus::UnknownPrefix, "0000 1...: no range starts with four zero bits");
	DecodeText(FromHex("40"), &s);
	Check(s == HierarchyIdStatus::Truncated, "0x40: a label continuing its level, then nothing");
	DecodeText(FromHex("FC"), &s);
	Check(s == HierarchyIdStatus::Truncated, "111111 and two bits: the 60-bit label cut short");
	DecodeText(FromBits("110"
						"00"
						"1"
						"0"
						"1"
						"000"
						"1"),
			   &s);
	Check(s == HierarchyIdStatus::BadFixedBit, "16's pattern with its fixed 0 set");
	DecodeText(FromBits("110"
						"00"
						"0"
						"0"
						"0"
						"000"
						"1"),
			   &s);
	Check(s == HierarchyIdStatus::BadFixedBit, "16's pattern with its fixed 1 clear");
	DecodeText(FromHex("5C"), &s);
	Check(s != HierarchyIdStatus::Ok, "/1/ with a 1 in its padding");
	for (const char *text :
		 {"", "1/", "/1", "//", "/1//", "/a/", "/1./", "/.1/", "/1..2/", "/--1/", "/-/", "/1234567890123456789/"}) {
		std::vector<uint8_t> b;
		Check(Encode(text, b) == HierarchyIdStatus::TextSyntax, std::string("text '") + text + "': not a path");
	}
}

//===--------------------------------------------------------------------===//
// The read path: a hierarchyid column of a stream, through the stager and the per-value converter.
//===--------------------------------------------------------------------===//

using duckdb::idx_t;
using duckdb::LogicalType;
using duckdb::tds::ColumnMetadata;

ColumnMetadata HierarchyIdColumn(const std::string &name) {
	ColumnMetadata c;
	c.name = name;
	c.type_id = duckdb::tds::TDS_TYPE_UDT;
	c.max_length = 892;
	c.flags = 0x01;	 // nullable
	c.udt_schema = "sys";
	c.udt_type_name = "hierarchyid";
	return c;
}

//! A UDT value as the row carries it: PLP (G2), a u64 total, one chunk, the terminator; NULL as PLP_NULL.
void AppendPlp(std::vector<uint8_t> &row, const std::vector<uint8_t> *value) {
	auto put = [&row](uint64_t v, int bytes) {
		for (int i = 0; i < bytes; i++) {
			row.push_back(static_cast<uint8_t>(v >> (8 * i)));
		}
	};
	if (!value) {
		put(~uint64_t(0), 8);
		return;
	}
	put(value->size(), 8);
	if (!value->empty()) {
		put(value->size(), 4);
		row.insert(row.end(), value->begin(), value->end());
	}
	put(0, 4);
}

void TestReadPath() {
	std::cout << "A stream's hierarchyid column, staged and per value" << std::endl;
	const ColumnMetadata column = HierarchyIdColumn("node");
	Check(duckdb::tds::encoding::TypeConverter::GetDuckDBType(column) == LogicalType::VARCHAR, "typed VARCHAR");

	std::vector<uint8_t> long_value;
	const std::string long_text = "/1/2/3/4/5/6/7/8/9/10/11/12/-3.4/";
	Encode(long_text, long_value);
	const std::vector<uint8_t> root;
	const std::vector<uint8_t> g2 = FromHex("6467D0");
	const std::vector<const std::vector<uint8_t> *> values = {&root, nullptr, &g2, &long_value, &root};
	const std::vector<std::string> want = {"/", "NULL", "/1.5/-2/", long_text, "/"};

	std::vector<ColumnMetadata> columns = {column};
	duckdb::DataChunk chunk;
	chunk.Initialize(duckdb::Allocator::DefaultAllocator(), {LogicalType::VARCHAR});
	std::vector<duckdb::Vector *> targets = {&chunk.data[0]};
	duckdb::mssql::codec::staging::RowStager stager;
	stager.Configure(columns, targets);
	stager.BeginChunk(targets);
	bool framed = true;
	for (idx_t r = 0; r < values.size(); r++) {
		std::vector<uint8_t> row;
		AppendPlp(row, values[r]);
		framed = stager.StageRow(row.data(), row.size(), r) == row.size() && framed;
	}
	stager.FinalizeChunk(values.size());
	chunk.SetChildCardinality(values.size());
	bool all = framed;
	for (idx_t r = 0; r < values.size(); r++) {
		const auto v = chunk.data[0].GetValue(r);
		const std::string got = v.IsNull() ? "NULL" : v.ToString();
		if (got != want[r]) {
			all = false;
			std::cout << "    row " << r << ": " << got << ", want " << want[r] << std::endl;
		}
	}
	Check(all, "staged: the root, NULL, /1.5/-2/, a text past string_t's inline 12 bytes, the root again");

	// Every row one value: the constant path (TryEmitConstant), decoded once through DecodeFirstValue.
	{
		duckdb::DataChunk same;
		same.Initialize(duckdb::Allocator::DefaultAllocator(), {LogicalType::VARCHAR});
		std::vector<duckdb::Vector *> t = {&same.data[0]};
		duckdb::mssql::codec::staging::RowStager s;
		s.Configure(columns, t);
		s.BeginChunk(t);
		for (idx_t r = 0; r < 4; r++) {
			std::vector<uint8_t> row;
			AppendPlp(row, &g2);
			s.StageRow(row.data(), row.size(), r);
		}
		s.FinalizeChunk(4);
		same.SetChildCardinality(4);
		Check(same.data[0].GetVectorType() == duckdb::VectorType::CONSTANT_VECTOR &&
				  same.data[0].GetValue(3).ToString() == "/1.5/-2/",
			  "four equal values: a CONSTANT vector of /1.5/-2/");
	}

	// Per value (the RETURNING parser's and the fallback's path); a hierarchyid has no decode notes.
	duckdb::Vector out(LogicalType::VARCHAR, 4);
	duckdb::tds::encoding::TypeConverter::ConvertValue(g2, false, column, out, 0, nullptr, nullptr);
	duckdb::tds::encoding::TypeConverter::ConvertValue(root, false, column, out, 1, nullptr, nullptr);
	duckdb::tds::encoding::TypeConverter::ConvertValue(long_value, false, column, out, 2, nullptr, nullptr);
	Check(out.GetValue(0).ToString() == "/1.5/-2/" && out.GetValue(1).ToString() == "/" &&
			  out.GetValue(2).ToString() == long_text,
		  "per value: /1.5/-2/, /, the long text");

	// A malformed value is an error naming the column, staged or per value.
	std::string staged_error;
	try {
		duckdb::mssql::codec::staging::RowStager bad;
		duckdb::DataChunk c2;
		c2.Initialize(duckdb::Allocator::DefaultAllocator(), {LogicalType::VARCHAR});
		std::vector<duckdb::Vector *> t2 = {&c2.data[0]};
		bad.Configure(columns, t2);
		bad.BeginChunk(t2);
		const std::vector<uint8_t> junk = FromHex("08");
		std::vector<uint8_t> row;
		AppendPlp(row, &junk);
		bad.StageRow(row.data(), row.size(), 0);
		bad.FinalizeChunk(1);
	} catch (std::exception &e) {
		staged_error = e.what();
	}
	Check(staged_error.find("'node'") != std::string::npos && staged_error.find("hierarchyid") != std::string::npos,
		  "staged, malformed: " + staged_error);
	std::string value_error;
	try {
		duckdb::tds::encoding::TypeConverter::ConvertValue(FromHex("40"), false, column, out, 3, nullptr, nullptr);
	} catch (std::exception &e) {
		value_error = e.what();
	}
	Check(value_error.find("'node'") != std::string::npos, "per value, malformed: " + value_error);

	// A user CLR type stays bytes: the binary kernel, a BLOB.
	ColumnMetadata user = column;
	user.udt_schema = "dbo";
	user.udt_type_name = "Point";
	Check(duckdb::tds::encoding::TypeConverter::GetDuckDBType(user) == LogicalType::BLOB, "dbo.Point: a BLOB");
	std::vector<ColumnMetadata> user_columns = {user};
	duckdb::DataChunk c3;
	c3.Initialize(duckdb::Allocator::DefaultAllocator(), {LogicalType::BLOB});
	std::vector<duckdb::Vector *> t3 = {&c3.data[0]};
	duckdb::mssql::codec::staging::RowStager user_stager;
	user_stager.Configure(user_columns, t3);
	user_stager.BeginChunk(t3);
	std::vector<uint8_t> row;
	const std::vector<uint8_t> payload = FromHex("0102FF00");
	AppendPlp(row, &payload);
	user_stager.StageRow(row.data(), row.size(), 0);
	user_stager.FinalizeChunk(1);
	c3.SetChildCardinality(1);
	Check(c3.data[0].GetValue(0) == duckdb::Value::BLOB(payload.data(), payload.size()),
		  "a user type's value: its bytes, " + c3.data[0].GetValue(0).ToString());
}

// Spec 093 T8 (review of #621): the staged kernel with notes, as the backup reader calls it -- a value
// that is not a hierarchyid's bytes (a damaged page, or one the server took unchecked) is NULL and counted,
// its row read; with none, as on the wire, it is refused naming the column.
void TestUnreadableAsNull() {
	std::cout << "An unreadable value: NULL and counted with notes, refused without" << std::endl;
	const ColumnMetadata column = HierarchyIdColumn("node");
	const std::vector<std::vector<uint8_t>> values = {
		FromHex("6467D0"), FromHex("5800"), {}, FromHex("08"), FromHex("58")};
	duckdb::mssql::codec::staging::ColumnStaging st;
	st.kind = duckdb::mssql::codec::staging::StagingKind::Var;
	st.offsets.resize(values.size());
	st.lengths.resize(values.size());
	st.validity_words.assign(1, ~uint64_t(0));
	for (idx_t r = 0; r < values.size(); r++) {
		st.offsets[r] = static_cast<uint32_t>(st.payload_used);
		st.lengths[r] = static_cast<uint32_t>(values[r].size());
		st.buffer.insert(st.buffer.end(), values[r].begin(), values[r].end());
		st.payload_used += values[r].size();
	}
	st.count = values.size();
	duckdb::Vector out(LogicalType::VARCHAR, values.size());
	duckdb::mssql::codec::hierarchyid::HierarchyIdColumnNotes notes;
	duckdb::mssql::codec::hierarchyid::DecodeChunkFromStaging(st, values.size(), column, out, &notes);
	const std::vector<std::string> want = {"/1.5/-2/", "NULL", "/", "NULL", "/1/"};
	bool all = true;
	for (idx_t r = 0; r < values.size(); r++) {
		const auto v = out.GetValue(r);
		const std::string got = v.IsNull() ? "NULL" : v.ToString();
		if (got != want[r]) {
			all = false;
			std::cout << "    row " << r << ": " << got << ", want " << want[r] << std::endl;
		}
	}
	Check(all, "with notes: the readable values read, the two that are not NULL");
	Check(notes.malformed == 2 && notes.first_malformed == HierarchyIdStatus::BadPadding,
		  "with notes: two counted, the first's status kept (bad padding)");
	std::string error;
	try {
		duckdb::Vector refused(LogicalType::VARCHAR, values.size());
		duckdb::mssql::codec::hierarchyid::DecodeChunkFromStaging(st, values.size(), column, refused, nullptr);
	} catch (std::exception &e) {
		error = e.what();
	}
	Check(error.find("'node'") != std::string::npos && error.find("cannot be read") != std::string::npos,
		  "without notes: refused, naming the column: " + error);
}

}  // namespace

int main() {
	std::cout << "hierarchyid on the client (spec 093 T4, D5)" << std::endl;
	TestServerBytes();
	TestLayouts();
	TestRoundTrips();
	TestMalformed();
	TestReadPath();
	TestUnreadableAsNull();
	if (failures) {
		std::cout << failures << " FAILED" << std::endl;
		return 1;
	}
	std::cout << "All hierarchyid tests PASSED" << std::endl;
	return 0;
}
