// test/cpp/codec/test_row_stager.cpp
// Unit tests for the staged read path's row walks (spec 059 D1b).
//
// Does NOT require a running SQL Server instance: both walks take a raw byte
// pointer, so the rows are built here. That is the point — a live server sends
// NBCROW only when the null bitmap costs less than the NULL markers it replaces,
// which depends on the result set's shape rather than the table's, so no fixture
// here had ever produced one and the walk shipped with three out-of-bounds
// defects and no test. (test/sql/query/nbc_row.test covers the live form, and
// documents the pricing rule.)
//
// Covers:
//   - the NULL bitmap convention itself, asserted directly rather than assumed;
//   - every append arm reached through the NBC walk, value present;
//   - every append arm with its bitmap bit SET: zero bytes consumed, NULL
//     staged, and every other column still decoding — which is what proves the
//     "zero bytes" part rather than merely a row length that happens to add up;
//   - a 0xFFFF length prefix on a column the bitmap calls PRESENT, on both P2
//     arms (the defect @oluies found in PR #213: SkipValueNBC returns 2, the
//     walk consumed 65537);
//   - ROW and NBCROW forms of the same logical row producing identical output;
//   - the nbc_rows counter, without which every test here could silently stop
//     testing the NBC walk. The fixture enables the D10 counters the way
//     MSSQL_DEBUG>=2 does, so nothing in the walks exists solely for a test.
//
// Build & run:
//   make test-row-stager
//
// Built WITHOUT -DNDEBUG on purpose: `StageRow`/`StageNBCRow` end with
// `D_ASSERT(p == end)`, and that assertion is the framing check — it fires when
// a walk consumes a different number of bytes than the row holds.

#include "codec/staging/row_stager.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "tds/encoding/type_converter.hpp"
#include "tds/tds_types.hpp"

#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using duckdb::idx_t;
using duckdb::LogicalType;
using duckdb::Vector;
using duckdb::mssql::codec::staging::AppendArm;
using duckdb::mssql::codec::staging::RowStager;
using duckdb::tds::ColumnMetadata;
using duckdb::tds::encoding::TypeConverter;

