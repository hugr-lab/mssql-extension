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
//     MSSQL_DEBUG>=2 does, so nothing in the walks exists solely for a test;
//   - D2: every framing shape (bare / P1 / P2 / PLP / LOB, both row forms)
//     consumed byte-for-byte identically by the walk and by
//     RowReader::SkipValue — the two independent switches whose agreement is
//     the walk's entire memory-safety argument.
//
// Build & run:
//   make test-row-stager
//
// Built WITHOUT -DNDEBUG on purpose: `StageRow`/`StageNBCRow` end with
// `D_ASSERT(p == end)`, and that assertion is the framing check — it fires when
// a walk consumes a different number of bytes than the row holds.

#include "codec/staging/row_stager.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "tds/encoding/type_converter.hpp"
#include "tds/tds_column_metadata.hpp"
#include "tds/tds_types.hpp"

#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using duckdb::idx_t;
using duckdb::LogicalType;
using duckdb::LogicalTypeId;
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

//! PLP whose total length is declared UNKNOWN — what a server streaming a MAX
//! value actually sends. Only the terminator ends the value.
Wire PlpUnknown(const std::vector<Wire> &chunks) {
	Wire out = Plp(chunks);
	const uint64_t unknown = 0xFFFFFFFFFFFFFFFEULL;
	std::memcpy(out.data(), &unknown, 8);
	return out;
}

