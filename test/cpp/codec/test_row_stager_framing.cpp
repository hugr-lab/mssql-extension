// test/cpp/codec/test_row_stager_framing.cpp
//
// Differential framing tests for the staged read path (spec 055 T5).
//
// WHY THIS EXISTS
// ---------------
// RowStager::StageRow / StageNBCRow walk a row with NO per-value bounds checks.
// That is safe only because the row was handed over by the token parser in
// raw-row mode, which returns a row solely once RowReader::SkipRow has proved
// its exact length is buffered (see row_stager.hpp's "WHY THE WALK NEEDS NO
// BOUNDS CHECKS").
//
// So the safety property is not "the stager frames correctly" — it is "the
// stager frames IDENTICALLY to RowReader". Those are two independent switch
// statements over the same wire types, in two different files
// (row_stager.cpp's StageRow vs tds_row_reader.cpp's SkipValue), and nothing
// else in the tree pins them together. Any divergence is a heap over-read, not
// a wrong value.
//
// HOW A DIVERGENCE IS DETECTED
// ----------------------------
// Every fixture is `<column under test> , <canary INT>`. A bare TDS INT is
// always exactly four bytes in both walks, so the canary's decoded value is a
// witness for the byte count the PRECEDING column consumed:
//
//   consumed too few  -> the canary reads shifted bytes -> wrong value
//   consumed too many -> the canary reads shifted bytes -> wrong value, AND
//                        the row buffer is over-read
//
// The row is additionally copied into an EXACT-SIZED heap allocation
// (new uint8_t[n], not a vector whose capacity may exceed its size), so an
// over-read past the last column is a heap-buffer-overflow that ASan reports
// even when the canary happens to survive. Build under ASan for that half of
// the coverage; see the `test-row-stager-framing` Makefile target.
//
// Each fixture is also run through RowReader::SkipRow first, asserting it
// consumes the whole row — that validates the fixture itself, so a bad test
// vector fails as a bad test vector rather than as a stager bug.
//
// Does NOT require a running SQL Server instance.
//
// Build & run:
//   make test-row-stager-framing

#include "codec/staging/row_stager.hpp"
#include "tds/encoding/type_converter.hpp"
#include "tds/tds_column_metadata.hpp"
#include "tds/tds_row_reader.hpp"
#include "tds/tds_types.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/vector.hpp"

#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using duckdb::FlatVector;
using duckdb::idx_t;
using duckdb::LogicalType;
using duckdb::LogicalTypeId;
using duckdb::string_t;
using duckdb::Vector;
using duckdb::mssql::codec::staging::RowStager;
using duckdb::tds::ColumnMetadata;
using duckdb::tds::RowReader;
using duckdb::tds::encoding::TypeConverter;

namespace tds = duckdb::tds;

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

//! The witness value. Asymmetric across all four bytes on purpose: a one-byte
//! framing slip has to change it.
const int32_t CANARY = static_cast<int32_t>(0x0BADC0DE);

ColumnMetadata MakeCol(const char *name, uint8_t type_id, uint16_t max_length, uint8_t precision = 0,
					   uint8_t scale = 0) {
	ColumnMetadata c;
	c.name = name;
	c.type_id = type_id;
	c.max_length = max_length;
	c.precision = precision;
	c.scale = scale;
	c.collation = 0;
	c.flags = tds::COL_FLAG_NULLABLE;
	return c;
}

ColumnMetadata CanaryCol() {
	return MakeCol("canary", tds::TDS_TYPE_INT, 4);
}

void AppendLe32(std::vector<uint8_t> &out, uint32_t v) {
	out.push_back(static_cast<uint8_t>(v & 0xFF));
	out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
	out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
	out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

//! UTF-16LE bytes for an ASCII string, as a non-PLP nvarchar payload would sit
//! on the wire.
std::vector<uint8_t> Utf16(const std::string &ascii) {
	std::vector<uint8_t> out;
	for (char ch : ascii) {
		out.push_back(static_cast<uint8_t>(ch));
		out.push_back(0);
	}
	return out;
}

//! A 2-byte-length-prefixed value (the non-PLP variable framing).
std::vector<uint8_t> P2(const std::vector<uint8_t> &payload) {
	std::vector<uint8_t> out;
	out.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
	out.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xFF));
	out.insert(out.end(), payload.begin(), payload.end());
	return out;
}

