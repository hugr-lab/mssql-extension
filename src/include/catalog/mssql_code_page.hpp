#pragma once

#include <cstdint>
#include <string>

namespace duckdb {
namespace mssql {

//! The Windows code page a SQL Server collation stores `varchar` data in, from
//! the collation's name: the `_CPnnn_` token of a `SQL_` collation (`CP1` is
//! 1252), the language family of a Windows collation (`Cyrillic_General` is
//! 1251), `_UTF8` is 65001. 0 when the name is empty or the family is not in
//! the table — every caller treats 0 as "cannot tell", which is the safe side.
//! Issue #361: the answer decides whether a non-ASCII constant may travel as a
//! `varchar` parameter (the seekable form on a `SQL_` collation) or must go as
//! `nvarchar`.
constexpr int32_t CODE_PAGE_UNKNOWN = 0;
constexpr int32_t CODE_PAGE_UTF8 = 65001;

int32_t CodePageOfCollation(const std::string &collation_name);

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
