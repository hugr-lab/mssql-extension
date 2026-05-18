// test/cpp/test_issue_96_attach_loop.cpp
//
// Spec 047 (Process-Wide State Cleanup) — US1 acceptance test, T024.
// C++ port of the verbatim Python loop from
// https://github.com/hugr-lab/mssql-extension/issues/96.
//
// Pre-047 (`MSSQLContextManager` singleton with global
// `g_context_managers` map): the second iteration's ATTACH throws
// `Catalog Error: MSSQL Error: Context 'TO_MSSQL' already exists.
// Use a different name or DETACH first.` because the prior instance's
// ~MSSQLCatalog never cleared the singleton context entry (the entry
// was keyed by alias name and the instance pointer it held was now
// dangling).
//
// Post-047 (per-catalog ownership + MSSQLContextManager deletion):
// each fresh DuckDB instance has its own catalog map; ATTACH-ing the
// same alias 100 times in distinct instances always succeeds.
//
// Verifies SC-009 — closes issue #96.
//
// REQUIRES: Running SQL Server with `TestDB` database
// (provisioned by docker/init/init.sql).
//
// Environment variables (same defaults as the Makefile test harness):
//   MSSQL_TEST_HOST  (default: localhost)
//   MSSQL_TEST_PORT  (default: 1433)
//   MSSQL_TEST_USER  (default: sa)
//   MSSQL_TEST_PASS  (required — test prints a skip notice and exits 0 if unset)
//
// Build + run via `make test-issue-96-attach-loop`.

#include <cassert>
#include <cstdlib>
#include <iostream>
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
	std::string database;

	static TestConfig FromEnv() {
		TestConfig c;
		c.host = getenv_or("MSSQL_TEST_HOST", "localhost");
		c.port = getenv_or("MSSQL_TEST_PORT", "1433");
		c.user = getenv_or("MSSQL_TEST_USER", "sa");
		c.pass = getenv_or("MSSQL_TEST_PASS", "");
		c.database = getenv_or("MSSQL_TEST_DB", "TestDB");
		return c;
	}

	bool IsValid() const {
		return !pass.empty();
	}

	std::string Dsn() const {
		std::ostringstream oss;
		oss << "Server=" << host << "," << port << ";Database=" << database << ";User Id=" << user
		    << ";Password=" << pass;
		return oss.str();
	}
};

void load_extension(Connection &conn) {
	auto r1 = conn.Query("LOAD 'build/debug/extension/mssql/mssql.duckdb_extension'");
	if (r1->HasError()) {
		auto r2 = conn.Query("LOAD mssql");
		if (r2->HasError()) {
			throw std::runtime_error("Failed to LOAD mssql extension: " + r2->GetError());
		}
	}
}

} // namespace

int main() {
	std::cout << "==========================================" << std::endl;
	std::cout << "Spec 047 — Issue #96 ATTACH loop regression" << std::endl;
	std::cout << "==========================================" << std::endl;

	auto cfg = TestConfig::FromEnv();
	if (!cfg.IsValid()) {
		std::cerr << "\nSKIPPED: MSSQL_TEST_PASS is not set." << std::endl;
		std::cerr << "Set MSSQL_TEST_HOST / PORT / USER / PASS (defaults match docker test container)." << std::endl;
		return 0;
	}

	std::cout << "\nDSN: Server=" << cfg.host << "," << cfg.port << ";Database=" << cfg.database << ";User Id="
	          << cfg.user << ";Password=***" << std::endl;
	std::cout << "Repro: 100 iterations of `{ DuckDB db; ATTACH AS TO_MSSQL; SELECT 1; }`" << std::endl;

	std::ostringstream attach_sql;
	attach_sql << "ATTACH '" << cfg.Dsn() << "' AS TO_MSSQL (TYPE mssql)";
	const std::string attach = attach_sql.str();

	const int iterations = 100;
	int failed_iter = -1;
	std::string first_failure;

	for (int i = 1; i <= iterations; ++i) {
		try {
			DuckDB db(nullptr);
			Connection conn(db);
			load_extension(conn);

			auto r_attach = conn.Query(attach);
			if (r_attach->HasError()) {
				failed_iter = i;
				first_failure = r_attach->GetError();
				break;
			}

			auto r_query = conn.Query("SELECT * FROM mssql_scan('TO_MSSQL', 'SELECT 1')");
			if (r_query->HasError()) {
				failed_iter = i;
				first_failure = r_query->GetError();
				break;
			}
			// db destructs here — no explicit DETACH.
		} catch (const std::exception &e) {
			failed_iter = i;
			first_failure = e.what();
			break;
		}

		if (i == 1 || i % 25 == 0 || i == iterations) {
			std::cout << "  iter " << i << "/" << iterations << " ok" << std::endl;
		}
	}

	if (failed_iter > 0) {
		std::cerr << "\nFAILED at iteration " << failed_iter << ": " << first_failure << std::endl;
		// Specifically call out the pre-047 manifestation if it's the one we hit.
		if (first_failure.find("already exists") != std::string::npos) {
			std::cerr << "(this is the verbatim issue #96 symptom — singleton context-map not cleared)" << std::endl;
		}
		return 1;
	}

	std::cout << "\n==========================================" << std::endl;
	std::cout << "All " << iterations << " iterations PASSED (SC-009, closes issue #96)" << std::endl;
	std::cout << "==========================================" << std::endl;
	return 0;
}
