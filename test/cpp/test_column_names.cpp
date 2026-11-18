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
	std::cout << (failures ? "FAILED" : "PASSED") << " (" << failures << " failure(s))" << std::endl;
	return failures ? 1 : 0;
}
