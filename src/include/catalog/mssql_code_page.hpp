#pragma once

#include <cstdint>
#include <string>

namespace duckdb {
namespace mssql {

//! Code pages as SQL Server reports them — COLLATIONPROPERTY(name, 'CodePage'):
//! 1252 for SQL_Latin1_General_CP1, 1251 for Cyrillic_General, 65001 for a
//! _UTF8 collation, 0 (NULL) for a non-text column or a Unicode-only
//! collation. The metadata loaders read it with the column and the database
//! collation (issue #361); every consumer treats 0 as "cannot tell", the safe
//! side: a non-ASCII constant then travels as nvarchar, as it always did.
constexpr int32_t CODE_PAGE_UNKNOWN = 0;
constexpr int32_t CODE_PAGE_UTF8 = 65001;

//! Whether every character of a UTF-8 string has a representation in the code
//! page. ASCII is representable everywhere SQL Server stores `varchar` (every
//! Windows code page is an ASCII superset), UTF-8 holds everything, and the
//! single-byte pages 874 and 1250–1258 are answered from their tables. Unknown
//! pages and the double-byte ones (932, 936, 949, 950 — no tables) answer false
//! for anything outside ASCII, which keeps such constants on the `nvarchar`
//! path they take today.
bool CodePageCanEncode(int32_t code_page, const std::string &utf8);

}  // namespace mssql
}  // namespace duckdb
