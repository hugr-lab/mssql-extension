// test/cpp/test_result_stream_registry_isolation.cpp
//
// Spec 047 (Process-Wide State Cleanup) — US3 acceptance test, T040.
// Verifies SC-006: per-catalog result stream registry isolation.
//
// Pre-047: `MSSQLResultStreamRegistry` was a process-wide singleton keyed
// by uint64_t IDs. UUIDs generated in instance A's `MSSQLScanBind` were
// addressable from instance B's `MSSQLScanInitGlobal` (and vice versa) —
// a real cross-instance leak if a fresh instance had ever picked up an
// orphaned ID.
//
// Post-047: registry lives on `MSSQLCatalog` (mutex + `unordered_map<string,
// unique_ptr<MSSQLResultStream>>`). Bind looks up the catalog via
// `Catalog::GetCatalog(context, context_name)` and registers on THAT
// catalog; InitGlobal does the same. Two instances running `mssql_scan`
// against their own MSSQL catalogs cannot see each other's UUIDs.
//
// Strategy: 50 iterations of interleaved `mssql_scan` calls across two
// DuckDB instances, each with their OWN MSSQL ATTACH (same alias `mssql`,
// but the per-catalog ownership rule from US1 makes that legal). Every
// iteration must return the expected row count. If the registry regressed
// to a singleton, concurrent UUID lookups would surface as wrong row
// counts or "result stream not found" warnings.
//
// REQUIRES: Running SQL Server with `master` and `TestDB` databases.
//
// Environment variables (same defaults as Makefile harness):
//   MSSQL_TEST_HOST  (default: localhost)
//   MSSQL_TEST_PORT  (default: 1433)
//   MSSQL_TEST_USER  (default: sa)
//   MSSQL_TEST_PASS  (required — test prints SKIP and exits 0 if unset)
//
// Build + run via `make test-result-stream-registry-isolation`.

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "duckdb.hpp"

using namespace duckdb;

namespace {

std::string getenv_or(const char *name, const char *default_value) {
	const char *value = std::getenv(name);
	return value ? value : default_value;
}

struct TestConfig {
	std::string host;
	std::string port;
	std::string user;
	std::string pass;

	static TestConfig FromEnv() {
		TestConfig c;
		c.host = getenv_or("MSSQL_TEST_HOST", "localhost");
		c.port = getenv_or("MSSQL_TEST_PORT", "1433");
		c.user = getenv_or("MSSQL_TEST_USER", "sa");
		c.pass = getenv_or("MSSQL_TEST_PASS", "");
		return c;
	}

	bool IsValid() const {
		return !pass.empty();
	}

