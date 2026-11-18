// Issue #572: mssql::DuckDBColumnNames -- the names DuckDB reads a SQL Server table's columns under.
// No SQL Server.

#include <iostream>
#include <string>
#include <vector>

#include "catalog/mssql_column_names.hpp"

namespace {

int failures = 0;

void Check(const std::vector<std::string> &in, const std::vector<std::string> &want, const std::string &what) {
	const auto got = duckdb::mssql::DuckDBColumnNames(in);
	const bool ok = got == want;
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
		for (const auto &n : got) {
			std::cout << "    got " << n << std::endl;
		}
	}
}

}  // namespace

int main() {
	Check({"a", "b", "my col", "q]t'x"}, {"a", "b", "my col", "q]t'x"}, "names apart stay as they are");
	Check({"Id", "ID", "id"}, {"Id", "ID_1", "id_2"}, "names that differ only in case take _1, _2");
	Check({"Id", "ID", "ID_1"}, {"Id", "ID_2", "ID_1"}, "a real column ID_1 keeps its name");
	Check({"Id", "ID", "id_1"}, {"Id", "ID_2", "id_1"}, "reserved whatever its case");
	Check({"", "", "x"}, {"C0", "C1", "x"}, "unnamed result columns named as DuckDB names them");
	Check({"", "C0"}, {"C0_1", "C0"}, "an unnamed column never takes a real column's name (review of #574)");
	Check({"x", "x"}, {"x", "x_1"}, "an exact repeat (a raw scan's) is named apart too");
	// A query's own aliases over the first names (review of #574): only a later name equal to one moves.
	auto aliased = [](const std::vector<std::string> &server, const std::vector<std::string> &aliases) {
		auto got = duckdb::mssql::AvoidAliases(duckdb::mssql::DuckDBColumnNames(server), server, aliases);
		for (size_t i = 0; i < aliases.size(); i++) {
			got[i] = aliases[i];  // DuckDB's overlay
		}
		return got;
	};
	auto check_aliased = [&](const std::vector<std::string> &server, const std::vector<std::string> &aliases,
							 const std::vector<std::string> &want, const std::string &what) {
		const bool ok = aliased(server, aliases) == want;
		std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
		if (!ok) {
			failures++;
		}
	};
	check_aliased({"x", "x"}, {"x_1"}, {"x_1", "x"}, "x, x AS s(x_1): x_1, x");
	check_aliased({"x", "x", "x"}, {"a"}, {"a", "x_1", "x_2"}, "x, x, x AS s(a): a, x_1, x_2 (s.x_1 reads 2)");
	check_aliased({"x", "x", "x"}, {"a", "b"}, {"a", "b", "x_2"}, "x, x, x AS s(a, b): a, b, x_2");
	check_aliased({"p", "q", "r"}, {"a"}, {"a", "q", "r"}, "no repeat: unchanged");
	check_aliased({"x", "x", "x"}, {"x_2"}, {"x_2", "x_1", "x"}, "x, x, x AS s(x_2): the third moves to x");
	std::cout << (failures ? "FAILED" : "PASSED") << " (" << failures << " failure(s))" << std::endl;
	return failures ? 1 : 0;
}
