// test/cpp/test_skip_form_equivalence.cpp
//
// Spec 058 D1-alt: the skip walk dispatches on a per-column `SkipDesc` resolved
// once at COLMETADATA, instead of a per-value switch on `type_id`. That is only
// safe while `ResolveSkipForm` agrees with the `SkipValue` / `SkipValueNBC`
// arms it replaces, and that agreement is a TWO-SWITCH invariant living in one
// file with nothing holding it.
//
// The asymmetry is what makes it worth a test:
//
//   * a type added to SkipValue but not to ResolveSkipForm degrades to SLOW,
//     which is correct — just slower;
//   * a WIDTH edited in one switch and not the other silently misframes every
//     row after that column, and no existing test fails. `diff_check.sh` needs
//     a live server and is manual; the SQL suite is `[mssql]`-grouped and gated;
//     the fuzzer catches crashes, not divergence.
//
// So this asserts the equivalence directly: for every TDS type, build a
// synthetic column and a synthetic value, then require that the fast walk
// consumes EXACTLY what the legacy per-value walk consumes — in both the ROW
// and NBCROW forms.
//
// TDS-only by construction: no DuckDB headers, so it links without libduckdb
// and runs in the `cpp-unit-tests` CI job on every PR, which is the gap the
// review identified. Shape follows test/cpp/codec/test_row_stager_framing.cpp.
//
// Build & run:
//   make test-skip-form-equivalence

#include "tds/tds_column_metadata.hpp"
#include "tds/tds_row_reader.hpp"
#include "tds/tds_types.hpp"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using duckdb::tds::ColumnMetadata;
using duckdb::tds::ResolveSkipForm;
using duckdb::tds::RowReader;
using duckdb::tds::SkipDesc;
using duckdb::tds::SkipForm;

namespace tds = duckdb::tds;

