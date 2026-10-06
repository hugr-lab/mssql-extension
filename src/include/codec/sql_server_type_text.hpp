#pragma once

#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
namespace codec {

//! A SQL Server type as T-SQL spells it -- `nvarchar(max)`, `decimal(18, 2)`,
//! `datetime2(7)`, `int`: the base name lowercased and the modifiers as written
//! (trimmed, lowercased: `max` stays a word).
struct SqlServerTypeText {
	std::string base;
	std::vector<std::string> args;
};

//! Split `text` into base name and modifiers. False when it is not of the form
//! `name` or `name(a[, b])` -- unbalanced parentheses, or anything after the
//! closing one. Says nothing about whether the name is a known type.
bool ParseSqlServerTypeText(const std::string &text, SqlServerTypeText &out);

}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