namespace {

int failures = 0;

#define CHECK_TRUE(expr, label)                                                    \
	do {                                                                           \
		if (!(expr)) {                                                             \
			std::cerr << "FAIL: " << (label) << " (" << #expr << ")" << std::endl; \
			failures++;                                                            \
		}                                                                          \
	} while (0)

#define CHECK_EQ(actual, expected, label)                                                            \
	do {                                                                                             \
		auto a_ = (actual);                                                                          \
		auto e_ = (expected);                                                                        \
		if (!(a_ == e_)) {                                                                           \
			std::cerr << "FAIL: " << (label) << " — got " << a_ << ", expected " << e_ << std::endl; \
			failures++;                                                                              \
		}                                                                                            \
	} while (0)

//===--------------------------------------------------------------------===//
// Wire-format builders
//
// Hand-written on purpose. These are a THIRD, independent statement of the TDS
// row framing, alongside the walk in row_stager.cpp and SkipValue in
// tds_row_reader.cpp — the two the spec-059 differential test exists to keep
// from drifting. Deriving them from either would test nothing.
//===--------------------------------------------------------------------===//

typedef std::vector<uint8_t> Wire;

Wire Cat(const Wire &a, const Wire &b) {
	Wire out = a;
	out.insert(out.end(), b.begin(), b.end());
	return out;
}

//! Bare value bytes: TDS's non-nullable fixed types carry no prefix at all.
Wire Bare(std::initializer_list<int> bytes) {
	Wire out;
	for (int b : bytes) {
		out.push_back(static_cast<uint8_t>(b));
	}
	return out;
}

//! One length byte, 0 meaning NULL: the *N nullable variants and the temporal
//! types.
Wire P1(const Wire &value) {
	Wire out;
	out.push_back(static_cast<uint8_t>(value.size()));
	return Cat(out, value);
}

Wire P1Null() {
	return Wire(1, 0);
}

//! Two length bytes, 0xFFFF meaning NULL: the variable-length forms.
Wire P2(const Wire &value) {
	Wire out;
	out.push_back(static_cast<uint8_t>(value.size() & 0xFF));
	out.push_back(static_cast<uint8_t>(value.size() >> 8));
	return Cat(out, value);
}

Wire P2Null() {
	return Bare({0xFF, 0xFF});
}

//! PLP: 8-byte total length, then a chunk list, then a zero-length terminator.
Wire Plp(const std::vector<Wire> &chunks) {
	uint64_t total = 0;
	for (const Wire &c : chunks) {
		total += c.size();
	}
	Wire out(8);
	std::memcpy(out.data(), &total, 8);
	for (const Wire &c : chunks) {
		const uint32_t len = static_cast<uint32_t>(c.size());
		const size_t at = out.size();
		out.resize(at + 4);
		std::memcpy(out.data() + at, &len, 4);
		out.insert(out.end(), c.begin(), c.end());
	}
	out.resize(out.size() + 4, 0);	// terminator
	return out;
}

Wire PlpNull() {
	return Wire(8, 0xFF);
}

//! Legacy LOB (TEXT/NTEXT/IMAGE): a one-byte text-pointer length, the pointer,
//! an 8-byte row timestamp, then a 4-byte data length. Only the data is value.
Wire Lob(const Wire &value) {
	Wire out;
	out.push_back(16);
	out.resize(1 + 16, 0xAB);	   // text pointer — content is irrelevant to the walk
	out.resize(1 + 16 + 8, 0xCD);  // row timestamp — likewise
	const uint32_t len = static_cast<uint32_t>(value.size());
	const size_t at = out.size();
	out.resize(at + 4);
	std::memcpy(out.data() + at, &len, 4);
	return Cat(out, value);
}

Wire LobNull() {
	return Wire(1, 0);
}

//! ASCII as UTF-16LE, which is what every N-typed column carries.
Wire Utf16(const std::string &ascii) {
	Wire out;
	for (char c : ascii) {
		out.push_back(static_cast<uint8_t>(c));
		out.push_back(0);
	}
	return out;
}

Wire Ascii(const std::string &s) {
	return Wire(s.begin(), s.end());
}

//! The NBC null bitmap: ceil(n/8) bytes, LSB-first within each byte, bit SET
//! meaning NULL (row_stager.cpp: `bitmap[c >> 3] & (1u << (c & 7))`).
Wire NullBitmap(const std::vector<bool> &nulls) {
	Wire bitmap((nulls.size() + 7) / 8, 0);
	for (size_t c = 0; c < nulls.size(); c++) {
		if (nulls[c]) {
			bitmap[c >> 3] |= static_cast<uint8_t>(1u << (c & 7));
		}
	}
	return bitmap;
}

ColumnMetadata Meta(uint8_t type_id, uint16_t max_length, uint8_t precision = 0, uint8_t scale = 0) {
	ColumnMetadata c;
	c.name = "c";
	c.type_id = type_id;
	c.max_length = max_length;
	c.precision = precision;
	c.scale = scale;
	c.collation = 0;
	c.flags = 0;
	return c;
}

//===--------------------------------------------------------------------===//
// One column under test: its metadata, its wire bytes, and what it must decode
// to. `null_wire` is empty for the types that have NO NULL form in a plain ROW
// — TDS sends a nullable INT as INTN, so a bare INT column simply never carries
// a NULL there. In an NBCROW the bitmap can null ANY of them, which is exactly
// what makes the second walk worth testing.
//===--------------------------------------------------------------------===//

struct ArmCase {
	const char *label;
	ColumnMetadata meta;
	AppendArm arm;
	Wire value;
	Wire null_wire;
	const char *expected;
};

std::vector<ArmCase> AllArms() {
	using namespace duckdb::tds;
	std::vector<ArmCase> cases;

	cases.push_back(
		{"INT / RawDirect4", Meta(TDS_TYPE_INT, 4), AppendArm::RawDirect4, Bare({42, 0, 0, 0}), Wire(), "42"});
	cases.push_back(
		{"SMALLINT / RawDirect2", Meta(TDS_TYPE_SMALLINT, 2), AppendArm::RawDirect2, Bare({0xFE, 0xFF}), Wire(), "-2"});
	cases.push_back({"BIT / RawDirect1", Meta(TDS_TYPE_BIT, 1), AppendArm::RawDirect1, Bare({1}), Wire(), "true"});
	// 1.5f is 0x3FC00000, little-endian on the wire and in the vector alike.
	cases.push_back(
		{"REAL / RawDirect4", Meta(TDS_TYPE_REAL, 4), AppendArm::RawDirect4, Bare({0, 0, 0xC0, 0x3F}), Wire(), "1.5"});
	cases.push_back({"BIGINT / RawDirect8", Meta(TDS_TYPE_BIGINT, 8), AppendArm::RawDirect8,
					 Bare({1, 0, 0, 0, 0, 0, 0, 0}), Wire(), "1"});

	cases.push_back(
		{"INTN(1) / P1Direct1", Meta(TDS_TYPE_INTN, 1), AppendArm::P1Direct1, P1(Bare({7})), P1Null(), "7"});
	cases.push_back({"INTN(2) / P1Direct2", Meta(TDS_TYPE_INTN, 2), AppendArm::P1Direct2, P1(Bare({0x39, 0x30})),
					 P1Null(), "12345"});
	cases.push_back({"INTN(4) / P1Direct4", Meta(TDS_TYPE_INTN, 4), AppendArm::P1Direct4,
					 P1(Bare({0xD2, 0x02, 0x96, 0x49})), P1Null(), "1234567890"});
	cases.push_back({"INTN(8) / P1Direct8", Meta(TDS_TYPE_INTN, 8), AppendArm::P1Direct8,
					 P1(Bare({0xFF, 0, 0, 0, 0, 0, 0, 0})), P1Null(), "255"});
	cases.push_back(
		{"BITN / P1Direct1", Meta(TDS_TYPE_BITN, 1), AppendArm::P1Direct1, P1(Bare({0})), P1Null(), "false"});

	// MONEY: two int32s, high first, in units of 1/10000. 123456 -> 12.3456.
	cases.push_back({"MONEY / RawStageFixed", Meta(TDS_TYPE_MONEY, 8), AppendArm::RawStageFixed,
					 Bare({0, 0, 0, 0, 0x40, 0xE2, 0x01, 0x00}), Wire(), "12.3456"});
	// DATE: three bytes, days since 0001-01-01. 738899 = 2024-01-15.
	cases.push_back({"DATE / P1StageFixed", Meta(TDS_TYPE_DATE, 3), AppendArm::P1StageFixed,
					 P1(Bare({0x53, 0x46, 0x0B})), P1Null(), "2024-01-15"});
	// UNIQUEIDENTIFIER: the first three groups travel little-endian.
	cases.push_back(
		{"GUID / P1StageFixed", Meta(TDS_TYPE_UNIQUEIDENTIFIER, 16), AppendArm::P1StageFixed,
		 P1(Bare({0x78, 0x56, 0x34, 0x12, 0xBC, 0x9A, 0xF0, 0xDE, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88})),
		 P1Null(), "12345678-9abc-def0-1122-334455667788"});
	// DECIMAL(18,2): a sign byte (1 = positive) plus an 8-byte LE mantissa.
	cases.push_back({"DECIMAL / P1StageDecimal", Meta(TDS_TYPE_DECIMAL, 9, 18, 2), AppendArm::P1StageDecimal,
					 P1(Bare({1, 0x39, 0x30, 0, 0, 0, 0, 0, 0})), P1Null(), "123.45"});

	cases.push_back({"NVARCHAR(10) / P2StageString", Meta(TDS_TYPE_NVARCHAR, 20), AppendArm::P2StageString,
					 P2(Utf16("Ab")), P2Null(), "Ab"});
	cases.push_back({"VARBINARY(8) / P2StageBinary", Meta(TDS_TYPE_BIGVARBINARY, 8), AppendArm::P2StageBinary,
					 P2(Ascii("abc")), P2Null(), "abc"});
	// A CHAR/VARCHAR column stages through the same binary arm: the bytes go into
	// the VARCHAR vector as they are (collation lives in the scan's SELECT list).
	cases.push_back({"VARCHAR(8) / P2StageBinary", Meta(TDS_TYPE_BIGVARCHAR, 8), AppendArm::P2StageBinary,
					 P2(Ascii("hi")), P2Null(), "hi"});

	// PLP, deliberately in more than one chunk — a single chunk would never
	// exercise the assembly loop.
	std::vector<Wire> nchunks;
	nchunks.push_back(Utf16("He"));
	nchunks.push_back(Utf16("llo"));
	cases.push_back({"NVARCHAR(MAX) / PlpStageString", Meta(TDS_TYPE_NVARCHAR, 0xFFFF), AppendArm::PlpStageString,
					 Plp(nchunks), PlpNull(), "Hello"});
	std::vector<Wire> bchunks;
	bchunks.push_back(Ascii("xy"));
	bchunks.push_back(Ascii("z"));
	cases.push_back({"VARBINARY(MAX) / PlpStageBinary", Meta(TDS_TYPE_BIGVARBINARY, 0xFFFF), AppendArm::PlpStageBinary,
					 Plp(bchunks), PlpNull(), "xyz"});

	cases.push_back({"NTEXT / LobStageString", Meta(TDS_TYPE_NTEXT, 0xFFFF), AppendArm::LobStageString,
					 Lob(Utf16("Ok")), LobNull(), "Ok"});
	cases.push_back({"TEXT / LobStageBinary", Meta(TDS_TYPE_TEXT, 0xFFFF), AppendArm::LobStageBinary, Lob(Ascii("txt")),
					 LobNull(), "txt"});
	return cases;
}

//===--------------------------------------------------------------------===//
// A configured stager over a set of columns, with output vectors it owns.
//===--------------------------------------------------------------------===//

class Fixture {
public:
	//! `skipped` columns get a null target, which is how the stream marks a
	//! column it parses for its length and discards.
	void Add(const ColumnMetadata &meta, bool skipped = false) {
		metadata_.push_back(meta);
		skipped_.push_back(skipped);
	}

