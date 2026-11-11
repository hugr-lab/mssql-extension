//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_lob.hpp
//
// Spec 085 PR 2b step 6: a LOB value read from its tree of fragment records (spec 085 § Rules, LOBs),
// for the catalog's own off-row values (a view's definition past 8000 bytes in sysobjvalues). The
// in-row root (a complex column of kind 4 or 5: a 12-byte header, then 12-byte links of u32
// cumulative size, u32 page, u16 file, u16 slot) points to fragment records (record type 4) on text
// pages (types 3 and 4) of the rowset's LOB_DATA unit: DATA (3) holds bytes from 14 to the record's
// length; SMALL_ROOT (0) a u16 size at 14 and its bytes from 20; INTERNAL (2) 16-byte links from 20
// (u64 cumulative size) and LARGE_ROOT (5) 12-byte links from 24, their count a u16 at 16. Every
// node's bytes must equal what its parent claims (a child may never claim more than its parent),
// every fragment be referenced once, every page name the unit, and the value stay within the
// caller's bound. A row-overflow pointer or a text pointer (kind 2) is the scan's (PR 3).
// Measured: a 9058-byte definition, a root of two links to DATA fragments of 8040 and 1018 bytes
// (catalog fixture, 2022). DuckDB-free (D15).
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_page.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace duckdb {
namespace mssql {
namespace backup {

//! Hands page `at` over into `out` (its bytes and header): PageStore::ReadLive's, or a test's.
typedef std::function<void(const PagePointer &at, Page &out)> LobPageFetch;

//! The value whose in-row root is `pointer`, its fragments on pages of allocation unit `lob_unit`
//! fetched through `fetch`; at most `max_bytes`. A FormatError naming `what` past the bound, for a
//! child claiming more bytes than its parent (checked before it is read, so the bound holds for every
//! node), a fragment referenced twice, a slot past the page's slot array, or a node whose bytes are not
//! the ones its parent claims.
std::string ReadLobValue(const LobPageFetch &fetch, const std::string &pointer, uint64_t lob_unit,
						 const std::string &what, uint64_t max_bytes);
//! ReadLobValue over `store`'s live pages.
std::string ReadLobValue(const PageStore &store, const std::string &pointer, uint64_t lob_unit, const std::string &what,
						 uint64_t max_bytes);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