//! A 1-byte-length-prefixed value.
std::vector<uint8_t> P1(const std::vector<uint8_t> &payload) {
	std::vector<uint8_t> out;
	out.push_back(static_cast<uint8_t>(payload.size()));
	out.insert(out.end(), payload.begin(), payload.end());
	return out;
}

//! A PLP value as one chunk. `declare_total` picks between a declared total
//! length and the UNKNOWN marker — both are legal, and the stager assembles
//! them through different halves of AppendPlp.
std::vector<uint8_t> Plp(const std::vector<uint8_t> &payload, bool declare_total) {
	std::vector<uint8_t> out;
	if (declare_total) {
		uint64_t total = payload.size();
		for (int i = 0; i < 8; i++) {
			out.push_back(static_cast<uint8_t>((total >> (i * 8)) & 0xFF));
		}
	} else {
		// PLP_UNKNOWN_LENGTH = 0xFFFFFFFFFFFFFFFE, little-endian on the wire.
		out.push_back(0xFE);
		for (int i = 0; i < 7; i++) {
			out.push_back(0xFF);
		}
	}
	if (payload.empty()) {
		// An empty PLP value is the header plus a bare terminator — there is no
		// zero-length data chunk ahead of it. Emitting one would make the fixture
		// four bytes longer than SkipPLPType consumes, which the SkipRow
		// assertion in Run() would (correctly) report as a bad test vector.
		AppendLe32(out, 0);
		return out;
	}
	AppendLe32(out, static_cast<uint32_t>(payload.size()));
	out.insert(out.end(), payload.begin(), payload.end());
	AppendLe32(out, 0);	 // chunk terminator
	return out;
}

//! A PLP value split across two chunks — the shape AppendPlp's loop exists for.
std::vector<uint8_t> PlpTwoChunks(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
	std::vector<uint8_t> out;
	out.push_back(0xFE);
	for (int i = 0; i < 7; i++) {
		out.push_back(0xFF);
	}
	AppendLe32(out, static_cast<uint32_t>(a.size()));
	out.insert(out.end(), a.begin(), a.end());
	AppendLe32(out, static_cast<uint32_t>(b.size()));
	out.insert(out.end(), b.begin(), b.end());
	AppendLe32(out, 0);
	return out;
}

std::vector<uint8_t> PlpNull() {
	return std::vector<uint8_t>(8, 0xFF);
}

//! A legacy-LOB value: 1-byte text-pointer length, the pointer, an 8-byte row
//! timestamp, a 4-byte data length, then the data. Nothing else on the wire is
//! framed this way, which is exactly why it needs its own fixture.
std::vector<uint8_t> Lob(const std::vector<uint8_t> &payload) {
	std::vector<uint8_t> out;
	const uint8_t pointer_len = 16;
	out.push_back(pointer_len);
	for (uint8_t i = 0; i < pointer_len; i++) {
		out.push_back(static_cast<uint8_t>(0xA0 + i));
	}
	for (int i = 0; i < 8; i++) {
		out.push_back(static_cast<uint8_t>(0x10 + i));	// row timestamp
	}
	AppendLe32(out, static_cast<uint32_t>(payload.size()));
	out.insert(out.end(), payload.begin(), payload.end());
	return out;
}

std::vector<uint8_t> LobNull() {
	return std::vector<uint8_t>{0};	 // zero-length text pointer
}

//===--------------------------------------------------------------------===//
// Fixture
//===--------------------------------------------------------------------===//

struct Case {
	std::string label;
	ColumnMetadata col;
	//! Wire bytes for ONE value of `col`, in ROW (non-NBC) framing.
	std::vector<uint8_t> wire;
	//! Expect the column to decode as SQL NULL.
	bool expect_null = false;
	//! Optional value assertion on the column's output vector, row 0.
	std::function<void(Vector &)> check;
	//! Skip the NBCROW-with-value-present form. Only the 0xFFFF NULL sentinel
	//! needs this: an NBC row states NULL-ness in its bitmap, so a sentinel in
	//! the length prefix as well is malformed rather than a shape to round-trip.
	//! That combination is what test [2] is for.
	bool row_framing_only = false;
};

