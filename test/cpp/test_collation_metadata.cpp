// test/cpp/test_collation_metadata.cpp
//
// Unit tests for the TDS COLLATION fields on ColumnMetadata (issue #224):
//
//   * the 5th wire byte (SortId), which was parsed and discarded until PR #320
//     and is the ONLY thing distinguishing the SQL_* code pages -- they share
//     an LCID;
//   * ColumnMetadata::IsUtf8Collation(), the fUTF8 flag at 0x04000000;
//   * ColumnMetadata::IsSingleByteTextColumn(), which decides whether a
//     column's bytes land in a DuckDB VARCHAR verbatim.
//
// These exist because the behaviour they cover was otherwise only asserted by
// test/sql/query/collation_metadata_warnings.test, which needs a live SQL
// Server and therefore does NOT gate a PR. A regression -- the TEXT/NTEXT arm
// of ParseTypeInfo drifting from the BIGVARCHAR arm, or the 0x04000000 bit
// position being "simplified" -- would have shipped green.
//
// No SQL Server, no DuckDB runtime: synthetic COLMETADATA bytes fed through
// ColumnMetadataParser. Wire values are the ones dumped from a live SQL Server
// 2025 and recorded in tds_column_metadata.hpp.
//
// Build/run: part of STANDALONE_TEST_SOURCES (`make test-cpp`), which CI runs.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "tds/tds_column_metadata.hpp"
#include "tds/tds_types.hpp"

using duckdb::tds::ColumnMetadata;
using duckdb::tds::ColumnMetadataParser;

static int g_failures = 0;

static void Check(bool ok, const std::string &what) {
	if (ok) {
		return;
	}
	std::cerr << "FAIL: " << what << std::endl;
	g_failures++;
}

//===--------------------------------------------------------------------===//
// Byte-stream builders
//===--------------------------------------------------------------------===//

static void PushU16(std::vector<uint8_t> &b, uint16_t v) {
	b.push_back(static_cast<uint8_t>(v & 0xFF));
	b.push_back(static_cast<uint8_t>(v >> 8));
}

static void PushU32(std::vector<uint8_t> &b, uint32_t v) {
	b.push_back(static_cast<uint8_t>(v & 0xFF));
	b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
	b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
	b.push_back(static_cast<uint8_t>(v >> 24));
}

//! Column name as B_VARCHAR: one length byte counting CHARACTERS, then UCS-2LE.
static void PushColumnName(std::vector<uint8_t> &b, const std::string &ascii_name) {
	b.push_back(static_cast<uint8_t>(ascii_name.size()));
	for (char c : ascii_name) {
		b.push_back(static_cast<uint8_t>(c));
		b.push_back(0);
	}
}

//! The 5 collation bytes: the 4-byte LE word the extension stores, then SortId.
static void PushCollation(std::vector<uint8_t> &b, uint32_t collation_word, uint8_t sort_id) {
	PushU32(b, collation_word);
	b.push_back(sort_id);
}

//! A BIGCHAR / BIGVARCHAR / NCHAR / NVARCHAR column: 2-byte max length + collation.
static void PushVarcharColumn(std::vector<uint8_t> &b, uint8_t type_id, uint16_t max_length, uint32_t collation_word,
							  uint8_t sort_id, const std::string &name) {
	PushU32(b, 0);	// UserType
	PushU16(b, 0);	// Flags
	b.push_back(type_id);
	PushU16(b, max_length);
	PushCollation(b, collation_word, sort_id);
	PushColumnName(b, name);
}

//! A TEXT / NTEXT column: 4-byte LONGLEN + collation. This is the SECOND parse
//! arm, and the one most likely to drift from the first.
static void PushTextColumn(std::vector<uint8_t> &b, uint8_t type_id, uint32_t collation_word, uint8_t sort_id,
						   const std::string &name) {
	PushU32(b, 0);	// UserType
	PushU16(b, 0);	// Flags
	b.push_back(type_id);
	PushU32(b, 0x7FFFFFFF);	 // LONGLEN
	PushCollation(b, collation_word, sort_id);
	// TableName: 1 byte part count, then each part as US_VARCHAR (2-byte char count).
	b.push_back(1);
	PushU16(b, 3);
	for (char c : std::string("dbo")) {
		b.push_back(static_cast<uint8_t>(c));
		b.push_back(0);
	}
	PushColumnName(b, name);
}

//! An IMAGE column: LONGLEN + TableName, and NO collation at all.
static void PushImageColumn(std::vector<uint8_t> &b, const std::string &name) {
	PushU32(b, 0);
	PushU16(b, 0);
	b.push_back(duckdb::tds::TDS_TYPE_IMAGE);
	PushU32(b, 0x7FFFFFFF);
	b.push_back(1);
	PushU16(b, 3);
	for (char c : std::string("dbo")) {
		b.push_back(static_cast<uint8_t>(c));
		b.push_back(0);
	}
	PushColumnName(b, name);
}