	void Configure() {
		// What MSSQL_DEBUG>=2 does for a real stream. `nbc_rows` lives behind this
		// gate like every other counter — the walks must not carry an add that only
		// a test reads — and a test that owns its stager can simply turn it on.
		// Enabling them also puts the counter-folding code itself under test.
		stager_.EnableCounters();
		for (size_t i = 0; i < metadata_.size(); i++) {
			if (skipped_[i]) {
				owned_.push_back(nullptr);
				targets_.push_back(nullptr);
				continue;
			}
			owned_.push_back(std::unique_ptr<Vector>(new Vector(TypeConverter::GetDuckDBType(metadata_[i]))));
			targets_.push_back(owned_.back().get());
		}
		stager_.Configure(metadata_, targets_);
	}

	void BeginChunk() {
		stager_.BeginChunk(targets_);
	}
	size_t StageRow(const Wire &row, idx_t row_idx) {
		return stager_.StageRow(row.data(), row.size(), row_idx);
	}
	size_t StageNBCRow(const Wire &row, idx_t row_idx) {
		return stager_.StageNBCRow(row.data(), row.size(), row_idx);
	}
	void FinalizeChunk(idx_t rows) {
		stager_.FinalizeChunk(rows);
	}

	//! What column `c` holds at `row`, rendered — "NULL" for a NULL, so a wrong
	//! NULL and a wrong value read the same way in the failure output.
	std::string ValueAt(size_t c, idx_t row) {
		const duckdb::Value v = owned_[c]->GetValue(row);
		return v.IsNull() ? std::string("NULL") : v.ToString();
	}