//! Filler for the framing matrix, where only the byte COUNT is under test.
//! Zeros rather than a pattern: every finalize kernel still runs over the staged
//! value, and zero is in range for all of them (day 0, midnight, decimal 0).
Wire Zeros(size_t n) {
	return Wire(n, 0);
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

//! Explicit UTF-16LE code units, for the values ASCII cannot express.
Wire Units(const std::vector<uint16_t> &units) {
	Wire out;
	for (uint16_t u : units) {
		out.push_back(static_cast<uint8_t>(u & 0xFF));
		out.push_back(static_cast<uint8_t>(u >> 8));
	}
	return out;
}

Wire RepeatUnit(uint16_t unit, size_t n) {
	return Units(std::vector<uint16_t>(n, unit));
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
	// The negative case is not decoration: its high word is -1, and assembling the
	// 64-bit value by shifting that left was undefined behaviour until spec 059's
	// fuzz run caught it — on ordinary data, not a hostile stream.
	cases.push_back({"MONEY / RawStageFixed", Meta(TDS_TYPE_MONEY, 8), AppendArm::RawStageFixed,
					 Bare({0, 0, 0, 0, 0x40, 0xE2, 0x01, 0x00}), Wire(), "12.3456"});
	cases.push_back({"MONEY negative / RawStageFixed", Meta(TDS_TYPE_MONEY, 8), AppendArm::RawStageFixed,
					 Bare({0xFF, 0xFF, 0xFF, 0xFF, 0xC0, 0x1D, 0xFE, 0xFF}), Wire(), "-12.3456"});
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
		targets_types_.push_back(LogicalType(LogicalTypeId::INVALID));
	}

	//! A column whose OUTPUT type is not the one the wire implies — a view with an
	//! inline CAST against a stale catalog (issue #89).
	void AddDiverging(const ColumnMetadata &meta, const LogicalType &target_type) {
		metadata_.push_back(meta);
		skipped_.push_back(false);
		targets_types_.push_back(target_type);
	}

	void Configure() {
		// What MSSQL_DEBUG>=2 does for a real stream. `nbc_rows` lives behind this
		// gate like every other counter — the walks must not carry an add that only
		// a test reads — and a test that owns its stager can simply turn it on.
		// Enabling them also puts the counter-folding code itself under test.
		stager_.EnableCounters();
		// A real DataChunk, not loose vectors: the stream fills one of these and
		// calls Reset() on it between chunks, which is what restores FLAT vectors
		// and clean validity after the constant path (spec 056) leaves one
		// CONSTANT. A fixture that reset nothing would test a state production
		// never sees — and it did, until the BeginChunk assertion said so.
		duckdb::vector<LogicalType> types;
		for (size_t i = 0; i < metadata_.size(); i++) {
			types.push_back(targets_types_[i].id() == LogicalTypeId::INVALID
								? TypeConverter::GetDuckDBType(metadata_[i])
								: targets_types_[i]);
		}
		chunk_.Initialize(duckdb::Allocator::DefaultAllocator(), types);
		for (size_t i = 0; i < metadata_.size(); i++) {
			targets_.push_back(skipped_[i] ? nullptr : &chunk_.data[i]);
		}
		stager_.Configure(metadata_, targets_);
	}

	void BeginChunk() {
		chunk_.Reset();
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
		const duckdb::Value v = chunk_.data[c].GetValue(row);
		return v.IsNull() ? std::string("NULL") : v.ToString();
	}

	//! CONSTANT or FLAT — the spec-056 emission is asserted, not assumed.
	bool IsConstant(size_t c) {
		return chunk_.data[c].GetVectorType() == duckdb::VectorType::CONSTANT_VECTOR;
	}

	bool IsNull(size_t c, idx_t row) {
		return chunk_.data[c].GetValue(row).IsNull();
	}

	RowStager &stager() {
		return stager_;
	}

	//! The metadata the stager was configured with — so a test can build a SECOND,
	//! independent RowReader over the same columns and compare framing (D2).
	const std::vector<ColumnMetadata> &metadata() const {
		return metadata_;
	}

private:
	std::vector<ColumnMetadata> metadata_;
	std::vector<bool> skipped_;
	//! INVALID means "whatever the wire type implies" — the ordinary case.
	std::vector<LogicalType> targets_types_;
	duckdb::DataChunk chunk_;
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

//===--------------------------------------------------------------------===//
// D2 — the differential framing test
//
// The staged walk carries NO bounds test per value. Its whole safety argument is
// that the parser hands up a row whose length `RowReader::SkipValue` established,
// and that the walk then consumes exactly those bytes. Those are two independent
// switch statements over the same wire types in two different files, with
// nothing tying them together — and that gap is precisely how the NBC 0xFFFF
// defect got in: SkipValueNBC returned 2, the walk consumed 65537.
//
// So: every framing shape, through both, byte for byte. A sentinel column
// follows the column under test, because agreeing on a byte COUNT and consuming
// the RIGHT bytes are different claims and only the sentinel tests the second.
//===--------------------------------------------------------------------===//

struct FramingCase {
	const char *label;
	ColumnMetadata meta;
	Wire value;
	//! The type's NULL form in a plain ROW; empty for the bare fixed types, which
	//! have none — TDS sends a nullable one as its *N variant instead.
	Wire null_wire;
};

//! Follows every column under test. Bare INT: the shortest framing there is, so
//! a preceding over-read lands in it rather than past the row, and its value is
//! a pattern no length prefix or filler byte could produce.
ColumnMetadata SentinelMeta() {
	return Meta(duckdb::tds::TDS_TYPE_INT, 4);
}
const size_t SENTINEL_BYTES = 4;
const char *const SENTINEL_TEXT = "1515870810";	 // 0x5A5A5A5A

Wire SentinelWire() {
	return Bare({0x5A, 0x5A, 0x5A, 0x5A});
}

std::vector<FramingCase> FramingMatrix() {
	using namespace duckdb::tds;
	std::vector<FramingCase> m;

	// Bare: no prefix at all, width implied by the type.
	m.push_back({"bare TINYINT", Meta(TDS_TYPE_TINYINT, 1), Zeros(1), Wire()});
	m.push_back({"bare BIT", Meta(TDS_TYPE_BIT, 1), Zeros(1), Wire()});
	m.push_back({"bare SMALLINT", Meta(TDS_TYPE_SMALLINT, 2), Zeros(2), Wire()});
	m.push_back({"bare INT", Meta(TDS_TYPE_INT, 4), Zeros(4), Wire()});
	m.push_back({"bare BIGINT", Meta(TDS_TYPE_BIGINT, 8), Zeros(8), Wire()});
	m.push_back({"bare REAL", Meta(TDS_TYPE_REAL, 4), Zeros(4), Wire()});
	m.push_back({"bare FLOAT", Meta(TDS_TYPE_FLOAT, 8), Zeros(8), Wire()});
	m.push_back({"bare MONEY", Meta(TDS_TYPE_MONEY, 8), Zeros(8), Wire()});
	m.push_back({"bare SMALLMONEY", Meta(TDS_TYPE_SMALLMONEY, 4), Zeros(4), Wire()});
	m.push_back({"bare DATETIME", Meta(TDS_TYPE_DATETIME, 8), Zeros(8), Wire()});
	m.push_back({"bare SMALLDATETIME", Meta(TDS_TYPE_SMALLDATETIME, 4), Zeros(4), Wire()});

	// P1: one length byte, 0 meaning NULL. Every declared width of every *N
	// variant, because the width lives in the metadata and the two sides read it
	// from different places — the walk from a resolved stride, the parser from
	// the byte on the wire.
	m.push_back({"P1 INTN(1)", Meta(TDS_TYPE_INTN, 1), P1(Zeros(1)), P1Null()});
	m.push_back({"P1 INTN(2)", Meta(TDS_TYPE_INTN, 2), P1(Zeros(2)), P1Null()});
	m.push_back({"P1 INTN(4)", Meta(TDS_TYPE_INTN, 4), P1(Zeros(4)), P1Null()});
	m.push_back({"P1 INTN(8)", Meta(TDS_TYPE_INTN, 8), P1(Zeros(8)), P1Null()});
	m.push_back({"P1 BITN", Meta(TDS_TYPE_BITN, 1), P1(Zeros(1)), P1Null()});
	m.push_back({"P1 FLOATN(4)", Meta(TDS_TYPE_FLOATN, 4), P1(Zeros(4)), P1Null()});
	m.push_back({"P1 FLOATN(8)", Meta(TDS_TYPE_FLOATN, 8), P1(Zeros(8)), P1Null()});
	m.push_back({"P1 MONEYN(4)", Meta(TDS_TYPE_MONEYN, 4), P1(Zeros(4)), P1Null()});
	m.push_back({"P1 MONEYN(8)", Meta(TDS_TYPE_MONEYN, 8), P1(Zeros(8)), P1Null()});
	m.push_back({"P1 DATETIMEN(4)", Meta(TDS_TYPE_DATETIMEN, 4), P1(Zeros(4)), P1Null()});
	m.push_back({"P1 DATETIMEN(8)", Meta(TDS_TYPE_DATETIMEN, 8), P1(Zeros(8)), P1Null()});
	m.push_back({"P1 GUID", Meta(TDS_TYPE_UNIQUEIDENTIFIER, 16), P1(Zeros(16)), P1Null()});
	m.push_back({"P1 DATE", Meta(TDS_TYPE_DATE, 3), P1(Zeros(3)), P1Null()});

	// The scale-dependent temporal widths: 3 bytes of time up to scale 2, 4 up to
	// 4, 5 beyond — plus a 3-byte date, and 2 more for an offset. The walk derives
	// this from the scale; the parser reads the length byte. Every boundary of
	// that arithmetic is here.
	const uint8_t scales[] = {0, 2, 3, 4, 5, 7};
	for (uint8_t scale : scales) {
		const size_t time_len = scale <= 2 ? 3 : (scale <= 4 ? 4 : 5);
		ColumnMetadata t = Meta(TDS_TYPE_TIME, 0, 0, scale);
		m.push_back({"P1 TIME", t, P1(Zeros(time_len)), P1Null()});
		ColumnMetadata dt2 = Meta(TDS_TYPE_DATETIME2, 0, 0, scale);
		m.push_back({"P1 DATETIME2", dt2, P1(Zeros(3 + time_len)), P1Null()});
		ColumnMetadata dto = Meta(TDS_TYPE_DATETIMEOFFSET, 0, 0, scale);
		m.push_back({"P1 DATETIMEOFFSET", dto, P1(Zeros(5 + time_len)), P1Null()});
	}

	// DECIMAL: width from the declared precision, at each of its four steps.
	m.push_back({"P1 DECIMAL(9,2)", Meta(TDS_TYPE_DECIMAL, 5, 9, 2), P1(Zeros(5)), P1Null()});
	m.push_back({"P1 DECIMAL(19,4)", Meta(TDS_TYPE_DECIMAL, 9, 19, 4), P1(Zeros(9)), P1Null()});
	m.push_back({"P1 NUMERIC(28,0)", Meta(TDS_TYPE_NUMERIC, 13, 28, 0), P1(Zeros(13)), P1Null()});
	m.push_back({"P1 NUMERIC(38,10)", Meta(TDS_TYPE_NUMERIC, 17, 38, 10), P1(Zeros(17)), P1Null()});
	// The one fixed-width family whose value may legitimately arrive SHORTER than
	// its declared width: a server that trims trailing zero bytes of the mantissa.
	// The walk zero-extends and must still consume only what arrived.
	m.push_back({"P1 DECIMAL(19,4) trimmed", Meta(TDS_TYPE_DECIMAL, 9, 19, 4), P1(Zeros(5)), P1Null()});

	// P2: two length bytes, 0xFFFF meaning NULL.
	m.push_back({"P2 NVARCHAR", Meta(TDS_TYPE_NVARCHAR, 20), P2(Utf16("ab")), P2Null()});
	m.push_back({"P2 NVARCHAR empty", Meta(TDS_TYPE_NVARCHAR, 20), P2(Wire()), P2Null()});
	m.push_back({"P2 NCHAR", Meta(TDS_TYPE_NCHAR, 20), P2(Utf16("ab")), P2Null()});
	m.push_back({"P2 VARCHAR", Meta(TDS_TYPE_BIGVARCHAR, 8), P2(Ascii("abc")), P2Null()});
	m.push_back({"P2 CHAR", Meta(TDS_TYPE_BIGCHAR, 8), P2(Ascii("abc")), P2Null()});
	m.push_back({"P2 VARBINARY", Meta(TDS_TYPE_BIGVARBINARY, 8), P2(Zeros(3)), P2Null()});
	m.push_back({"P2 VARBINARY empty", Meta(TDS_TYPE_BIGVARBINARY, 8), P2(Wire()), P2Null()});
	m.push_back({"P2 BINARY", Meta(TDS_TYPE_BIGBINARY, 8), P2(Zeros(8)), P2Null()});

	// PLP: 8-byte total, chunk list, terminator. Multi-chunk and the UNKNOWN
	// total are what a streaming server actually sends.
	std::vector<Wire> nchunks;
	nchunks.push_back(Utf16("He"));
	nchunks.push_back(Utf16("llo"));
	std::vector<Wire> bchunks;
	bchunks.push_back(Ascii("xy"));
	bchunks.push_back(Ascii("z"));
	m.push_back({"PLP NVARCHAR(MAX)", Meta(TDS_TYPE_NVARCHAR, 0xFFFF), Plp(nchunks), PlpNull()});
	m.push_back({"PLP NVARCHAR(MAX) unknown-length", Meta(TDS_TYPE_NVARCHAR, 0xFFFF), PlpUnknown(nchunks), PlpNull()});
	m.push_back({"PLP NVARCHAR(MAX) empty", Meta(TDS_TYPE_NVARCHAR, 0xFFFF), Plp(std::vector<Wire>()), PlpNull()});
	m.push_back({"PLP XML", Meta(TDS_TYPE_XML, 0xFFFF), Plp(nchunks), PlpNull()});
	m.push_back({"PLP VARCHAR(MAX)", Meta(TDS_TYPE_BIGVARCHAR, 0xFFFF), Plp(bchunks), PlpNull()});
	m.push_back({"PLP VARBINARY(MAX)", Meta(TDS_TYPE_BIGVARBINARY, 0xFFFF), Plp(bchunks), PlpNull()});
	m.push_back(
		{"PLP VARBINARY(MAX) unknown-length", Meta(TDS_TYPE_BIGVARBINARY, 0xFFFF), PlpUnknown(bchunks), PlpNull()});

	// The legacy LOBs: the only framing on the wire with a text pointer.
	m.push_back({"LOB TEXT", Meta(TDS_TYPE_TEXT, 0xFFFF), Lob(Ascii("txt")), LobNull()});
	m.push_back({"LOB NTEXT", Meta(TDS_TYPE_NTEXT, 0xFFFF), Lob(Utf16("Ok")), LobNull()});
	m.push_back({"LOB IMAGE", Meta(TDS_TYPE_IMAGE, 0xFFFF), Lob(Zeros(5)), LobNull()});
	return m;
}

//! Does this case even resolve to a staging arm? A mis-specified fixture would
//! otherwise throw out of the walk and take the whole binary down with it.
bool Stageable(const FramingCase &c) {
	const duckdb::mssql::codec::staging::ColumnOps ops =
		duckdb::mssql::codec::staging::ResolveColumnOps(c.meta, TypeConverter::GetDuckDBType(c.meta));
	return ops.arm < AppendArm::Unsupported;
}

void TestFramingMatchesSkipValue() {
	std::cout << "[7] framing: the walk and RowReader::SkipValue agree, byte for byte..." << std::endl;
	const std::vector<FramingCase> matrix = FramingMatrix();

	for (const FramingCase &c : matrix) {
		CHECK_TRUE(Stageable(c), c.label);
		if (!Stageable(c)) {
			continue;
		}
		// Value present, then the same case with the type's ROW NULL form.
		for (int nulled = 0; nulled < 2; nulled++) {
			const Wire &under_test = nulled ? c.null_wire : c.value;
			if (nulled && under_test.empty()) {
				continue;  // bare types have no NULL form in a plain ROW
			}
			Fixture f;
			f.Add(c.meta);
			f.Add(SentinelMeta());
			f.Configure();
			f.BeginChunk();

			const Wire row = Cat(under_test, SentinelWire());
			// A second, independent RowReader over the same columns: this is the
			// implementation the parser bounds rows with, and the one the walk must
			// not diverge from.
			duckdb::tds::RowReader reference(f.metadata());
			const size_t skipped = reference.SkipValue(row.data(), row.size(), 0);
			CHECK_TRUE(skipped > 0, c.label);

			const size_t consumed = f.StageRow(row, 0);
			f.FinalizeChunk(1);

			CHECK_EQ(consumed, row.size(), c.label);
			CHECK_EQ(consumed - SENTINEL_BYTES, skipped, c.label);
			CHECK_EQ(f.ValueAt(1, 0), std::string(SENTINEL_TEXT), c.label);
			if (nulled) {
				CHECK_TRUE(f.IsNull(0, 0), c.label);
			}
		}
	}
}

void TestFramingMatchesSkipValueNBC() {
	std::cout << "[8] framing: the NBC walk and RowReader::SkipValueNBC agree..." << std::endl;
	const std::vector<FramingCase> matrix = FramingMatrix();
	const Wire bitmap = NullBitmap(std::vector<bool>(2, false));

	for (const FramingCase &c : matrix) {
		if (!Stageable(c)) {
			continue;  // already reported by the plain-ROW pass
		}
		Fixture f;
		f.Add(c.meta);
		f.Add(SentinelMeta());
		f.Configure();
		f.BeginChunk();

		const Wire row = Cat(Cat(bitmap, c.value), SentinelWire());
		duckdb::tds::RowReader reference(f.metadata());
		const size_t skipped = reference.SkipValueNBC(row.data() + bitmap.size(), row.size() - bitmap.size(), 0);
		CHECK_TRUE(skipped > 0, c.label);

		const size_t consumed = f.StageNBCRow(row, 0);
		f.FinalizeChunk(1);

		CHECK_EQ(consumed, row.size(), c.label);
		CHECK_EQ(consumed - bitmap.size() - SENTINEL_BYTES, skipped, c.label);
		CHECK_EQ(f.ValueAt(1, 0), std::string(SENTINEL_TEXT), c.label);
	}
}

void TestFramingNbcBitmapNull() {
	std::cout << "[8b] framing: the NBC walk with the column NULLed by the bitmap..." << std::endl;
	// The third row form, and the one neither pass above covers: the column is
	// present in the metadata but absent from the row entirely. SkipValueNBC is
	// never consulted for it — the bitmap answers instead — so the assertion is
	// that the column consumes NOTHING and the sentinel behind it still decodes.
	// (Suggested by @oluies' review of PR #213, whose own differential test
	// carries this variant.)
	const std::vector<FramingCase> matrix = FramingMatrix();
	std::vector<bool> nulls(2, false);
	nulls[0] = true;
	const Wire bitmap = NullBitmap(nulls);

	for (const FramingCase &c : matrix) {
		if (!Stageable(c)) {
			continue;
		}
		Fixture f;
		f.Add(c.meta);
		f.Add(SentinelMeta());
		f.Configure();
		f.BeginChunk();

		const Wire row = Cat(bitmap, SentinelWire());
		const size_t consumed = f.StageNBCRow(row, 0);
		f.FinalizeChunk(1);

		CHECK_EQ(consumed, row.size(), c.label);
		CHECK_TRUE(f.IsNull(0, 0), c.label);
		CHECK_EQ(f.ValueAt(1, 0), std::string(SENTINEL_TEXT), c.label);
	}
}

//===--------------------------------------------------------------------===//
// D5 — the string kernel's corners
//
// The batch decode converts a whole column in ONE simdutf call and then splits
// the result on the U+0000 staged after each value. Which split it uses is a
// property of the column, decided once, so a given fixture only ever exercises
// one branch — and the bench matrix that drove the design is single-script, so
// several of these branches had never run at all.
//===--------------------------------------------------------------------===//

typedef duckdb::mssql::codec::staging::BoundaryStrategy Boundary;

//! How the column just decoded found its value boundaries. Read from the D10
//! counters, which the fixture enables — so the branch a case exercises is
//! asserted rather than assumed from its data.
uint64_t BoundaryCount(Fixture &f, Boundary strategy) {
	return f.stager().Counters().boundary[static_cast<uint8_t>(strategy)];
}

//! One NVARCHAR/NCHAR column, one row per value, decoded.
void DecodeStrings(Fixture &f, const std::vector<Wire> &values) {
	f.Configure();
	f.BeginChunk();
	for (size_t i = 0; i < values.size(); i++) {
		const Wire row = P2(values[i]);
		f.StageRow(row, static_cast<idx_t>(i));
	}
	f.FinalizeChunk(static_cast<idx_t>(values.size()));
}

void TestStringBoundaryStrategies() {
	std::cout << "[9] string kernel: every boundary strategy, and the mixed-script trap..." << std::endl;

	// Pure ASCII: no boundary search at all — a value's output offset is its
	// staged offset halved, and the verdict is free (written == units).
	{
		Fixture f;
		f.Add(Meta(duckdb::tds::TDS_TYPE_NVARCHAR, 40));
		std::vector<Wire> values;
		values.push_back(Utf16("alpha"));
		values.push_back(Utf16(""));
		values.push_back(Utf16("beta"));
		DecodeStrings(f, values);
		CHECK_EQ(f.ValueAt(0, 0), std::string("alpha"), "ascii value 0");
		CHECK_EQ(f.ValueAt(0, 1), std::string(""), "ascii empty value");
		CHECK_EQ(f.ValueAt(0, 2), std::string("beta"), "ascii value 2");
		CHECK_EQ(BoundaryCount(f, Boundary::AsciiOffsets), static_cast<uint64_t>(1), "ASCII column takes the offsets");
	}

	// The mixed-script trap the kernel's comment names: a value whose UTF-8 length
	// falls STRICTLY between its unit count and twice it. Searching from the unit
	// count is correct; probing at 2u lands past this value's delimiter and merges
	// it with the next. Every bench fixture is single-script, so a 2u probe would
	// pass the entire matrix — this is the case that fails it.
	{
		Fixture f;
		f.Add(Meta(duckdb::tds::TDS_TYPE_NVARCHAR, 40));
		std::vector<Wire> values;
		values.push_back(Cat(Utf16("aaaa"), Units({0x00C4})));	// 5 units, 6 bytes
		values.push_back(Utf16("bbbbbb"));
		DecodeStrings(f, values);
		CHECK_EQ(f.ValueAt(0, 0), std::string("aaaa\xC3\x84"), "mixed-script value keeps its own boundary");
		CHECK_EQ(f.ValueAt(0, 1), std::string("bbbbbb"), "the next value is not swallowed");
		CHECK_EQ(BoundaryCount(f, Boundary::Sweep), static_cast<uint64_t>(1), "short runs take the word sweep");
	}

	// Long runs take memchr instead — a choice made once per column from the
	// average length of one value's output. 120 CJK units are 360 bytes, past the
	// 256-byte threshold where a call into memchr starts beating the word sweep.
	{
		Fixture f;
		// A bounded NVARCHAR, NOT 0xFFFF: that value means MAX, which is PLP-framed
		// and would not read these P2 rows at all.
		f.Add(Meta(duckdb::tds::TDS_TYPE_NVARCHAR, 400));
		std::vector<Wire> values;
		// Distinct on purpose: two identical values are a CONSTANT column-chunk
		// (spec 056) and never reach a boundary walk at all.
		values.push_back(RepeatUnit(0x4E00, 120));
		values.push_back(Cat(RepeatUnit(0x4E00, 119), Units({0x4E8C})));
		DecodeStrings(f, values);
		std::string expected;
		for (int i = 0; i < 120; i++) {
			expected += "\xE4\xB8\x80";
		}
		std::string expected2;
		for (int i = 0; i < 119; i++) {
			expected2 += "\xE4\xB8\x80";
		}
		expected2 += "\xE4\xBA\x8C";
		CHECK_EQ(f.ValueAt(0, 0), expected, "CJK value 0 at three bytes per unit");
		CHECK_EQ(f.ValueAt(0, 1), expected2, "CJK value 1");
		CHECK_EQ(BoundaryCount(f, Boundary::Memchr), static_cast<uint64_t>(1), "long runs take memchr");
	}
}

void TestStringEmbeddedNuls() {
	std::cout << "[10] string kernel: a value carrying its own U+0000..." << std::endl;

	// A U+0000 inside the data is legal in UCS-2 and produces a zero byte
	// indistinguishable from the separator staged after each value.
	//
	// The second case is the one that mattered: a value ENDING in U+0000. Its own
	// zero gets taken as its delimiter, and the next value's unit-count skip then
	// jumps over the real one — so the walk still consumes exactly one zero per
	// value and still lands exactly on the end of the output. The shipped check
	// was that landing position, so it saw nothing and two strings came out
	// silently wrong. Found here; the trigger is now a zero-byte count.
	{
		Fixture f;
		f.Add(Meta(duckdb::tds::TDS_TYPE_NVARCHAR, 40));
		std::vector<Wire> values;
		values.push_back(Units({0x00C4, 0x0000, 'b'}));	 // NUL in the middle
		values.push_back(Utf16("c"));
		DecodeStrings(f, values);
		CHECK_EQ(f.ValueAt(0, 0),
				 std::string("\xC3\x84\x00"
							 "b",
							 4),
				 "U+0000 in the middle of a value");
		CHECK_EQ(f.ValueAt(0, 1), std::string("c"), "the value after it");
	}
	{
		Fixture f;
		f.Add(Meta(duckdb::tds::TDS_TYPE_NVARCHAR, 40));
		std::vector<Wire> values;
		values.push_back(Units({'a', 'b', 0x00C4, 0x0000}));  // NUL LAST
		values.push_back(Utf16("c"));
		DecodeStrings(f, values);
		CHECK_EQ(f.ValueAt(0, 0), std::string("ab\xC3\x84\x00", 5), "a value ending in U+0000 keeps it");
		CHECK_EQ(f.ValueAt(0, 1), std::string("c"), "and does not steal the next value's delimiter");
		CHECK_EQ(BoundaryCount(f, Boundary::EmbeddedNul), static_cast<uint64_t>(1), "the re-split ran");
	}
}

void TestNcharTrim() {
	std::cout << "[11] string kernel: NCHAR trailing-space trim, valid and invalid input..." << std::endl;

	// NCHAR is blank-padded to its declared width. The trim moved to the OUTPUT
	// with spec 055 precisely so it stays correct when the payload is invalid
	// UTF-16 and goes through U+FFFD substitution — which is what this asserts.
	Fixture f;
	f.Add(Meta(duckdb::tds::TDS_TYPE_NCHAR, 40));
	std::vector<Wire> values;
	values.push_back(Utf16("ab    "));
	values.push_back(Units({0xD800, 'A', ' ', ' '}));  // unpaired surrogate, then padding
	values.push_back(Utf16("    "));
	DecodeStrings(f, values);
	CHECK_EQ(f.ValueAt(0, 0), std::string("ab"), "padding trimmed");
	CHECK_EQ(f.ValueAt(0, 1),
			 std::string("\xEF\xBF\xBD"
						 "A"),
			 "replacement char kept, padding still trimmed");
	CHECK_EQ(f.ValueAt(0, 2), std::string(""), "all-padding trims to empty");
	CHECK_EQ(f.stager().Counters().replaced_units, static_cast<uint64_t>(1), "one code unit replaced");
}

void TestDivergingColumnRendersAsText() {
	std::cout << "[12] issue #89: a column whose output type disagrees with the wire..." << std::endl;

	// A view with an inline CAST against a stale catalog: the wire says INT, the
	// vector is VARCHAR. The column must still stage by its WIRE framing and
	// render each value as text — not throw, which is what it did until PR #213,
	// and not write four raw bytes into a string_t.
	Fixture f;
	f.AddDiverging(Meta(duckdb::tds::TDS_TYPE_INT, 4), LogicalType::VARCHAR);
	f.Add(SentinelMeta());
	f.Configure();
	f.BeginChunk();

	const Wire row = Cat(Bare({42, 0, 0, 0}), SentinelWire());
	const size_t consumed = f.StageRow(row, 0);
	f.FinalizeChunk(1);

	CHECK_EQ(consumed, row.size(), "a diverging column is framed by its wire type");
	CHECK_EQ(f.ValueAt(0, 0), std::string("42"), "rendered as text");
	CHECK_EQ(f.ValueAt(1, 0), std::string(SENTINEL_TEXT), "the sentinel still decodes");
}

void TestDatetimeDayRange() {
	std::cout << "[13] a DATETIME day count no conforming server can send..." << std::endl;

	// Found by the spec-059 fuzz harness in seconds: DATETIME is the one temporal
	// type whose day field is a full signed 32-bit number, and the conversion
	// multiplies it by 86'400'000'000. Past ~1.07e8 days that overflowed int64 —
	// undefined behaviour, reachable by any server that sends the bytes.
	//
	// The conversion now assembles through an unsigned type, so such a value
	// wraps to a meaningless timestamp instead. Meaningless is the contract: the
	// bytes are outside the type's own range and there is no right answer. What
	// must hold is that the row still FRAMES correctly — 8 bytes consumed, the
	// column after it untouched — and that nothing traps.
	Fixture f;
	f.Add(Meta(duckdb::tds::TDS_TYPE_DATETIME, 8));
	f.Add(SentinelMeta());
	f.Configure();
	f.BeginChunk();

	Wire row = Bare({0x00, 0x00, 0x00, 0x80});	// days = INT32_MIN, the reduced fuzz case
	row = Cat(row, Bare({0, 0, 0, 0}));			// ticks
	row = Cat(row, SentinelWire());

	const size_t consumed = f.StageRow(row, 0);
	f.FinalizeChunk(1);
	CHECK_EQ(consumed, row.size(), "an out-of-range DATETIME is still framed as eight bytes");
	CHECK_EQ(f.ValueAt(1, 0), std::string(SENTINEL_TEXT), "and the column behind it is untouched");

	// The legal extremes still decode, which is what a range check would have
	// risked breaking. 1753-01-01 is -53690 days from 1900-01-01, 9999-12-31 is
	// 2958463.
	Fixture ok;
	ok.Add(Meta(duckdb::tds::TDS_TYPE_DATETIME, 8));
	ok.Configure();
	ok.BeginChunk();
	Wire low = Bare({0x46, 0x2E, 0xFF, 0xFF});	// -53690
	low = Cat(low, Bare({0, 0, 0, 0}));
	Wire high = Bare({0x7F, 0x24, 0x2D, 0x00});	 // 2958463
	high = Cat(high, Bare({0, 0, 0, 0}));
	ok.StageRow(low, 0);
	ok.StageRow(high, 1);
	ok.FinalizeChunk(2);
	CHECK_EQ(ok.ValueAt(0, 0), std::string("1753-01-01 00:00:00"), "the earliest legal DATETIME");
	CHECK_EQ(ok.ValueAt(0, 1), std::string("9999-12-31 00:00:00"), "the latest legal DATETIME");
}

void TestImpossibleDeclaredWidth() {
	std::cout << "[14] a fixed-width type declaring a width no server can send..." << std::endl;

	// The second fuzz finding. A DATETIMEN declaring 52 bytes is claimed by
	// neither the direct nor the staged-fixed branch, so it fell through to the
	// issue-#89 fallback — which took the width on trust and picked the arm from
	// a predicate that thought DATETIMEN was unprefixed. Result: the parser
	// consumed the value's one-byte NULL prefix, the walk copied SEVENTEEN bytes
	// (the staged-fixed switch's default), and the row's framing diverged. The
	// D_ASSERT caught it here; a release build would have read past the row.
	//
	// Every fixed-width type with an impossible width must now resolve to
	// Unsupported: there is no framing to stage it by, and nothing to rescue.
	const uint8_t fixed_types[] = {duckdb::tds::TDS_TYPE_INTN, duckdb::tds::TDS_TYPE_FLOATN,
								   duckdb::tds::TDS_TYPE_MONEYN, duckdb::tds::TDS_TYPE_DATETIMEN,
								   duckdb::tds::TDS_TYPE_BITN};
	const uint16_t impossible[] = {11, 12, 14, 15, 18, 52, 255};
	for (uint8_t type_id : fixed_types) {
		for (uint16_t width : impossible) {
			const ColumnMetadata meta = Meta(type_id, width);
			LogicalType target;
			try {
				target = TypeConverter::GetDuckDBType(meta);
			} catch (const std::exception &) {
				continue;  // the type converter rejects it first, which is also fine
			}
			const duckdb::mssql::codec::staging::ColumnOps ops =
				duckdb::mssql::codec::staging::ResolveColumnOps(meta, target);
			CHECK_TRUE(ops.arm == AppendArm::Unsupported, "an impossible declared width stages by nothing");
		}
	}

	// And the walk says so when a value arrives, rather than reading the row's
	// one byte as seventeen. This is the reduced fuzz input.
	Fixture f;
	f.Add(Meta(duckdb::tds::TDS_TYPE_DATETIMEN, 52));
	f.Configure();
	f.BeginChunk();
	const Wire row = Bare({0x00});	// a NULL DATETIMEN: one length byte, zero
	std::string message;
	try {
		f.StageRow(row, 0);
	} catch (const duckdb::InvalidInputException &e) {
		message = e.what();
	}
	CHECK_TRUE(message.find("cannot decode") != std::string::npos, "the column is named, not mis-framed");

	// The legal widths still stage, or the guard would have broken the type.
	for (uint16_t width : {4u, 8u}) {
		const ColumnMetadata meta = Meta(duckdb::tds::TDS_TYPE_DATETIMEN, static_cast<uint16_t>(width));
		const duckdb::mssql::codec::staging::ColumnOps ops =
			duckdb::mssql::codec::staging::ResolveColumnOps(meta, TypeConverter::GetDuckDBType(meta));
		CHECK_TRUE(ops.arm == AppendArm::P1StageFixed, "a legal DATETIMEN keeps its length-prefixed arm");
		CHECK_EQ(ops.stride, static_cast<uint32_t>(width), "and its declared width");
	}
}

void TestTemporalScaleIsBounded() {
	std::cout << "[15] a fractional-second scale outside 0..7..." << std::endl;

	// The fourth fuzz finding, and the only one caught in the metadata rather
	// than in a value. Everything downstream scales by ten to the power of this
	// byte — `Pow10(scale)` overflows int64 past 18, which is undefined
	// behaviour — so it is checked once, where it is parsed, rather than at each
	// use on the per-value path. MS-TDS caps TIME / DATETIME2 / DATETIMEOFFSET
	// at 7.
	//
	// COLMETADATA payload: column count, then UserType, Flags, the type byte, the
	// scale, and a zero-length column name.
	const uint8_t types[] = {duckdb::tds::TDS_TYPE_TIME, duckdb::tds::TDS_TYPE_DATETIME2,
							 duckdb::tds::TDS_TYPE_DATETIMEOFFSET};
	for (uint8_t type_id : types) {
		for (int scale = 0; scale <= 8; scale++) {
			Wire meta = Bare({0x01, 0x00});		   // one column
			meta = Cat(meta, Bare({0, 0, 0, 0}));  // UserType
			meta = Cat(meta, Bare({0x09, 0x00}));  // Flags: nullable
			meta = Cat(meta, Bare({type_id}));	   //
			meta = Cat(meta, Bare({scale}));	   // the byte under test
			meta = Cat(meta, Bare({0x00}));		   // empty column name

			std::vector<ColumnMetadata> columns;
			size_t consumed = 0;
			bool threw = false;
			try {
				duckdb::tds::ColumnMetadataParser::Parse(meta.data(), meta.size(), consumed, columns);
			} catch (const std::exception &) {
				threw = true;
			}
			if (scale <= 7) {
				CHECK_TRUE(!threw, "a legal scale parses");
				CHECK_EQ(static_cast<int>(columns.size()), 1, "a legal scale yields its column");
			} else {
				CHECK_TRUE(threw, "a scale past 7 is rejected where it is parsed");
			}
		}
	}
}

void TestConstantEmission() {
	std::cout << "[16] constant column-chunks (spec 056)..." << std::endl;

	// A uniform column-chunk is published as a CONSTANT vector: the decode
	// collapses from N values to one, so the win is on the scan itself and not
	// only downstream. Every arm is covered — Direct (already in the vector,
	// nothing decoded at all), Fixed and Var.
	const std::vector<ArmCase> cases = AllArms();
	for (size_t i = 0; i < cases.size(); i++) {
		// Direct columns are deliberately left flat: they have no decode to
		// collapse, so scanning them for uniformity buys only a downstream
		// constant and measured at +0.26 ns/value to do it.
		const bool collapses =
			!duckdb::mssql::codec::staging::ResolveColumnOps(cases[i].meta, TypeConverter::GetDuckDBType(cases[i].meta))
				 .direct_write;
		Fixture f;
		f.Add(cases[i].meta);
		f.Configure();
		f.BeginChunk();
		for (idx_t row = 0; row < 8; row++) {
			f.StageRow(cases[i].value, row);
		}
		f.FinalizeChunk(8);
		CHECK_EQ(f.IsConstant(0), collapses, cases[i].label);
		// Correctness is the point, not the vector type: every row must still read
		// back as the value, through the same accessor a consumer would use.
		for (idx_t row = 0; row < 8; row++) {
			CHECK_EQ(f.ValueAt(0, row), std::string(cases[i].expected), cases[i].label);
		}
	}

	// One differing row — and the LAST one, so a detector that stops early is
	// caught — must keep the column flat and decode every value.
	for (size_t i = 0; i < cases.size(); i++) {
		if (cases[i].null_wire.empty()) {
			continue;  // no NULL form to differ with in a plain ROW
		}
		Fixture f;
		f.Add(cases[i].meta);
		f.Configure();
		f.BeginChunk();
		for (idx_t row = 0; row < 7; row++) {
			f.StageRow(cases[i].value, row);
		}
		f.StageRow(cases[i].null_wire, 7);
		f.FinalizeChunk(8);
		CHECK_TRUE(!f.IsConstant(0), cases[i].label);
		CHECK_EQ(f.ValueAt(0, 0), std::string(cases[i].expected), cases[i].label);
		CHECK_EQ(f.ValueAt(0, 6), std::string(cases[i].expected), cases[i].label);
		CHECK_TRUE(f.IsNull(0, 7), cases[i].label);
	}
}

void TestConstantAllNullAndReuse() {
	std::cout << "[17] all-NULL chunks, and a vector reused across chunks..." << std::endl;

	// All NULL: detected by comparing two counters, no kernel runs, and the
	// vector is CONSTANT-NULL.
	Fixture f;
	f.Add(Meta(duckdb::tds::TDS_TYPE_NVARCHAR, 40));
	f.Add(Meta(duckdb::tds::TDS_TYPE_INTN, 4));
	f.Configure();
	f.BeginChunk();
	for (idx_t row = 0; row < 5; row++) {
		f.StageRow(Cat(P2Null(), P1Null()), row);
	}
	f.FinalizeChunk(5);
	CHECK_TRUE(f.IsConstant(0), "all-NULL string column is constant");
	CHECK_TRUE(f.IsConstant(1), "all-NULL integer column is constant");
	for (idx_t row = 0; row < 5; row++) {
		CHECK_TRUE(f.IsNull(0, row), "every row NULL");
		CHECK_TRUE(f.IsNull(1, row), "every row NULL");
	}

	// The same vectors, chunk after chunk: constant, then mixed, then all-NULL.
	// This is the reuse hazard — a CONSTANT vector carried into the next chunk
	// would hand the Direct arm a one-value view to write into, and stale NULL
	// bits would republish rows as NULL. DataChunk::Reset is what prevents both,
	// and the fixture goes through it exactly as the stream does.
	f.BeginChunk();
	for (idx_t row = 0; row < 4; row++) {
		f.StageRow(Cat(P2(Utf16("same")), P1(Bare({7, 0, 0, 0}))), row);
	}
	f.FinalizeChunk(4);
	CHECK_TRUE(f.IsConstant(0), "second chunk: uniform again");
	CHECK_TRUE(!f.IsConstant(1), "a uniform INTN is left flat — no decode to collapse");
	CHECK_EQ(f.ValueAt(0, 3), std::string("same"), "second chunk value");
	CHECK_EQ(f.ValueAt(1, 3), std::string("7"), "second chunk integer");

	f.BeginChunk();
	f.StageRow(Cat(P2(Utf16("a")), P1(Bare({1, 0, 0, 0}))), 0);
	f.StageRow(Cat(P2(Utf16("b")), P1(Bare({2, 0, 0, 0}))), 1);
	f.StageRow(Cat(P2Null(), P1Null()), 2);
	f.FinalizeChunk(3);
	CHECK_TRUE(!f.IsConstant(0), "third chunk: mixed values stay flat");
	CHECK_EQ(f.ValueAt(0, 0), std::string("a"), "flat after constant: row 0");
	CHECK_EQ(f.ValueAt(0, 1), std::string("b"), "flat after constant: row 1");
	CHECK_TRUE(f.IsNull(0, 2), "flat after constant: row 2 NULL");
	CHECK_EQ(f.ValueAt(1, 0), std::string("1"), "integer row 0");
	CHECK_EQ(f.ValueAt(1, 1), std::string("2"), "integer row 1");
	CHECK_TRUE(f.IsNull(1, 2), "integer row 2 NULL");

	// A single-row chunk stays flat: there is nothing to collapse.
	f.BeginChunk();
	f.StageRow(Cat(P2(Utf16("solo")), P1(Bare({9, 0, 0, 0}))), 0);
	f.FinalizeChunk(1);
	CHECK_TRUE(!f.IsConstant(0), "a one-row chunk is not worth collapsing");
	CHECK_EQ(f.ValueAt(0, 0), std::string("solo"), "one-row value");
}

}  // namespace

int main() {
	std::cout << "=== RowStager unit tests (spec 059 D1b + D2 + D5) ===" << std::endl;
	TestBitmapConvention();
	TestEveryArmPresent();
	TestEveryArmNulledByTheBitmap();
	TestNullPrefixOnAPresentColumn();
	TestRowAndNbcRowAgree();
	TestNbcRowCounter();
	TestFramingMatchesSkipValue();
	TestFramingMatchesSkipValueNBC();
	TestFramingNbcBitmapNull();
	TestStringBoundaryStrategies();
	TestStringEmbeddedNuls();
	TestNcharTrim();
	TestDivergingColumnRendersAsText();
	TestDatetimeDayRange();
	TestImpossibleDeclaredWidth();
	TestTemporalScaleIsBounded();
	TestConstantEmission();
	TestConstantAllNullAndReuse();

	if (failures == 0) {
		std::cout << "\nAll RowStager tests passed." << std::endl;
		return 0;
	}
	std::cerr << "\n" << failures << " check(s) failed." << std::endl;
	return 1;
}
