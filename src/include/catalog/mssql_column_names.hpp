#pragma once

#include "duckdb/common/string_util.hpp"

#include <set>
#include <string>
#include <vector>

namespace duckdb {
namespace mssql {

//! Issue #572: the names DuckDB reads a SQL Server table's columns under. DuckDB's names are
//! case-insensitive, quoted or not, and a case-sensitive database may hold columns that differ only in
//! case (`Id`, `ID`), which no DuckDB table can: a name that repeats an earlier one but for case takes
//! the first `_N` suffix no column of the table is named -- every real name reserved first, so a real
//! column keeps its own: (Id, ID, ID_1) reads as Id, ID_2, ID_1. Every other name is the server's, as
//! is every name the extension sends. An empty one (an unnamed result column, `SELECT 1`) is named as
//! DuckDB would name it, `C<position>`, but never over a real column's name: (unnamed, C0) reads as
//! C0_1, C0 (review of #574: DuckDB's own `C0` collided). Because every real name is reserved, a column the server adds
//! later can move a suffix: a new real `ID_2` makes the old ID_2 (the server's `ID`) ID_3 once the catalog reloads.
inline std::vector<std::string> DuckDBColumnNames(const std::vector<std::string> &names) {
	std::set<std::string> reserved;
	for (const auto &n : names) {
		reserved.insert(StringUtil::Lower(n));
	}
	std::set<std::string> used;
	std::vector<std::string> result;
	result.reserve(names.size());
	for (size_t i = 0; i < names.size(); i++) {
		const std::string &real = names[i];
		const bool unnamed = real.empty();
		// An unnamed result column takes DuckDB's name for it, `C<position>`, unless a real column holds it.
		const std::string n = unnamed ? "C" + std::to_string(i) : real;
		const std::string lower = StringUtil::Lower(n);
		if ((!unnamed || !reserved.count(lower)) && used.insert(lower).second) {
			result.push_back(n);
			continue;
		}
		for (size_t k = 1;; k++) {
			std::string candidate = n + "_" + std::to_string(k);
			const std::string lower = StringUtil::Lower(candidate);
			if (!reserved.count(lower) && used.insert(lower).second) {
				result.push_back(std::move(candidate));
				break;
			}
		}
	}
	return result;
}

//! `names` (DuckDBColumnNames of `server_names`) under a query's own column aliases (`… AS s(a, b)`),
//! which DuckDB lays over the first names: only a later name equal to an alias moves -- to its server
//! name when no other name holds it, else its first free suffix --, every other keeps the name it had
//! (review of #574: renumbering them all made `s.x_1` read another column). Returns the names past the
//! aliases' positions; the first ones are DuckDB's to overlay.
inline std::vector<std::string> AvoidAliases(const std::vector<std::string> &names,
											 const std::vector<std::string> &server_names,
											 const std::vector<std::string> &aliases) {
	std::set<std::string> taken;  // the aliases, and the names that stay
	for (const auto &a : aliases) {
		taken.insert(StringUtil::Lower(a));
	}
	std::set<std::string> alias_names = taken;
	std::set<std::string> reserved;	 // the real names past the aliases
	for (size_t i = aliases.size(); i < names.size(); i++) {
		reserved.insert(StringUtil::Lower(server_names[i]));
		if (!alias_names.count(StringUtil::Lower(names[i]))) {
			taken.insert(StringUtil::Lower(names[i]));
		}
	}
	std::vector<std::string> result = names;
	for (size_t i = aliases.size(); i < names.size(); i++) {
		if (!alias_names.count(StringUtil::Lower(names[i]))) {
			continue;
		}
		const std::string base = server_names[i].empty() ? "C" + std::to_string(i) : server_names[i];
		if (taken.insert(StringUtil::Lower(base)).second) {
			result[i] = base;
			continue;
		}
		for (size_t k = 1;; k++) {
			std::string candidate = base + "_" + std::to_string(k);
			const std::string lower = StringUtil::Lower(candidate);
			if (!reserved.count(lower) && taken.insert(lower).second) {
				result[i] = std::move(candidate);
				break;
			}
		}
	}
	return result;
}

}  // namespace mssql
}  // namespace duckdb