//! Run one fixture through both walks and compare their byte consumption.
//!
//! `nbc` builds an NBCROW instead: a NULL bitmap, then only the columns the
//! bitmap says are present. `null_via_bitmap` marks the column under test NULL
//! in the bitmap and omits its bytes entirely, which is the one thing an NBC row
//! does differently and the reason StageNBCRow is a separate function.
void Run(const Case &c, bool nbc, bool null_via_bitmap = false) {
	const std::string what =
		c.label + (nbc ? (null_via_bitmap ? " [NBCROW, NULL via bitmap]" : " [NBCROW]") : " [ROW]");

	std::vector<ColumnMetadata> meta;
	meta.push_back(c.col);
	meta.push_back(CanaryCol());

	std::vector<uint8_t> row;
	if (nbc) {
		// Two columns -> a one-byte bitmap. Bit 0 is the column under test.
		row.push_back(null_via_bitmap ? 0x01 : 0x00);
	}
	if (!null_via_bitmap) {
		row.insert(row.end(), c.wire.begin(), c.wire.end());
	}
	AppendLe32(row, static_cast<uint32_t>(CANARY));

	// 1. The reference framing. Also validates the fixture: if SkipRow does not
	//    consume exactly the bytes we built, the test vector is wrong and
	//    nothing below would mean anything.
	RowReader reader(meta);
	size_t consumed = 0;
	const bool bounded =
		nbc ? reader.SkipNBCRow(row.data(), row.size(), consumed) : reader.SkipRow(row.data(), row.size(), consumed);
	CHECK_TRUE(bounded, what + ": RowReader bounds the row");
	if (!bounded) {
		return;
	}
	CHECK_EQ(consumed, row.size(), what + ": RowReader consumes the whole row (fixture is well-formed)");

	// 2. The staged walk, over an EXACT-sized heap buffer so any read past the
	//    row is a heap-buffer-overflow under ASan rather than a silent read into
	//    a vector's spare capacity.
	std::unique_ptr<uint8_t[]> exact(new uint8_t[row.size()]);
	std::memcpy(exact.get(), row.data(), row.size());

	Vector under_test(TypeConverter::GetDuckDBType(c.col));
	Vector canary(LogicalType::INTEGER);
	std::vector<Vector *> targets;
	targets.push_back(&under_test);
	targets.push_back(&canary);

	RowStager stager;
	stager.Configure(meta, targets);
	stager.BeginChunk(targets);
	if (nbc) {
		stager.StageNBCRow(exact.get(), row.size(), 0);
	} else {
		stager.StageRow(exact.get(), row.size(), 0);
	}
	stager.FinalizeChunk(1);

	// 3. The witness. A framing slip of even one byte moves the canary.
	CHECK_EQ(FlatVector::GetData<int32_t>(canary)[0], CANARY, what + ": canary INT survives — framing agreed");
	CHECK_TRUE(FlatVector::Validity(canary).RowIsValid(0), what + ": canary is not NULL");

	// 4. Validity and (optionally) the decoded value of the column itself.
	const bool is_valid = FlatVector::Validity(under_test).RowIsValid(0);
	if (c.expect_null || null_via_bitmap) {
		CHECK_TRUE(!is_valid, what + ": value decodes as NULL");
	} else {
		CHECK_TRUE(is_valid, what + ": value decodes as non-NULL");
		if (is_valid && c.check) {
			c.check(under_test);
		}
	}
}

//! Run a fixture in ROW form, in NBCROW form with the value present, and in
//! NBCROW form with the value NULL via the bitmap. Every column type has to
//! survive all three; the shipped code has a separate switch for the second and
//! third, and only the first is exercised by the bench matrix.
void RunAllForms(const Case &c) {
	Run(c, /*nbc=*/false);
	if (!c.row_framing_only) {
		Run(c, /*nbc=*/true);
	}
	Run(c, /*nbc=*/true, /*null_via_bitmap=*/true);
}

//===--------------------------------------------------------------------===//
// Value assertions
//===--------------------------------------------------------------------===//

std::function<void(Vector &)> ExpectString(const std::string &expected) {
	return [expected](Vector &v) {
		const string_t got = FlatVector::GetData<string_t>(v)[0];
		const std::string actual(got.GetData(), got.GetSize());
		CHECK_EQ(actual, expected, "decoded string value");
	};
}

std::function<void(Vector &)> ExpectBlob(const std::vector<uint8_t> &expected) {
	return [expected](Vector &v) {
		const string_t got = FlatVector::GetData<string_t>(v)[0];
		CHECK_EQ(got.GetSize(), expected.size(), "decoded blob length");
		if (got.GetSize() == expected.size()) {
			CHECK_TRUE(std::memcmp(got.GetData(), expected.data(), expected.size()) == 0, "decoded blob bytes");
		}
	};
}