static std::vector<ColumnMetadata> ParseColumns(const std::vector<uint8_t> &body, uint16_t count) {
	std::vector<uint8_t> bytes;
	PushU16(bytes, count);
	bytes.insert(bytes.end(), body.begin(), body.end());

	ColumnMetadataParser parser;
	std::vector<ColumnMetadata> columns;
	size_t consumed = 0;
	if (!parser.Parse(bytes.data(), bytes.size(), consumed, columns)) {
		std::cerr << "FAIL: COLMETADATA did not parse" << std::endl;
		g_failures++;
		return {};
	}
	if (consumed != bytes.size()) {
		std::cerr << "FAIL: parser consumed " << consumed << " of " << bytes.size() << " bytes" << std::endl;
		g_failures++;
	}
	return columns;
}

//===--------------------------------------------------------------------===//
// Wire values, dumped from a live SQL Server 2025
//===--------------------------------------------------------------------===//

static constexpr uint32_t COLL_UTF8_CI_AS = 0x24D00409;	 // Latin1_General_100_CI_AS_SC_UTF8
static constexpr uint32_t COLL_UTF8_BIN2 = 0x26000409;	 // Latin1_General_100_BIN2_UTF8
static constexpr uint32_t COLL_CP1252 = 0x00D00409;		 // Latin1_General_CI_AS
static constexpr uint32_t COLL_CP1251 = 0x00D00419;		 // Cyrillic_General_CI_AS

// The SQL_* pair: SAME LCID, and only the SortId tells them apart.
static constexpr uint8_t SORT_CP1252 = 52;	 // SQL_Latin1_General_CP1_CI_AS
static constexpr uint8_t SORT_CP1251 = 106;	 // SQL_Latin1_General_CP1251_CI_AS

//===--------------------------------------------------------------------===//
// Tests
//===--------------------------------------------------------------------===//

//! The fUTF8 flag, on the BIGVARCHAR arm.
static void TestUtf8FlagVarchar() {
	std::vector<uint8_t> body;
	PushVarcharColumn(body, duckdb::tds::TDS_TYPE_BIGVARCHAR, 50, COLL_UTF8_CI_AS, 0, "u8ci");
	PushVarcharColumn(body, duckdb::tds::TDS_TYPE_BIGVARCHAR, 50, COLL_UTF8_BIN2, 0, "u8bin");
	PushVarcharColumn(body, duckdb::tds::TDS_TYPE_BIGVARCHAR, 50, COLL_CP1252, 0, "cp1252");
	PushVarcharColumn(body, duckdb::tds::TDS_TYPE_BIGCHAR, 10, COLL_CP1251, 0, "cp1251");

	auto cols = ParseColumns(body, 4);
	if (cols.size() != 4) {
		Check(false, "expected 4 columns from the BIGVARCHAR arm");
		return;
	}
	Check(cols[0].name == "u8ci" && cols[0].collation == COLL_UTF8_CI_AS, "u8ci collation word round-trips");
	Check(cols[0].IsUtf8Collation(), "Latin1_General_100_CI_AS_SC_UTF8 is a UTF-8 collation");
	Check(cols[1].IsUtf8Collation(), "Latin1_General_100_BIN2_UTF8 is a UTF-8 collation");
	Check(!cols[2].IsUtf8Collation(), "Latin1_General_CI_AS (CP1252) is NOT a UTF-8 collation");
	Check(!cols[3].IsUtf8Collation(), "Cyrillic_General_CI_AS (CP1251) is NOT a UTF-8 collation");

	// Every one of these is single-byte text, including the BIGCHAR.
	for (size_t i = 0; i < cols.size(); i++) {
		Check(cols[i].IsSingleByteTextColumn(), "column " + std::to_string(i) + " is single-byte text");
	}
}

//! The SortId is the whole point of the 5th byte: these two collations are
//! DIFFERENT CODE PAGES and share an LCID, so the 4-byte word cannot tell them
//! apart and a message quoting only the LCID would name the wrong one.
static void TestSortIdDistinguishesCodePages() {
	std::vector<uint8_t> body;
	PushVarcharColumn(body, duckdb::tds::TDS_TYPE_BIGVARCHAR, 50, COLL_CP1252, SORT_CP1252, "sql_cp1252");
	PushVarcharColumn(body, duckdb::tds::TDS_TYPE_BIGVARCHAR, 50, COLL_CP1252, SORT_CP1251, "sql_cp1251");

	auto cols = ParseColumns(body, 2);
	if (cols.size() != 2) {
		Check(false, "expected 2 columns for the SQL_* pair");
		return;
	}
	Check(cols[0].collation == cols[1].collation, "the SQL_* pair really does share a collation word");
	Check(cols[0].collation_sort_id == SORT_CP1252, "SQL_Latin1_General_CP1_CI_AS SortId is 52");
	Check(cols[1].collation_sort_id == SORT_CP1251, "SQL_Latin1_General_CP1251_CI_AS SortId is 106");
	Check(cols[0].collation_sort_id != cols[1].collation_sort_id, "SortId distinguishes the two code pages");
	Check(!cols[0].IsUtf8Collation() && !cols[1].IsUtf8Collation(), "neither SQL_* collation is UTF-8");
}

