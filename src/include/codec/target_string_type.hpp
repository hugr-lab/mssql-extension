//===----------------------------------------------------------------------===//
//                         DuckDB - MSSQL Extension
//
// codec/target_string_type.hpp
//
// Spec 060 — MSSQL_VARCHAR(n) / MSSQL_NVARCHAR(n): a string column's SQL Server
// type, stated by a cast or reported by the catalog, and carried on the
// LogicalType so that every site which generates DDL or sizes the wire can read
// the same answer.
//
// The bound type is a plain VARCHAR. DuckDB therefore neither enforces `length`
// nor truncates to it — a longer value sits in the vector untouched. The bound
// bites in exactly two places: our own guard at the write boundary, and the
// server. Anything that reads `mssql_varchar(50)` as a DuckDB constraint is
// reading it wrong.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {
namespace codec {

//! A string column's stated SQL Server type.
struct TargetStringType {
	//! nvarchar — `length` counts UTF-16 code units. Otherwise varchar, and
	//! `length` counts BYTES, which is SQL Server's own unit for it: the same n
	//! in the two types does not hold the same data (spec 060 § 3).
	bool unicode = true;
	//! n, in the unit `unicode` implies.
	int32_t length = 0;
	//! Collation for a varchar column. Empty means "fall back to
	//! mssql_utf8_collation", which in turn may be empty to inherit the
	//! database default. Meaningless for nvarchar, which has no code page.
	string collation;
};

//! Longest n SQL Server accepts before the type must become MAX.
static constexpr int32_t MAX_NVARCHAR_LENGTH = 4000;
static constexpr int32_t MAX_VARCHAR_LENGTH = 8000;

//! Build the annotated VARCHAR. Used by the type binder and by the catalog.
LogicalType MakeTargetStringType(const TargetStringType &spec);

//! Read the annotation back. False for any type that does not carry one —
//! including a plain VARCHAR, which is the overwhelmingly common case.
bool TryGetTargetStringType(const LogicalType &type, TargetStringType &result);

//! `nvarchar(n)` / `varchar(n) COLLATE <c>`, with `fallback_collation` used when
//! the type states none. An empty collation emits no COLLATE clause at all, so
//! the column inherits the database default — correct on Fabric, and the
//! documented way back to pre-#225 behaviour.
string FormatTargetStringDdl(const TargetStringType &spec, const string &fallback_collation);

//! A collation name reaches T-SQL as a bare identifier — COLLATE takes no
//! quoting — so anything that is not one must be refused before it is
//! concatenated into a CREATE TABLE.
bool IsValidCollationName(const string &name);

}  // namespace codec
}  // namespace duckdb
