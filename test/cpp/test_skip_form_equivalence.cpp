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

//! `"0x" + std::to_string(0xE7)` prints 0x231. Format the byte properly so a
//! failure names the type it actually saw.
std::string HexByte(uint8_t v) {
	static const char *d = "0123456789ABCDEF";
	return std::string("0x") + d[(v >> 4) & 0xF] + d[v & 0xF];
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
			Fail("PLP type " + HexByte(t), std::string("resolved ") + FormName(d.form) + ", expected SLOW");
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


//===----------------------------------------------------------------------===//
// [4] Multi-column rows
//
// Everything above uses a ONE-column row, which localises a width divergence
// but leaves three things unexercised — and they are exactly the ones the
// per-column design introduces:
//
//   * `skip_descs_[col_idx]` indexed with more than one entry. The
//     `SetSkipDescriptors(descs, count)` assert added alongside this test
//     guards a descriptor/column transposition, and with one column it can
//     never fire.
//   * a SLOW column sandwiched between fast ones, where `SkipRow` threads the
//     offset from `SkipByForm` into the `SkipValue` fallback and back.
//   * an NBC row with a NULL bit actually set — the bitmap above is always
//     0x00, so the "a NULL column carries no bytes" accounting is untested.
//
// A transposition or an NBC bitmap miscount passes [1] cleanly. These do not.
//===----------------------------------------------------------------------===//

struct MultiCase {
	std::string label;
	std::vector<ColumnMetadata> cols;
	std::vector<std::vector<uint8_t>> wires;  // one per column, ROW framing
	int null_col = -1;                        // NBC only: which column's bit is set
};

void CheckMulti(const MultiCase &m, bool nbc) {
	const std::string what = m.label + (nbc ? " [NBC]" : " [ROW]");
	if (m.cols.size() != m.wires.size()) {
		Fail(what, "fixture: column and wire counts differ");
		return;
	}

	// An NBC row omits a NULL column's bytes entirely; a ROW row carries every
	// column, so null_col is ignored there.
	std::vector<uint8_t> body;
	for (size_t i = 0; i < m.cols.size(); i++) {
		if (nbc && static_cast<int>(i) == m.null_col) {
			continue;
		}
		body.insert(body.end(), m.wires[i].begin(), m.wires[i].end());
	}

	std::vector<uint8_t> row;
	size_t bitmap_bytes = 0;
	if (nbc) {
		bitmap_bytes = (m.cols.size() + 7) / 8;
		row.assign(bitmap_bytes, 0x00);
		if (m.null_col >= 0) {
			row[m.null_col >> 3] |= static_cast<uint8_t>(1u << (m.null_col & 7));
		}
	}
	row.insert(row.end(), body.begin(), body.end());

	// Legacy: no descriptors, so every column takes the per-value arm.
	RowReader legacy(m.cols);
	size_t legacy_consumed = 0;
	const bool legacy_ok = nbc ? legacy.SkipNBCRow(row.data(), row.size(), legacy_consumed)
							   : legacy.SkipRow(row.data(), row.size(), legacy_consumed);
	if (!legacy_ok || legacy_consumed != row.size()) {
		Fail(what, "fixture is malformed: legacy walk consumed " + std::to_string(legacy_consumed) + " of " +
					   std::to_string(row.size()));
		return;
	}

	// Fast: the descriptors the parser would have resolved, one per column.
	std::vector<SkipDesc> descs;
	descs.reserve(m.cols.size());
	for (const auto &c : m.cols) {
		descs.push_back(ResolveSkipForm(c));
	}
	RowReader fast(m.cols);
	fast.SetSkipDescriptors(descs.data(), descs.size());
	size_t fast_consumed = 0;
	const bool fast_ok = nbc ? fast.SkipNBCRow(row.data(), row.size(), fast_consumed)
							 : fast.SkipRow(row.data(), row.size(), fast_consumed);
	if (!fast_ok) {
		Fail(what, "fast walk did not complete the row");
		return;
	}
	if (fast_consumed != legacy_consumed) {
		std::string forms;
		for (const auto &d : descs) {
			forms += std::string(forms.empty() ? "" : ",") + FormName(d.form);
		}
		Fail(what, "fast consumed " + std::to_string(fast_consumed) + ", legacy " +
					   std::to_string(legacy_consumed) + " (forms=" + forms + ")");
	}
}

void TestMultiColumn() {
	std::cout << "[4] multi-column rows: descriptor indexing, SLOW in the middle, NBC NULL bit..." << std::endl;

	const ColumnMetadata c_int = MakeCol(tds::TDS_TYPE_INT, 4);                 // FIXED
	const ColumnMetadata c_intn = MakeCol(tds::TDS_TYPE_INTN, 4);              // PREFIX1
	const ColumnMetadata c_nvar = MakeCol(tds::TDS_TYPE_NVARCHAR, 40);         // PREFIX2
	const ColumnMetadata c_plp = MakeCol(tds::TDS_TYPE_NVARCHAR, 0xFFFF);      // SLOW
	const ColumnMetadata c_guid = MakeCol(tds::TDS_TYPE_UNIQUEIDENTIFIER, 16); // PREFIX1

	std::vector<MultiCase> cases;

	// Every form together, so a descriptor/column transposition shifts a width.
	cases.push_back({"INT|INTN|NVARCHAR",
					 {c_int, c_intn, c_nvar},
					 {Bytes(4), P1(Bytes(4)), P2(Bytes(8))}});

	// A SLOW column between two fast ones: SkipRow must hand off to the legacy
	// arm and pick the offset back up.
	cases.push_back({"INT|PLP(SLOW)|NVARCHAR",
					 {c_int, c_plp, c_nvar},
					 {Bytes(4), Plp(Bytes(6)), P2(Bytes(4))}});

	// Deliberately asymmetric widths: a transposition of columns 0 and 2 would
	// keep the total identical if the widths matched, so they must not.
	cases.push_back({"GUID|INT|NVARCHAR",
					 {c_guid, c_int, c_nvar},
					 {P1(Bytes(16)), Bytes(4), P2(Bytes(2))}});

	// Wider than one bitmap byte, so NBC bit addressing past byte 0 is covered.
	{
		MultiCase wide;
		wide.label = "9 columns, NULL in the second bitmap byte";
		for (int i = 0; i < 9; i++) {
			wide.cols.push_back(c_int);
			wide.wires.push_back(Bytes(4));
		}
		wide.null_col = 8;
		cases.push_back(wide);
	}

	for (const MultiCase &m : cases) {
		CheckMulti(m, /*nbc=*/false);
		CheckMulti(m, /*nbc=*/true);
	}

	// The same shapes again with a NULL bit set on the MIDDLE column, which is
	// the case that exercises "a NULL column consumes no bytes" while columns
	// on both sides still do.
	for (MultiCase m : cases) {
		if (m.cols.size() < 3) {
			continue;
		}
		m.label += " (middle NULL)";
		m.null_col = 1;
		CheckMulti(m, /*nbc=*/true);
	}
	std::cout << "    " << cases.size() << " shapes x ROW/NBC, plus middle-NULL variants" << std::endl;
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
		TestMultiColumn();
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