//! The TEXT/NTEXT arm is a SEPARATE piece of parsing code that also reads the
//! 5 collation bytes. It was the arm the first cut of the fix forgot.
static void TestTextArmParsesCollation() {
	std::vector<uint8_t> body;
	PushTextColumn(body, duckdb::tds::TDS_TYPE_TEXT, COLL_CP1252, SORT_CP1252, "legacy_text");
	PushTextColumn(body, duckdb::tds::TDS_TYPE_TEXT, COLL_UTF8_BIN2, 0, "utf8_text");
	PushTextColumn(body, duckdb::tds::TDS_TYPE_NTEXT, COLL_CP1252, SORT_CP1252, "legacy_ntext");

	auto cols = ParseColumns(body, 3);
	if (cols.size() != 3) {
		Check(false, "expected 3 columns from the TEXT/NTEXT arm");
		return;
	}
	Check(cols[0].collation == COLL_CP1252, "TEXT collation word round-trips");
	Check(cols[0].collation_sort_id == SORT_CP1252, "TEXT SortId round-trips -- the arm that drifts");
	Check(!cols[0].IsUtf8Collation(), "CP1252 TEXT is not UTF-8");
	Check(cols[0].IsSingleByteTextColumn(), "TEXT is single-byte text");
	Check(cols[1].IsUtf8Collation(), "UTF-8-collated TEXT is UTF-8");
	Check(!cols[2].IsSingleByteTextColumn(), "NTEXT is NOT single-byte text -- it is UCS-2 on the wire");
}

//! Which types are candidates at all. NVARCHAR is transcoded; IMAGE carries no
//! collation and its bytes are meant to be arbitrary.
static void TestSingleByteTextClassification() {
	std::vector<uint8_t> body;
	PushVarcharColumn(body, duckdb::tds::TDS_TYPE_NVARCHAR, 100, COLL_CP1252, 0, "nv");
	PushVarcharColumn(body, duckdb::tds::TDS_TYPE_NCHAR, 20, COLL_CP1252, 0, "nc");
	PushImageColumn(body, "img");

	auto cols = ParseColumns(body, 3);
	if (cols.size() != 3) {
		Check(false, "expected 3 columns for the classification test");
		return;
	}
	Check(!cols[0].IsSingleByteTextColumn(), "NVARCHAR is not single-byte text");
	Check(!cols[1].IsSingleByteTextColumn(), "NCHAR is not single-byte text");
	Check(!cols[2].IsSingleByteTextColumn(), "IMAGE is not single-byte text");
	// IMAGE has no collation on the wire, so both fields must stay at their reset
	// values rather than carrying a previous column's.
	Check(cols[2].collation == 0 && cols[2].collation_sort_id == 0, "IMAGE leaves collation and SortId zeroed");
}

//! A column with no collation must not inherit the previous column's, which is
//! what a missing per-column reset would produce.
static void TestNoBleedBetweenColumns() {
	std::vector<uint8_t> body;
	PushVarcharColumn(body, duckdb::tds::TDS_TYPE_BIGVARCHAR, 50, COLL_CP1251, SORT_CP1251, "first");
	PushImageColumn(body, "second");
	PushVarcharColumn(body, duckdb::tds::TDS_TYPE_BIGVARCHAR, 50, COLL_UTF8_BIN2, 0, "third");

	auto cols = ParseColumns(body, 3);
	if (cols.size() != 3) {
		Check(false, "expected 3 columns for the bleed test");
		return;
	}
	Check(cols[1].collation_sort_id == 0, "SortId does not bleed from the preceding column");
	Check(cols[2].collation_sort_id == 0, "SortId is reset for a collation that carries none");
	Check(cols[2].IsUtf8Collation(), "the third column's own collation is read, not the first's");
}

int main() {
	TestUtf8FlagVarchar();
	TestSortIdDistinguishesCodePages();
	TestTextArmParsesCollation();
	TestSingleByteTextClassification();
	TestNoBleedBetweenColumns();

	if (g_failures > 0) {
		std::cerr << g_failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "All collation metadata tests passed" << std::endl;
	return 0;
}
