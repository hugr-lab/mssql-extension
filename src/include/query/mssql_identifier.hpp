#pragma once

#include <string>

namespace duckdb {
namespace mssql {

//! THE identifier quoter (spec 079 W4): `[name]`, with every `]` doubled. Every
//! identifier the extension sends -- schema, table, column, alias -- goes
//! through here. There were eight copies of it, and six more places that
//! bracketed a name without doubling `]`, so a table or column named `a]b`
//! broke COPY's INSERT BULK and DELETE.
//!
//! Self-contained (the standard library only) on purpose: header-only unit
//! tests build with `-I src/include` and no DuckDB (dml/mssql_identity_insert.hpp).
inline std::string QuoteIdentifier(const std::string &name) {
	std::string out;
	out.reserve(name.size() + 2);
	out += '[';
	for (char c : name) {
		out += c;
		if (c == ']') {
			out += ']';
		}
	}
	out += ']';
	return out;
}

}  // namespace mssql
}  // namespace duckdb
