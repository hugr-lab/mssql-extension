#pragma once

namespace duckdb {
namespace mssql {

//! The collation the extension writes UTF-8 text under when it chooses one: a VARCHAR column CTAS creates
//! (`mssql_utf8_collation`'s default, issue #225; BIN2 as Fabric Warehouse's own default) and a
//! sql_variant's string values when the database's collation is not in the build's table (spec 093 T2).
//! Codec-level so the codec does not depend on dml.
constexpr const char *MSSQL_DEFAULT_UTF8_COLLATION = "Latin1_General_100_BIN2_UTF8";

}  // namespace mssql
}  // namespace duckdb
