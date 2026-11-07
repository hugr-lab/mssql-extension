//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/code_page/code_page_tables.hpp
//
// The shapes of the generated tables (spec 091). The data is
// src/codec/code_page/code_page_tables.cpp, written by
// tools/code_pages/emit_tables.py from the server's own answers; nothing here
// is meant to be read outside codec/code_page/, which wraps it.
//
// No DuckDB include: the fuzz harness (fuzz/fuzz_code_page.cc) and the unit
// test (test/cpp/codec/test_code_page.cpp) link this module alone. The checks
// on the data are the emitter's, in Python, before any of this is written.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <cstdint>

namespace duckdb {
namespace mssql {
namespace codec {
namespace code_page {
namespace tables {

//! One code page SQL Server stores varchar in.
struct CodePageTable {
	uint16_t code_page;
	//! The entry's position in CODE_PAGES: what per-page state built at run time
	//! (the NEON kernel's planes) is found by, so a CodePageTable is always an
	//! entry of CODE_PAGES (FindCodePage), never a copy.
	uint8_t index;
	//! UTF-8 of every byte read alone, packed little-endian in the low 24 bits
	//! with its length in the top octet (1 for 0x00-0x7F, which read as
	//! themselves). A lead byte of a double-byte page has length 0.
	const uint32_t *single;
	//! The same bytes as UTF-16 code units (0 for a lead byte): the input of the
	//! two-stage decode, code page -> UTF-16 -> UTF-8.
	const uint16_t *single16;
	//! Double-byte pages only, else nullptr: 1 + the PAIRS row of a lead byte,
	//! 0 for any other byte.
	const uint8_t *lead_row;
	//! Double-byte pages only: rows of 256 code points, lead x trail. Entry 0 of
	//! a row is what the lead byte reads as when 0x00 follows it; the 0x00 is
	//! then read on its own.
	const uint16_t *pairs;
};

//! A row of sys.fn_helpcollations() with its COLLATIONPROPERTY answers.
struct CollationRow {
	const char *name;
	//! COLLATIONPROPERTY(name, 'CollationId'): SortId in the top octet, an
	//! internal index below (not the LCID). What a backup's catalog stores.
	int32_t collation_id;
	uint32_t lcid;
	//! 0 for a Unicode-only collation, 65001 for a _UTF8 one.
	uint32_t code_page;
	uint8_t version;
};

struct LcidCodePage {
	uint32_t lcid;
	bool utf8;
	uint32_t code_page;
};

extern const CodePageTable CODE_PAGES[];
extern const size_t CODE_PAGE_COUNT;
//! Sorted by collation_id.
extern const CollationRow COLLATIONS[];
extern const size_t COLLATION_COUNT;
//! Indices into COLLATIONS sorted by the ASCII-lowercased name.
extern const uint16_t COLLATION_BY_NAME[];
//! Indexed by SortId; -1 (CODE_PAGE_NOT_FOUND) where no collation has it.
extern const int32_t SORT_ID_CODE_PAGE[256];
//! Sorted by (lcid, utf8).
extern const LcidCodePage LCID_CODE_PAGES[];
extern const size_t LCID_CODE_PAGE_COUNT;

}  // namespace tables
}  // namespace code_page
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
