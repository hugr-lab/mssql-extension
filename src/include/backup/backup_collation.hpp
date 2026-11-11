//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_collation.hpp
//
// Spec 085 task 13 (PR 2b): a backup's collations against spec 091's table
// (tools/code_pages/data/collations.psv, the server's own answers). Two forms reach the reader: the
// trailer's TDS form (SCIN +216 / +220: LCID, flags, version, SortId), named for mssql_backup_info;
// and the CollationId the catalog and the boot page store (dbi_collation, syscolpars.collationid),
// which names a collation exactly, _SC included. DuckDB-free (D15).
//===----------------------------------------------------------------------===//

#pragma once

#include "codec/code_page/code_page_tables.hpp"

#include <cstdint>
#include <string>

namespace duckdb {
namespace mssql {
namespace backup {

using CollationRow = codec::code_page::tables::CollationRow;

//! The TDS collation flags a collation's name implies: _CI 0x01, _AI 0x02, kana (0x04) and width
//! (0x08) ignored unless _KS / _WS, _BIN 0x10, _BIN2 0x20, _UTF8 0x40 (spec 085 § Rules, SCIN +216).
//! Kana before width, as measured (E16: _CI_AS_KS records 0x09, _CI_AS_WS 0x05, Latin1_General_100
//! and Japanese alike); MS-TDS's COLLATION lists the two bits in both orders.
uint32_t FlagsOfName(const std::string &name);

//! The collation the TDS form (info: LCID in 20 bits, flags in 8, version in 4; SortId) names: a
//! SQL_ collation by its SortId, any other by its LCID, version and flags; of two names differing
//! only by _SC (no flag carries it), the one without, as HEADERONLY prints it. Empty when none or
//! still more than one.
std::string CollationNameOfTds(uint32_t info, uint8_t sort_id);

//! The collation whose CollationId is `id` (exact, _SC included); nullptr when the table has none.
const CollationRow *CollationById(int32_t id);

//! `row` is a collation the TDS form names (its SortId, or its LCID, version and flags): the two
//! forms of one database's collation agree.
bool CollationMatchesTds(const CollationRow &row, uint32_t info, uint8_t sort_id);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