	bool IsNull(size_t c, idx_t row) {
		return owned_[c]->GetValue(row).IsNull();
	}

	RowStager &stager() {
		return stager_;
	}

private:
	std::vector<ColumnMetadata> metadata_;
	std::vector<bool> skipped_;
	std::vector<std::unique_ptr<Vector>> owned_;
	std::vector<Vector *> targets_;
	RowStager stager_;
};

//===--------------------------------------------------------------------===//
// Tests
//===--------------------------------------------------------------------===//

void TestBitmapConvention() {
	std::cout << "[1] NBC bitmap: LSB-first, bit SET means NULL..." << std::endl;

	// Asserted directly, once: a builder with the convention inverted would
	// produce tests that pass while exercising the opposite case throughout.
	std::vector<bool> nulls(9, false);
	nulls[0] = true;
	nulls[8] = true;
	const Wire bitmap = NullBitmap(nulls);
	CHECK_EQ(bitmap.size(), static_cast<size_t>(2), "nine columns need two bitmap bytes");
	CHECK_EQ(static_cast<int>(bitmap[0]), 0x01, "column 0 is the LOW bit of byte 0");
	CHECK_EQ(static_cast<int>(bitmap[1]), 0x01, "column 8 is the LOW bit of byte 1");

	// And the walk agrees: nine INT columns, the first and the last NULL. If the
	// convention were inverted the seven present columns would be read from the
	// wrong offsets, so this also fails loudly rather than subtly.
	Fixture f;
	for (int i = 0; i < 9; i++) {
		f.Add(Meta(duckdb::tds::TDS_TYPE_INT, 4));
	}
	f.Configure();
	f.BeginChunk();

	Wire row = bitmap;
	for (int i = 1; i < 8; i++) {
		row = Cat(row, Bare({i * 10, 0, 0, 0}));
	}
	const size_t consumed = f.StageNBCRow(row, 0);
	f.FinalizeChunk(1);

	CHECK_EQ(consumed, row.size(), "the walk consumed exactly the row");
	CHECK_TRUE(f.IsNull(0, 0), "column 0 NULL");
	CHECK_TRUE(f.IsNull(8, 0), "column 8 NULL");
	for (int i = 1; i < 8; i++) {
		CHECK_EQ(f.ValueAt(i, 0), std::to_string(i * 10), "present column decodes at its own offset");
	}
}

void TestEveryArmPresent() {
	std::cout << "[2] every append arm, value present in an NBC row..." << std::endl;
	const std::vector<ArmCase> cases = AllArms();

	Fixture f;
	for (const ArmCase &c : cases) {
		f.Add(c.meta);
	}
	// One column the query does not project: it must still be walked, or every
	// column after it decodes from the wrong offset.
	f.Add(Meta(duckdb::tds::TDS_TYPE_INT, 4), /*skipped=*/true);
	f.Configure();
	f.BeginChunk();

	// Two NULLs, so this is a row a real server would actually send as NBCROW —
	// and the bitmap spans three bytes with the columns under test either side.
	std::vector<bool> nulls(cases.size() + 1, false);
	Wire row = NullBitmap(nulls);
	for (const ArmCase &c : cases) {
		row = Cat(row, c.value);
	}
	row = Cat(row, Bare({99, 0, 0, 0}));  // the skipped column's value

	const size_t consumed = f.StageNBCRow(row, 0);
	f.FinalizeChunk(1);
	CHECK_EQ(consumed, row.size(), "the walk consumed exactly the row");

	for (size_t i = 0; i < cases.size(); i++) {
		CHECK_EQ(f.ValueAt(i, 0), std::string(cases[i].expected), cases[i].label);
	}
	CHECK_TRUE(f.stager().IsSkipped(cases.size()), "the unprojected column resolved to the Skip arm");
	CHECK_EQ(f.stager().Counters().nbc_rows, static_cast<uint64_t>(1), "one NBC row counted");
}

void TestEveryArmNulledByTheBitmap() {
	std::cout << "[3] every append arm, NULLed by the bitmap (zero bytes consumed)..." << std::endl;
	const std::vector<ArmCase> cases = AllArms();

	// One column at a time is NULLed while every other column carries its value.
	// The other columns ARE the assertion: if the NULLed arm consumed any bytes,
	// everything after it decodes from the wrong offset. A row length that adds
	// up would not catch that on its own.
	for (size_t nulled = 0; nulled < cases.size(); nulled++) {
		Fixture f;
		for (const ArmCase &c : cases) {
			f.Add(c.meta);
		}
		f.Configure();
		f.BeginChunk();

		std::vector<bool> nulls(cases.size(), false);
		nulls[nulled] = true;
		Wire row = NullBitmap(nulls);
		for (size_t i = 0; i < cases.size(); i++) {
			if (i != nulled) {
				row = Cat(row, cases[i].value);
			}
		}

		const size_t consumed = f.StageNBCRow(row, 0);
		f.FinalizeChunk(1);
		CHECK_EQ(consumed, row.size(), "a bitmap NULL consumes no value bytes");
		CHECK_TRUE(f.IsNull(nulled, 0), cases[nulled].label);
		for (size_t i = 0; i < cases.size(); i++) {
			if (i != nulled) {
				CHECK_EQ(f.ValueAt(i, 0), std::string(cases[i].expected), cases[i].label);
			}
		}
	}

	// All of them at once: the row is its bitmap and nothing else.
	Fixture f;
	for (const ArmCase &c : cases) {
		f.Add(c.meta);
	}
	f.Configure();
	f.BeginChunk();
	const Wire row = NullBitmap(std::vector<bool>(cases.size(), true));
	const size_t consumed = f.StageNBCRow(row, 0);
	f.FinalizeChunk(1);
	CHECK_EQ(consumed, row.size(), "an all-NULL row is bitmap-only");
	for (size_t i = 0; i < cases.size(); i++) {
		CHECK_TRUE(f.IsNull(i, 0), "all-NULL row: every column NULL");
	}
}

void TestNullPrefixOnAPresentColumn() {
	std::cout << "[4] 0xFFFF prefix on a column the bitmap calls present (PR #213)..." << std::endl;

	// SkipValueNBC accepts this and returns 2, so the row passes the parser's
	// length check; the walk used to take 0xFFFF as a LENGTH and read 65535 bytes
	// from past the row. The string arm survived only because 0xFFFF is odd and
	// the UTF-16 parity check caught it by luck — so both arms are tested, and
	// the message is checked to pin WHICH guard fired.
	const uint8_t p2_types[] = {duckdb::tds::TDS_TYPE_NVARCHAR, duckdb::tds::TDS_TYPE_BIGVARBINARY};
	for (uint8_t type_id : p2_types) {
		Fixture f;
		f.Add(Meta(type_id, 8));
		f.Add(Meta(duckdb::tds::TDS_TYPE_INT, 4));	// sentinel, never reached
		f.Configure();
		f.BeginChunk();

		Wire row = NullBitmap(std::vector<bool>(2, false));
		row = Cat(row, Bare({0xFF, 0xFF}));
		row = Cat(row, Bare({1, 0, 0, 0}));

		std::string message;
		try {
			f.StageNBCRow(row, 0);
		} catch (const duckdb::InvalidInputException &e) {
			message = e.what();
		}
		CHECK_TRUE(message.find("null bitmap") != std::string::npos,
				   "a 0xFFFF prefix inside an NBC row is rejected as malformed, not read as a length");
	}
}

void TestRowAndNbcRowAgree() {
	std::cout << "[5] ROW and NBCROW forms of the same logical row agree..." << std::endl;
	// Only the columns with a NULL form in a plain ROW take part: a bare INT
	// column has none — TDS sends a nullable integer as INTN — while the bitmap
	// can null anything. That asymmetry is why the two walks exist.
	std::vector<ArmCase> cases;
	for (const ArmCase &c : AllArms()) {
		if (!c.null_wire.empty()) {
			cases.push_back(c);
		}
	}
	CHECK_TRUE(cases.size() >= 8, "at least eight nullable columns — fewer and a server sends a plain ROW");

	// Every third column NULL, so both forms carry NULLs and values throughout.
	std::vector<bool> nulls(cases.size(), false);
	for (size_t i = 0; i < cases.size(); i += 3) {
		nulls[i] = true;
	}

	Fixture plain;
	Fixture nbc;
	for (const ArmCase &c : cases) {
		plain.Add(c.meta);
		nbc.Add(c.meta);
	}
	plain.Configure();
	nbc.Configure();
	plain.BeginChunk();
	nbc.BeginChunk();

	Wire plain_row;
	Wire nbc_row = NullBitmap(nulls);
	for (size_t i = 0; i < cases.size(); i++) {
		plain_row = Cat(plain_row, nulls[i] ? cases[i].null_wire : cases[i].value);
		if (!nulls[i]) {
			nbc_row = Cat(nbc_row, cases[i].value);
		}
	}

	CHECK_EQ(plain.StageRow(plain_row, 0), plain_row.size(), "the ROW walk consumed exactly the row");
	CHECK_EQ(nbc.StageNBCRow(nbc_row, 0), nbc_row.size(), "the NBC walk consumed exactly the row");
	plain.FinalizeChunk(1);
	nbc.FinalizeChunk(1);

	for (size_t i = 0; i < cases.size(); i++) {
		CHECK_EQ(nbc.ValueAt(i, 0), plain.ValueAt(i, 0), cases[i].label);
		if (nulls[i]) {
			CHECK_TRUE(nbc.IsNull(i, 0), "the bitmap NULL is a NULL");
		}
	}
	CHECK_EQ(plain.stager().Counters().nbc_rows, static_cast<uint64_t>(0), "a plain ROW is not counted as NBC");
	CHECK_EQ(nbc.stager().Counters().nbc_rows, static_cast<uint64_t>(1), "the NBC row is counted");
}

void TestNbcRowCounter() {
	std::cout << "[6] nbc_rows counts rows across chunks..." << std::endl;
	Fixture f;
	f.Add(Meta(duckdb::tds::TDS_TYPE_INT, 4));
	f.Configure();

	const Wire nbc_row = Cat(NullBitmap(std::vector<bool>(1, false)), Bare({5, 0, 0, 0}));
	const Wire plain_row = Bare({5, 0, 0, 0});
	for (int chunk = 0; chunk < 3; chunk++) {
		f.BeginChunk();
		f.StageNBCRow(nbc_row, 0);
		f.StageRow(plain_row, 1);
		f.StageNBCRow(nbc_row, 2);
		f.FinalizeChunk(3);
	}
	CHECK_EQ(f.stager().Counters().nbc_rows, static_cast<uint64_t>(6), "two NBC rows per chunk, three chunks");
}

}  // namespace

int main() {
	std::cout << "=== RowStager unit tests (spec 059 D1b) ===" << std::endl;
	TestBitmapConvention();
	TestEveryArmPresent();
	TestEveryArmNulledByTheBitmap();
	TestNullPrefixOnAPresentColumn();
	TestRowAndNbcRowAgree();
	TestNbcRowCounter();

	if (failures == 0) {
		std::cout << "\nAll RowStager tests passed." << std::endl;
		return 0;
	}
	std::cerr << "\n" << failures << " check(s) failed." << std::endl;
	return 1;
}