template <class T>
std::function<void(Vector &)> ExpectScalar(T expected, const char *label) {
	return [expected, label](Vector &v) { CHECK_TRUE(FlatVector::GetData<T>(v)[0] == expected, label); };
}

//===--------------------------------------------------------------------===//
// [1] Framing agreement across every staged wire form
//===--------------------------------------------------------------------===//

void TestFramingAgreesWithRowReader() {
	std::cout << "[1] framing: StageRow/StageNBCRow consume what RowReader bounds..." << std::endl;

	std::vector<Case> cases;

	// --- Bare fixed-width: no length prefix at all. ---
	cases.push_back(
		{"TINYINT", MakeCol("c", tds::TDS_TYPE_TINYINT, 1), {0x2A}, false, ExpectScalar<uint8_t>(42, "TINYINT value")});
	cases.push_back({"BIT", MakeCol("c", tds::TDS_TYPE_BIT, 1), {0x01}, false, ExpectScalar<bool>(true, "BIT value")});
	cases.push_back({"SMALLINT",
					 MakeCol("c", tds::TDS_TYPE_SMALLINT, 2),
					 {0x34, 0x12},
					 false,
					 ExpectScalar<int16_t>(0x1234, "SMALLINT value")});
	cases.push_back({"INT",
					 MakeCol("c", tds::TDS_TYPE_INT, 4),
					 {0x78, 0x56, 0x34, 0x12},
					 false,
					 ExpectScalar<int32_t>(0x12345678, "INT value")});
	cases.push_back({"BIGINT",
					 MakeCol("c", tds::TDS_TYPE_BIGINT, 8),
					 {0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01},
					 false,
					 ExpectScalar<int64_t>(0x0123456789ABCDEFLL, "BIGINT value")});
	cases.push_back({"REAL",
					 MakeCol("c", tds::TDS_TYPE_REAL, 4),
					 {0x00, 0x00, 0x80, 0x3F},
					 false,
					 ExpectScalar<float>(1.0f, "REAL value")});
	cases.push_back({"FLOAT",
					 MakeCol("c", tds::TDS_TYPE_FLOAT, 8),
					 {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F},
					 false,
					 ExpectScalar<double>(1.0, "FLOAT value")});
	// MONEY is 8 bytes, sent high-int32 first; SMALLMONEY 4. Both bare.
	cases.push_back({"MONEY", MakeCol("c", tds::TDS_TYPE_MONEY, 8), {0, 0, 0, 0, 0x10, 0x27, 0, 0}});
	cases.push_back({"SMALLMONEY", MakeCol("c", tds::TDS_TYPE_SMALLMONEY, 4), {0x10, 0x27, 0, 0}});
	cases.push_back({"DATETIME", MakeCol("c", tds::TDS_TYPE_DATETIME, 8), {0x00, 0x95, 0x00, 0x00, 0, 0, 0, 0}});
	cases.push_back({"SMALLDATETIME", MakeCol("c", tds::TDS_TYPE_SMALLDATETIME, 4), {0x00, 0x95, 0x10, 0x00}});

	// --- Nullable fixed-width variants: one length byte, 0 meaning NULL. ---
	cases.push_back({"INTN(1)", MakeCol("c", tds::TDS_TYPE_INTN, 1), P1({0x2A}), false,
					 ExpectScalar<uint8_t>(42, "INTN(1) value")});
	cases.push_back({"INTN(2)", MakeCol("c", tds::TDS_TYPE_INTN, 2), P1({0x34, 0x12})});
	cases.push_back({"INTN(4)", MakeCol("c", tds::TDS_TYPE_INTN, 4), P1({0x78, 0x56, 0x34, 0x12}), false,
					 ExpectScalar<int32_t>(0x12345678, "INTN(4) value")});
	cases.push_back({"INTN(8)", MakeCol("c", tds::TDS_TYPE_INTN, 8), P1({1, 0, 0, 0, 0, 0, 0, 0}), false,
					 ExpectScalar<int64_t>(1, "INTN(8) value")});
	// The NULL form of a prefixed column: a lone zero byte, no value bytes. The
	// prefixed arms test `len == STRIDE` first, so this is the other side of
	// that single compare.
	cases.push_back({"INTN(4) NULL", MakeCol("c", tds::TDS_TYPE_INTN, 4), {0x00}, true});
	cases.push_back(
		{"BITN", MakeCol("c", tds::TDS_TYPE_BITN, 1), P1({0x01}), false, ExpectScalar<bool>(true, "BITN value")});
	cases.push_back({"BITN NULL", MakeCol("c", tds::TDS_TYPE_BITN, 1), {0x00}, true});
	cases.push_back({"FLOATN(4)", MakeCol("c", tds::TDS_TYPE_FLOATN, 4), P1({0x00, 0x00, 0x80, 0x3F})});
	cases.push_back({"FLOATN(8)", MakeCol("c", tds::TDS_TYPE_FLOATN, 8), P1({0, 0, 0, 0, 0, 0, 0xF0, 0x3F})});
	cases.push_back({"MONEYN(8)", MakeCol("c", tds::TDS_TYPE_MONEYN, 8), P1({0, 0, 0, 0, 0x10, 0x27, 0, 0})});
	cases.push_back({"MONEYN(4)", MakeCol("c", tds::TDS_TYPE_MONEYN, 4), P1({0x10, 0x27, 0, 0})});
	cases.push_back({"MONEYN NULL", MakeCol("c", tds::TDS_TYPE_MONEYN, 8), {0x00}, true});
	cases.push_back({"DATETIMEN(8)", MakeCol("c", tds::TDS_TYPE_DATETIMEN, 8), P1({0x00, 0x95, 0, 0, 0, 0, 0, 0})});
	cases.push_back({"DATETIMEN(4)", MakeCol("c", tds::TDS_TYPE_DATETIMEN, 4), P1({0x00, 0x95, 0x10, 0x00})});

	// --- DECIMAL/NUMERIC: sign byte + little-endian mantissa, width from the
	//     declared precision. ---
	cases.push_back({"DECIMAL(9,2)", MakeCol("c", tds::TDS_TYPE_DECIMAL, 5, 9, 2), P1({1, 0x39, 0x30, 0x00, 0x00}),
					 false, ExpectScalar<int32_t>(12345, "DECIMAL(9,2) mantissa")});
	cases.push_back(
		{"NUMERIC(19,4)", MakeCol("c", tds::TDS_TYPE_NUMERIC, 9, 19, 4), P1({1, 0x39, 0x30, 0, 0, 0, 0, 0, 0})});
	cases.push_back({"DECIMAL(28,4)", MakeCol("c", tds::TDS_TYPE_DECIMAL, 13, 28, 4),
					 P1({1, 0x39, 0x30, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0})});
	cases.push_back({"DECIMAL(38,0)", MakeCol("c", tds::TDS_TYPE_DECIMAL, 17, 38, 0),
					 P1({1, 0x39, 0x30, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0})});
	// The one fixed-width family whose value may legitimately be SHORTER than
	// the declared width: a p38 column carrying a 4-byte mantissa. RowReader
	// frames it by the prefix; the stager must zero-extend and consume the same
	// 1 + len, not 1 + STRIDE. Divergence here reads 12 bytes into the next
	// column.
	cases.push_back({"DECIMAL(38,0) short mantissa", MakeCol("c", tds::TDS_TYPE_DECIMAL, 17, 38, 0),
					 P1({1, 0x39, 0x30, 0x00, 0x00})});
	cases.push_back({"DECIMAL NULL", MakeCol("c", tds::TDS_TYPE_DECIMAL, 5, 9, 2), {0x00}, true});

	// --- Temporal: fixed per column, width derived from scale. ---
	cases.push_back({"DATE", MakeCol("c", tds::TDS_TYPE_DATE, 3), P1({0x8F, 0x46, 0x0B})});
	cases.push_back({"TIME(0)", MakeCol("c", tds::TDS_TYPE_TIME, 3, 0, 0), P1({0x10, 0x0E, 0x00})});
	cases.push_back({"TIME(3)", MakeCol("c", tds::TDS_TYPE_TIME, 4, 0, 3), P1({0x10, 0x0E, 0x00, 0x00})});
	cases.push_back({"TIME(7)", MakeCol("c", tds::TDS_TYPE_TIME, 5, 0, 7), P1({0x10, 0x0E, 0x00, 0x00, 0x00})});
	cases.push_back(
		{"DATETIME2(0)", MakeCol("c", tds::TDS_TYPE_DATETIME2, 6, 0, 0), P1({0x10, 0x0E, 0x00, 0x8F, 0x46, 0x0B})});
	cases.push_back({"DATETIME2(7)", MakeCol("c", tds::TDS_TYPE_DATETIME2, 8, 0, 7),
					 P1({0x10, 0x0E, 0x00, 0x00, 0x00, 0x8F, 0x46, 0x0B})});
	cases.push_back({"DATETIMEOFFSET(0)", MakeCol("c", tds::TDS_TYPE_DATETIMEOFFSET, 8, 0, 0),
					 P1({0x10, 0x0E, 0x00, 0x8F, 0x46, 0x0B, 0x00, 0x00})});
	cases.push_back({"DATETIMEOFFSET(7)", MakeCol("c", tds::TDS_TYPE_DATETIMEOFFSET, 10, 0, 7),
					 P1({0x10, 0x0E, 0x00, 0x00, 0x00, 0x8F, 0x46, 0x0B, 0x3C, 0x00})});
	cases.push_back({"DATE NULL", MakeCol("c", tds::TDS_TYPE_DATE, 3), {0x00}, true});

	// --- UNIQUEIDENTIFIER: 16 bytes behind a one-byte prefix. ---
	std::vector<uint8_t> guid;
	for (uint8_t i = 0; i < 16; i++) {
		guid.push_back(static_cast<uint8_t>(0x11 * (i + 1)));
	}
	cases.push_back({"UNIQUEIDENTIFIER", MakeCol("c", tds::TDS_TYPE_UNIQUEIDENTIFIER, 16), P1(guid)});
	cases.push_back({"UNIQUEIDENTIFIER NULL", MakeCol("c", tds::TDS_TYPE_UNIQUEIDENTIFIER, 16), {0x00}, true});

	// --- Variable length, 2-byte prefix. ---
	cases.push_back(
		{"NVARCHAR(20)", MakeCol("c", tds::TDS_TYPE_NVARCHAR, 40), P2(Utf16("Hi")), false, ExpectString("Hi")});
	cases.push_back({"NVARCHAR(20) empty", MakeCol("c", tds::TDS_TYPE_NVARCHAR, 40), P2({}), false, ExpectString("")});
	// A U+00E9 makes the column non-ASCII, so the batch decode leaves the
	// arithmetic fast path and takes the delimiter walk instead.
	cases.push_back({"NVARCHAR(20) non-ASCII", MakeCol("c", tds::TDS_TYPE_NVARCHAR, 40), P2({0xE9, 0x00, 0x62, 0x00}),
					 false,
					 ExpectString("\xC3\xA9"
								  "b")});
	// NCHAR is space-padded by the server and the padding is trimmed on output.
	cases.push_back({"NCHAR(4)", MakeCol("c", tds::TDS_TYPE_NCHAR, 8), P2(Utf16("Hi  ")), false, ExpectString("Hi")});
	cases.push_back({"BIGVARCHAR(20)", MakeCol("c", tds::TDS_TYPE_BIGVARCHAR, 20), P2({'a', 'b', 'c'}), false,
					 ExpectString("abc")});
	cases.push_back({"BIGCHAR(5)", MakeCol("c", tds::TDS_TYPE_BIGCHAR, 5), P2({'a', 'b', ' ', ' ', ' '}), false,
					 ExpectString("ab")});
	cases.push_back({"BIGVARBINARY(16)", MakeCol("c", tds::TDS_TYPE_BIGVARBINARY, 16), P2({0xDE, 0xAD, 0xBE, 0xEF}),
					 false, ExpectBlob({0xDE, 0xAD, 0xBE, 0xEF})});
	cases.push_back({"BIGBINARY(4)", MakeCol("c", tds::TDS_TYPE_BIGBINARY, 4), P2({0x01, 0x02, 0x03, 0x04})});
	// The 0xFFFF NULL sentinel of the 2-byte framing. In a ROW this is the arm's
	// first test; StageNBCRow's regression is covered separately in [2].
	cases.push_back(
		{"NVARCHAR NULL sentinel", MakeCol("c", tds::TDS_TYPE_NVARCHAR, 40), {0xFF, 0xFF}, true, nullptr, true});
	cases.push_back(
		{"VARBINARY NULL sentinel", MakeCol("c", tds::TDS_TYPE_BIGVARBINARY, 16), {0xFF, 0xFF}, true, nullptr, true});

	// --- PLP / MAX: a chunk list, with the total length declared or UNKNOWN. ---
	cases.push_back({"NVARCHAR(MAX) declared", MakeCol("c", tds::TDS_TYPE_NVARCHAR, 0xFFFF), Plp(Utf16("Hi"), true),
					 false, ExpectString("Hi")});
	cases.push_back({"NVARCHAR(MAX) unknown", MakeCol("c", tds::TDS_TYPE_NVARCHAR, 0xFFFF), Plp(Utf16("Hi"), false),
					 false, ExpectString("Hi")});
	cases.push_back({"NVARCHAR(MAX) two chunks", MakeCol("c", tds::TDS_TYPE_NVARCHAR, 0xFFFF),
					 PlpTwoChunks(Utf16("Hi"), Utf16("There")), false, ExpectString("HiThere")});
	cases.push_back(
		{"NVARCHAR(MAX) empty", MakeCol("c", tds::TDS_TYPE_NVARCHAR, 0xFFFF), Plp({}, true), false, ExpectString("")});
	cases.push_back({"NVARCHAR(MAX) NULL", MakeCol("c", tds::TDS_TYPE_NVARCHAR, 0xFFFF), PlpNull(), true});
	cases.push_back({"VARCHAR(MAX)", MakeCol("c", tds::TDS_TYPE_BIGVARCHAR, 0xFFFF), Plp({'a', 'b', 'c'}, true), false,
					 ExpectString("abc")});
	cases.push_back({"VARBINARY(MAX)", MakeCol("c", tds::TDS_TYPE_BIGVARBINARY, 0xFFFF),
					 Plp({0xDE, 0xAD, 0xBE, 0xEF}, true), false, ExpectBlob({0xDE, 0xAD, 0xBE, 0xEF})});
	cases.push_back({"VARBINARY(MAX) NULL", MakeCol("c", tds::TDS_TYPE_BIGVARBINARY, 0xFFFF), PlpNull(), true});
	// XML is PLP regardless of max_length — IsPLPType() special-cases it, and so
	// must both walks.
	cases.push_back({"XML", MakeCol("c", tds::TDS_TYPE_XML, 0), Plp(Utf16("<a/>"), true), false, ExpectString("<a/>")});

	// --- Legacy LOBs (issue #197). Their row framing is unlike anything else. ---
	cases.push_back(
		{"TEXT", MakeCol("c", tds::TDS_TYPE_TEXT, 0xFFFF), Lob({'a', 'b', 'c'}), false, ExpectString("abc")});
	cases.push_back({"NTEXT", MakeCol("c", tds::TDS_TYPE_NTEXT, 0xFFFF), Lob(Utf16("Hi")), false, ExpectString("Hi")});
	cases.push_back(
		{"IMAGE", MakeCol("c", tds::TDS_TYPE_IMAGE, 0xFFFF), Lob({0xDE, 0xAD}), false, ExpectBlob({0xDE, 0xAD})});
	cases.push_back({"TEXT NULL", MakeCol("c", tds::TDS_TYPE_TEXT, 0xFFFF), LobNull(), true});
	cases.push_back({"NTEXT NULL", MakeCol("c", tds::TDS_TYPE_NTEXT, 0xFFFF), LobNull(), true});
	cases.push_back({"IMAGE NULL", MakeCol("c", tds::TDS_TYPE_IMAGE, 0xFFFF), LobNull(), true});
	// A LOB whose data is empty but whose text pointer is present: a zero-length
	// VALUE, which is a different thing from the zero-length POINTER that means
	// NULL.
	cases.push_back({"TEXT empty", MakeCol("c", tds::TDS_TYPE_TEXT, 0xFFFF), Lob({}), false, ExpectString("")});

	for (const Case &c : cases) {
		RunAllForms(c);
	}
	std::cout << "    " << cases.size() << " wire forms x 3 row shapes" << std::endl;
}