namespace {

int failures = 0;

void Fail(const std::string &what, const std::string &detail) {
	std::cerr << "FAIL: " << what << " — " << detail << std::endl;
	failures++;
}

const char *FormName(SkipForm f) {
	switch (f) {
	case SkipForm::FIXED:
		return "FIXED";
	case SkipForm::PREFIX1:
		return "PREFIX1";
	case SkipForm::PREFIX2:
		return "PREFIX2";
	case SkipForm::SLOW:
		return "SLOW";
	}
	return "?";
}

ColumnMetadata MakeCol(uint8_t type_id, uint16_t max_length, uint8_t precision = 0, uint8_t scale = 0) {
	ColumnMetadata c;
	c.name = "c";
	c.type_id = type_id;
	c.max_length = max_length;
	c.precision = precision;
	c.scale = scale;
	c.collation = 0;
	c.flags = tds::COL_FLAG_NULLABLE;
	return c;
}

std::vector<uint8_t> Bytes(size_t n, uint8_t fill = 0x5A) {
	return std::vector<uint8_t>(n, fill);
}

std::vector<uint8_t> P1(const std::vector<uint8_t> &payload) {
	std::vector<uint8_t> out{static_cast<uint8_t>(payload.size())};
	out.insert(out.end(), payload.begin(), payload.end());
	return out;
}

std::vector<uint8_t> P2(const std::vector<uint8_t> &payload) {
	std::vector<uint8_t> out{static_cast<uint8_t>(payload.size() & 0xFF),
							 static_cast<uint8_t>((payload.size() >> 8) & 0xFF)};
	out.insert(out.end(), payload.begin(), payload.end());
	return out;
}

//! PLP: 8-byte declared total, one chunk, terminator.
std::vector<uint8_t> Plp(const std::vector<uint8_t> &payload) {
	std::vector<uint8_t> out;
	uint64_t total = payload.size();
	for (int i = 0; i < 8; i++) {
		out.push_back(static_cast<uint8_t>((total >> (i * 8)) & 0xFF));
	}
	if (payload.empty()) {
		out.insert(out.end(), 4, 0x00);
		return out;
	}
	const uint32_t n = static_cast<uint32_t>(payload.size());
	for (int i = 0; i < 4; i++) {
		out.push_back(static_cast<uint8_t>((n >> (i * 8)) & 0xFF));
	}
	out.insert(out.end(), payload.begin(), payload.end());
	out.insert(out.end(), 4, 0x00);
	return out;
}

struct Case {
	std::string label;
	ColumnMetadata col;
	std::vector<uint8_t> wire;
};

//! The core assertion.
//!
//! `SkipValue` is the legacy per-value walk; the fast walk is reached by
//! handing the reader a descriptor array and calling `SkipRow`. Comparing the
//! two on a ONE-COLUMN row makes the row's consumed length exactly the value's,
//! so a divergence of even one byte shows up as a mismatch rather than as
//! silent misframing of a later column.
void CheckOne(const Case &c, bool nbc) {
	const std::string what = c.label + (nbc ? " [NBC]" : " [ROW]");

	std::vector<ColumnMetadata> cols{c.col};

	// 1. Legacy: the per-value arm, with no descriptors set.
	RowReader legacy(cols);
	const size_t legacy_consumed =
		nbc ? legacy.SkipValueNBC(c.wire.data(), c.wire.size(), 0) : legacy.SkipValue(c.wire.data(), c.wire.size(), 0);
	if (legacy_consumed == 0) {
		Fail(what, "fixture is not a complete value for the legacy walk (SkipValue returned 0)");
		return;
	}
	if (legacy_consumed != c.wire.size()) {
		Fail(what, "fixture is malformed: legacy consumed " + std::to_string(legacy_consumed) + " of " +
					   std::to_string(c.wire.size()));
		return;
	}

	// 2. Fast: the same bytes through SkipRow/SkipNBCRow with the descriptor
	//    the parser would have resolved at COLMETADATA.
	const SkipDesc desc = ResolveSkipForm(c.col);
	std::vector<SkipDesc> descs{desc};

	// An NBC row prefixes a NULL bitmap; one column means one bitmap byte, and
	// bit 0 clear says the value is present.
	std::vector<uint8_t> row;
	if (nbc) {
		row.push_back(0x00);
	}
	row.insert(row.end(), c.wire.begin(), c.wire.end());

	RowReader fast(cols);
	fast.SetSkipDescriptors(descs.data(), descs.size());
	size_t fast_consumed = 0;
	const bool ok = nbc ? fast.SkipNBCRow(row.data(), row.size(), fast_consumed)
						: fast.SkipRow(row.data(), row.size(), fast_consumed);
	if (!ok) {
		Fail(what, std::string("fast walk did not complete the row (form=") + FormName(desc.form) + ")");
		return;
	}

	const size_t expect = nbc ? c.wire.size() + 1 : c.wire.size();
	if (fast_consumed != expect) {
		Fail(what, std::string("form=") + FormName(desc.form) + " width=" + std::to_string(desc.width) +
					   " consumed " + std::to_string(fast_consumed) + ", legacy path implies " +
					   std::to_string(expect));
	}
}

void TestEveryType() {
	std::cout << "[1] SkipByForm(ResolveSkipForm(col)) == SkipValue(col), every TDS type..." << std::endl;

	std::vector<Case> cases;

	// --- bare fixed width ---
	cases.push_back({"TINYINT", MakeCol(tds::TDS_TYPE_TINYINT, 1), Bytes(1)});
	cases.push_back({"BIT", MakeCol(tds::TDS_TYPE_BIT, 1), Bytes(1)});
	cases.push_back({"SMALLINT", MakeCol(tds::TDS_TYPE_SMALLINT, 2), Bytes(2)});
	cases.push_back({"INT", MakeCol(tds::TDS_TYPE_INT, 4), Bytes(4)});
	cases.push_back({"REAL", MakeCol(tds::TDS_TYPE_REAL, 4), Bytes(4)});
	cases.push_back({"FLOAT", MakeCol(tds::TDS_TYPE_FLOAT, 8), Bytes(8)});
	cases.push_back({"BIGINT", MakeCol(tds::TDS_TYPE_BIGINT, 8), Bytes(8)});
	cases.push_back({"MONEY", MakeCol(tds::TDS_TYPE_MONEY, 8), Bytes(8)});
	cases.push_back({"SMALLMONEY", MakeCol(tds::TDS_TYPE_SMALLMONEY, 4), Bytes(4)});
	cases.push_back({"DATETIME", MakeCol(tds::TDS_TYPE_DATETIME, 8), Bytes(8)});
	cases.push_back({"SMALLDATETIME", MakeCol(tds::TDS_TYPE_SMALLDATETIME, 4), Bytes(4)});

	// --- one length byte ---
	for (uint16_t w : {1, 2, 4, 8}) {
		cases.push_back({"INTN(" + std::to_string(w) + ")", MakeCol(tds::TDS_TYPE_INTN, w), P1(Bytes(w))});
	}
	cases.push_back({"BITN", MakeCol(tds::TDS_TYPE_BITN, 1), P1(Bytes(1))});
	cases.push_back({"FLOATN(4)", MakeCol(tds::TDS_TYPE_FLOATN, 4), P1(Bytes(4))});
	cases.push_back({"FLOATN(8)", MakeCol(tds::TDS_TYPE_FLOATN, 8), P1(Bytes(8))});
	cases.push_back({"MONEYN(4)", MakeCol(tds::TDS_TYPE_MONEYN, 4), P1(Bytes(4))});
	cases.push_back({"MONEYN(8)", MakeCol(tds::TDS_TYPE_MONEYN, 8), P1(Bytes(8))});
	cases.push_back({"DATETIMEN(4)", MakeCol(tds::TDS_TYPE_DATETIMEN, 4), P1(Bytes(4))});
	cases.push_back({"DATETIMEN(8)", MakeCol(tds::TDS_TYPE_DATETIMEN, 8), P1(Bytes(8))});
	cases.push_back({"UNIQUEIDENTIFIER", MakeCol(tds::TDS_TYPE_UNIQUEIDENTIFIER, 16), P1(Bytes(16))});

	// DECIMAL/NUMERIC across every mantissa bucket.
	for (uint8_t p : {4, 9, 19, 28, 38}) {
		const size_t w = p <= 9 ? 5 : (p <= 19 ? 9 : (p <= 28 ? 13 : 17));
		cases.push_back({"DECIMAL(" + std::to_string(p) + ")", MakeCol(tds::TDS_TYPE_DECIMAL, static_cast<uint16_t>(w),
																	  p, 2),
						 P1(Bytes(w))});
		cases.push_back({"NUMERIC(" + std::to_string(p) + ")", MakeCol(tds::TDS_TYPE_NUMERIC, static_cast<uint16_t>(w),
																	  p, 2),
						 P1(Bytes(w))});
	}

	// Temporal: scale drives the width, so every bucket is covered.
	cases.push_back({"DATE", MakeCol(tds::TDS_TYPE_DATE, 3), P1(Bytes(3))});
	for (uint8_t s : {0, 3, 7}) {
		const size_t t = s <= 2 ? 3 : (s <= 4 ? 4 : 5);
		cases.push_back({"TIME(" + std::to_string(s) + ")", MakeCol(tds::TDS_TYPE_TIME, 0, 0, s), P1(Bytes(t))});
		cases.push_back(
			{"DATETIME2(" + std::to_string(s) + ")", MakeCol(tds::TDS_TYPE_DATETIME2, 0, 0, s), P1(Bytes(3 + t))});
		cases.push_back({"DATETIMEOFFSET(" + std::to_string(s) + ")", MakeCol(tds::TDS_TYPE_DATETIMEOFFSET, 0, 0, s),
						 P1(Bytes(5 + t))});
	}

	// --- two length bytes ---
	cases.push_back({"NVARCHAR(20)", MakeCol(tds::TDS_TYPE_NVARCHAR, 40), P2(Bytes(8))});
	cases.push_back({"NCHAR(4)", MakeCol(tds::TDS_TYPE_NCHAR, 8), P2(Bytes(8))});
	cases.push_back({"BIGVARCHAR(20)", MakeCol(tds::TDS_TYPE_BIGVARCHAR, 20), P2(Bytes(5))});
	cases.push_back({"BIGCHAR(5)", MakeCol(tds::TDS_TYPE_BIGCHAR, 5), P2(Bytes(5))});
	cases.push_back({"BIGVARBINARY(16)", MakeCol(tds::TDS_TYPE_BIGVARBINARY, 16), P2(Bytes(4))});
	cases.push_back({"BIGBINARY(4)", MakeCol(tds::TDS_TYPE_BIGBINARY, 4), P2(Bytes(4))});
	// Empty and the 0xFFFF NULL sentinel — the short-circuit the review called
	// out by name, and the one place a form-based walk could disagree with the
	// legacy arm about how many bytes a NULL costs.
	cases.push_back({"NVARCHAR empty", MakeCol(tds::TDS_TYPE_NVARCHAR, 40), P2({})});
	cases.push_back({"NVARCHAR NULL sentinel", MakeCol(tds::TDS_TYPE_NVARCHAR, 40), {0xFF, 0xFF}});
	cases.push_back({"VARBINARY NULL sentinel", MakeCol(tds::TDS_TYPE_BIGVARBINARY, 16), {0xFF, 0xFF}});

	// --- PLP / MAX: must resolve SLOW, and the legacy walk must still own it ---
	cases.push_back({"NVARCHAR(MAX)", MakeCol(tds::TDS_TYPE_NVARCHAR, 0xFFFF), Plp(Bytes(6))});
	cases.push_back({"VARCHAR(MAX)", MakeCol(tds::TDS_TYPE_BIGVARCHAR, 0xFFFF), Plp(Bytes(3))});
	cases.push_back({"VARBINARY(MAX)", MakeCol(tds::TDS_TYPE_BIGVARBINARY, 0xFFFF), Plp(Bytes(4))});
	cases.push_back({"XML", MakeCol(tds::TDS_TYPE_XML, 0), Plp(Bytes(8))});

	for (const Case &c : cases) {
		CheckOne(c, /*nbc=*/false);
		CheckOne(c, /*nbc=*/true);
	}
	std::cout << "    " << cases.size() << " types x 2 row forms" << std::endl;
}

//! A PLP column must never take a fast form: its length is a chunk list, and no
//! fixed width or single prefix describes it. `IsPLPType()` routes it to SLOW,
//! and the review verified that today — this keeps it true.
void TestPlpStaysSlow() {
	std::cout << "[2] PLP/MAX columns resolve SLOW..." << std::endl;
	const uint8_t plp_types[] = {tds::TDS_TYPE_NVARCHAR, tds::TDS_TYPE_BIGVARCHAR, tds::TDS_TYPE_BIGVARBINARY};
	for (uint8_t t : plp_types) {
		const SkipDesc d = ResolveSkipForm(MakeCol(t, 0xFFFF));
		if (d.form != SkipForm::SLOW) {
			Fail("PLP type 0x" + std::to_string(t), std::string("resolved ") + FormName(d.form) + ", expected SLOW");
		}
	}
	const SkipDesc x = ResolveSkipForm(MakeCol(tds::TDS_TYPE_XML, 0));
	if (x.form != SkipForm::SLOW) {
		Fail("XML", std::string("resolved ") + FormName(x.form) + ", expected SLOW (XML is always PLP)");
	}
}

//! A type the resolver has not been taught must degrade to SLOW rather than
//! guess a width — the safe direction of the asymmetry.
void TestUnknownDegradesToSlow() {
	std::cout << "[3] unknown wire types degrade to SLOW..." << std::endl;
	for (uint8_t t : {0x00, 0x11, 0xF0 /* UDT */, 0x62 /* SQL_VARIANT */}) {
		const SkipDesc d = ResolveSkipForm(MakeCol(t, 8));
		if (d.form != SkipForm::SLOW) {
			Fail("unknown type " + std::to_string(t),
				 std::string("resolved ") + FormName(d.form) + ", expected SLOW");
		}
	}
}

}  // namespace

int main() {
	std::cout << "============================================================" << std::endl;
	std::cout << "SkipForm equivalence (spec 058 D1-alt)" << std::endl;
	std::cout << "============================================================" << std::endl;

	try {
		TestEveryType();
		TestPlpStaysSlow();
		TestUnknownDegradesToSlow();
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
