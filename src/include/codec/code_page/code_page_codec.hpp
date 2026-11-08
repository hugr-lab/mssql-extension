//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/code_page/code_page_codec.hpp
//
// The read path's side of spec 091's code-page module (spec 092 D2): a CHAR /
// VARCHAR / TEXT column under a code page, read as bytes, published into a
// DuckDB VARCHAR vector as UTF-8. The module itself (code_page.hpp) stays
// DuckDB-free; everything that knows a Vector or the staging lives here.
//
// Same shape as the family codecs: one batch kernel for a staged column, one
// per-value entry for the paths that are per value for every type.
//===----------------------------------------------------------------------===//

#pragma once

#include "codec/code_page/code_page_tables.hpp"
#include "codec/staging/column_staging.hpp"
#include "duckdb/common/types/vector.hpp"
#include "tds/tds_column_metadata.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
namespace codec {
namespace code_page {

using tables::CodePageTable;

//! The page a single-byte text column's wire collation names, or nullptr: not such a
//! column, a UTF-8 collation, or one newer than the tables.
const CodePageTable *WireCodePage(const tds::ColumnMetadata &col);

//! Set ColumnMetadata::code_page on every single-byte text column whose wire
//! collation names a page the module has a table for. Other columns keep nullptr,
//! and a collation newer than the tables keeps today's verbatim bytes (and the
//! warning): that is a raw scan's only way out, since it has no name to check.
void ResolveWireCodePages(std::vector<tds::ColumnMetadata> &columns);

//! A catalog scan's bare read assumed `named`, the page of the column's collation
//! name. What the column's wire collation says otherwise, as a warning's text, or
//! empty when they agree (spec 092 D1). The stream decodes by the wire's page, which
//! is right: what is stale is the catalog (a collation changed by another client; a
//! view's sys.columns stays old until sp_refreshview), and it goes on labelling the
//! column with the old collation. tools/code_pages/check_wire_keys.py rules out the
//! tables disagreeing with themselves. A UTF-8 wire collation says nothing: those bytes
//! are valid whatever the catalog remembers.
std::string NamedCodePageMismatch(const tds::ColumnMetadata &col, const CodePageTable &named,
								  const std::string &collation_name);

//! A column the catalog had read bare, because the collation it names has a table, that arrives
//! under a collation with none: a catalog staler than the column's collation, changed to one newer
//! than the tables. Its bytes are not UTF-8, so it is an error, not a warning (review of #538):
//! the server's cast that kept such bytes off the catalog path is the one bare reads dropped. Empty
//! when the column decodes. `collation_name` is the catalog's, empty where it has none.
std::string UndecodableBareRead(const tds::ColumnMetadata &col, const std::string &collation_name);

//! Publish a staged column whose ColumnMetadata::code_page is set: its UTF-8 in
//! the vector's string heap and every row's string_t, in one call into the module
//! (code_page::DecodeColumn). A BIGCHAR's padding is trimmed off the output.
void DecodeChunkFromStaging(const staging::ColumnStaging &st, idx_t count, const tds::ColumnMetadata &col, Vector &out);

//! One value of such a column: the constant column, and the per-value paths (a
//! composite rowid's STRUCT, INSERT ... RETURNING).
void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row);

}  // namespace code_page
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