//===--------------------------------------------------------------------===//
// [2] Regression: NBCROW carrying the 0xFFFF sentinel on a binary/char column
//===--------------------------------------------------------------------===//

//! An NBCROW says "this column is NULL" in the bitmap, so a conforming server
//! never also sends the 0xFFFF NULL sentinel in the length prefix. A corrupt or
//! hostile stream can, and the two walks disagreed about what that means:
//!
//!   RowReader::SkipValueNBC  -> 0xFFFF is NULL, the value is 2 bytes
//!   StageNBCRow P2StageBinary -> 0xFFFF is a LENGTH, memcpy 65535 bytes
//!
//! Since SkipNBCRow is what proves the row is buffered, the stager read ~64 KB
//! past a row it was told was 7 bytes long: a heap over-read whose bytes are
//! then published as a BLOB/VARCHAR value.
//!
//! P2StageString escaped only by accident — 0xFFFF is odd, so the UTF-16
//! whole-code-unit check rejected it first. That is luck, not a guard, and it
//! is pinned here too so it cannot quietly become the load-bearing check.
void TestNbcNullSentinelIsNotALength() {
	std::cout << "[2] regression: NBCROW + 0xFFFF prefix must not be read as a length..." << std::endl;

	struct Sentinel {
		const char *label;
		ColumnMetadata col;
	};
	const Sentinel sentinels[] = {
		{"varbinary(50)", MakeCol("c", tds::TDS_TYPE_BIGVARBINARY, 50)},
		{"binary(50)", MakeCol("c", tds::TDS_TYPE_BIGBINARY, 50)},
		{"varchar(50)", MakeCol("c", tds::TDS_TYPE_BIGVARCHAR, 50)},
		{"char(50)", MakeCol("c", tds::TDS_TYPE_BIGCHAR, 50)},
		{"nvarchar(50)", MakeCol("c", tds::TDS_TYPE_NVARCHAR, 100)},
		{"nchar(50)", MakeCol("c", tds::TDS_TYPE_NCHAR, 100)},
	};

	for (const Sentinel &s : sentinels) {
		std::vector<ColumnMetadata> meta;
		meta.push_back(s.col);
		meta.push_back(CanaryCol());

		// Bitmap says both columns are PRESENT, but the prefix says NULL.
		std::vector<uint8_t> row;
		row.push_back(0x00);
		row.push_back(0xFF);
		row.push_back(0xFF);
		AppendLe32(row, static_cast<uint32_t>(CANARY));

		RowReader reader(meta);
		size_t consumed = 0;
		CHECK_TRUE(reader.SkipNBCRow(row.data(), row.size(), consumed), "SkipNBCRow bounds the malformed row");
		// This is the number the stager is entitled to rely on: 1 bitmap byte,
		// 2 for the sentinel, 4 for the canary.
		CHECK_EQ(consumed, static_cast<size_t>(7), std::string(s.label) + ": RowReader treats 0xFFFF as NULL");

		std::unique_ptr<uint8_t[]> exact(new uint8_t[row.size()]);
		std::memcpy(exact.get(), row.data(), row.size());

		Vector under_test(TypeConverter::GetDuckDBType(s.col));
		Vector canary(LogicalType::INTEGER);
		std::vector<Vector *> targets;
		targets.push_back(&under_test);
		targets.push_back(&canary);

		RowStager stager;
		stager.Configure(meta, targets);
		stager.BeginChunk(targets);

		// Under ASan a regression aborts the process here rather than failing an
		// assertion — that IS the report. A throw is also acceptable behaviour;
		// what must not happen is a 65535-byte read.
		bool threw = false;
		try {
			stager.StageNBCRow(exact.get(), row.size(), 0);
			stager.FinalizeChunk(1);
		} catch (const duckdb::Exception &) {
			threw = true;
		}

		if (threw) {
			// Rejecting the malformed prefix outright is fine; the point is that
			// it is not consumed as a length.
			continue;
		}
		CHECK_TRUE(!FlatVector::Validity(under_test).RowIsValid(0),
				   std::string(s.label) + ": 0xFFFF sentinel decodes as NULL, not a 65535-byte value");
		CHECK_EQ(FlatVector::GetData<int32_t>(canary)[0], CANARY,
				 std::string(s.label) + ": canary INT survives the sentinel");
	}
}

}  // namespace

int main() {
	std::cout << "============================================================" << std::endl;
	std::cout << "RowStager framing tests (spec 055 T5)" << std::endl;
	std::cout << "============================================================" << std::endl;

	try {
		TestFramingAgreesWithRowReader();
		TestNbcNullSentinelIsNotALength();
	} catch (const std::exception &e) {
		std::cerr << "UNCAUGHT EXCEPTION: " << e.what() << std::endl;
		return 2;
	}

	std::cout << "============================================================" << std::endl;
	if (failures > 0) {
		std::cerr << failures << " assertion(s) failed." << std::endl;
		return 1;
	}
	std::cout << "ALL TESTS PASSED" << std::endl;
	return 0;
}