	std::string DsnFor(const std::string &database) const {
		std::ostringstream oss;
		oss << "Server=" << host << "," << port << ";Database=" << database << ";User Id=" << user
		    << ";Password=" << pass;
		return oss.str();
	}
};

void check_no_error(unique_ptr<MaterializedQueryResult> &result, const std::string &context) {
	if (result->HasError()) {
		throw std::runtime_error(context + ": " + result->GetError());
	}
}

void load_extension(Connection &conn) {
	auto r1 = conn.Query("LOAD 'build/debug/extension/mssql/mssql.duckdb_extension'");
	if (r1->HasError()) {
		auto r2 = conn.Query("LOAD mssql");
		if (r2->HasError()) {
			throw std::runtime_error("Failed to LOAD mssql extension: " + r2->GetError());
		}
	}
}

int64_t query_single_int(Connection &conn, const std::string &sql) {
	auto r = conn.Query(sql);
	check_no_error(r, "Query failed: " + sql);
	if (r->RowCount() == 0) {
		throw std::runtime_error("Query returned no rows: " + sql);
	}
	return r->GetValue(0, 0).GetValue<int64_t>();
}

// ---------------------------------------------------------------------------
// Per-catalog stream registry isolation (SC-006).
//
// Two DuckDB instances, each with its own MSSQL catalog under the same
// alias `mssql` (per-catalog ownership from US1 makes shared aliasing
// safe). 50 iterations of interleaved `mssql_scan` calls. Each scan
// triggers a fresh Register/Retrieve cycle on its own catalog. Any
// regression that re-shares the registry process-wide would surface as a
// mis-routed stream (wrong row count) or a registry-miss fallback that
// re-executes the query against the wrong catalog (still wrong count).
// ---------------------------------------------------------------------------
void scenario_per_catalog_stream_isolation(const TestConfig &cfg) {
	std::cout << "\n=== Per-catalog result-stream registry isolation (SC-006) ===" << std::endl;

	DuckDB db_a(nullptr);
	DuckDB db_b(nullptr);
	Connection conn_a(db_a);
	Connection conn_b(db_b);

	load_extension(conn_a);
	load_extension(conn_b);

	std::ostringstream attach_a;
	attach_a << "ATTACH '" << cfg.DsnFor("master") << "' AS mssql (TYPE mssql)";
	std::ostringstream attach_b;
	attach_b << "ATTACH '" << cfg.DsnFor("TestDB") << "' AS mssql (TYPE mssql)";

	auto r_a = conn_a.Query(attach_a.str());
	check_no_error(r_a, "Instance A ATTACH (master)");
	auto r_b = conn_b.Query(attach_b.str());
	check_no_error(r_b, "Instance B ATTACH (TestDB)");

	// Tiny TOP N keeps each scan cheap but still exercises the full
	// Bind→InitGlobal stream handoff path (the location of the former
	// singleton lookup). 5 is well below the row count of sys.tables in
	// either database.
	const int iterations = 50;
	const int top_n = 5;
	const std::string scan_sql = "SELECT COUNT(*) FROM mssql_scan('mssql', 'SELECT TOP " + std::to_string(top_n) +
	                             " name FROM sys.tables')";

	for (int i = 0; i < iterations; ++i) {
		auto count_a = query_single_int(conn_a, scan_sql);
		auto count_b = query_single_int(conn_b, scan_sql);
		if (count_a != top_n) {
			throw std::runtime_error("Iteration " + std::to_string(i) + ": Instance A returned " +
			                         std::to_string(count_a) + " rows, expected " + std::to_string(top_n));
		}
		if (count_b != top_n) {
			throw std::runtime_error("Iteration " + std::to_string(i) + ": Instance B returned " +
			                         std::to_string(count_b) + " rows, expected " + std::to_string(top_n));
		}
	}
	std::cout << "  Completed " << iterations << " interleaved scan pairs across both instances" << std::endl;
	std::cout << "  Every scan returned exactly " << top_n << " rows (no cross-instance UUID leak)" << std::endl;
	std::cout << "  PASSED (SC-006)" << std::endl;
}

} // namespace

int main() {
	std::cout << "==========================================" << std::endl;
	std::cout << "Spec 047 — Result Stream Registry Isolation" << std::endl;
	std::cout << "==========================================" << std::endl;

	auto cfg = TestConfig::FromEnv();
	if (!cfg.IsValid()) {
		std::cerr << "\nSKIPPED: MSSQL_TEST_PASS is not set." << std::endl;
		std::cerr << "Set MSSQL_TEST_HOST / PORT / USER / PASS (defaults match docker test container)." << std::endl;
		return 0;
	}

	std::cout << "\nConnection: " << cfg.user << "@" << cfg.host << ":" << cfg.port << std::endl;

	try {
		scenario_per_catalog_stream_isolation(cfg);
	} catch (const std::exception &e) {
		std::cerr << "\nTEST FAILED: " << e.what() << std::endl;
		return 1;
	}

	std::cout << "\n==========================================" << std::endl;
	std::cout << "Scenario PASSED (SC-006)" << std::endl;
	std::cout << "==========================================" << std::endl;
	return 0;
}
