// test/cpp/test_multi_instance_pool_isolation.cpp
//
// Spec 047 (Process-Wide State Cleanup) — US1 acceptance test, T023.
// Mirrors User Scenarios 1, 2, and 3 from spec.md and verifies
// SC-001, SC-002, SC-003.
//
// Pre-047 (`MssqlPoolManager` singleton keyed by alias name): both
// instances aliased as `mssql` share one pool. Scenarios 1 and 2 fail
// (wrong routing / cross-instance DETACH side-effect). Scenario 3
// leaks TDS sockets in the singleton's `pools_` map after instance
// destruction.
//
// Post-047 (per-catalog `unique_ptr<ConnectionPool>` ownership): each
// `MSSQLCatalog` owns its own pool. RAII closes sockets on
// `~MSSQLCatalog`. All three scenarios pass.
//
// REQUIRES: Running SQL Server with `master` and `TestDB` databases
// (provisioned by docker/init/init.sql).
//
// Environment variables (same defaults as the Makefile test harness):
//   MSSQL_TEST_HOST  (default: localhost)
//   MSSQL_TEST_PORT  (default: 1433)
//   MSSQL_TEST_USER  (default: sa)
//   MSSQL_TEST_PASS  (required — test prints a skip notice and exits 0 if unset)
//
// Build + run via `make test-multi-instance-pool-isolation`.

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
	std::string error;
	auto r1 = conn.Query("LOAD 'build/debug/extension/mssql/mssql.duckdb_extension'");
	if (r1->HasError()) {
		auto r2 = conn.Query("LOAD mssql");
		if (r2->HasError()) {
			throw std::runtime_error("Failed to LOAD mssql extension: " + r2->GetError());
		}
	}
}

std::string query_single_string(Connection &conn, const std::string &sql) {
	auto r = conn.Query(sql);
	check_no_error(r, "Query failed: " + sql);
	if (r->RowCount() == 0) {
		throw std::runtime_error("Query returned no rows: " + sql);
	}
	return r->GetValue(0, 0).GetValue<std::string>();
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
// Scenario 1 — Two concurrent instances, same alias, different DSNs.
//
// Verifies SC-001 (multi-instance routing correctness).
//
// Pre-047: both instances' queries route through whichever pool was
// registered first in the singleton, yielding `DB_NAME() = 'master'`
// for both connections.
// Post-047: each catalog owns its own pool; `DB_NAME()` reflects the
// catalog's own ATTACH credentials.
// ---------------------------------------------------------------------------
void scenario_1_routing_correctness(const TestConfig &cfg) {
	std::cout << "\n=== Scenario 1: multi-instance routing correctness ===" << std::endl;

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

	// Each catalog must route to its OWN database, not the first-attached pool.
	auto db_a_name = query_single_string(conn_a, "SELECT * FROM mssql_scan('mssql', 'SELECT DB_NAME()')");
	auto db_b_name = query_single_string(conn_b, "SELECT * FROM mssql_scan('mssql', 'SELECT DB_NAME()')");

	std::cout << "  Instance A DB_NAME(): " << db_a_name << " (expected: master)" << std::endl;
	std::cout << "  Instance B DB_NAME(): " << db_b_name << " (expected: TestDB)" << std::endl;

	if (db_a_name != "master") {
		throw std::runtime_error("Scenario 1 FAILED: Instance A routed to '" + db_a_name + "', expected 'master'");
	}
	if (db_b_name != "TestDB") {
		throw std::runtime_error("Scenario 1 FAILED: Instance B routed to '" + db_b_name + "', expected 'TestDB'");
	}

	std::cout << "  PASSED (SC-001)" << std::endl;
}

// ---------------------------------------------------------------------------
// Scenario 2 — DETACH on one instance leaves the other intact.
//
// Verifies SC-002 (DETACH-isolation correctness).
//
// Pre-047: instance A's DETACH calls `MssqlPoolManager::RemovePool("mssql")`,
// destroying the singleton entry shared with instance B; instance B's
// next query throws.
// Post-047: ~MSSQLCatalog destroys instance A's own pool only; instance B's
// catalog (and its own pool) is untouched.
// ---------------------------------------------------------------------------
void scenario_2_detach_isolation(const TestConfig &cfg) {
	std::cout << "\n=== Scenario 2: DETACH-isolation correctness ===" << std::endl;

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
	check_no_error(r_a, "Instance A ATTACH");
	auto r_b = conn_b.Query(attach_b.str());
	check_no_error(r_b, "Instance B ATTACH");

	// Warm both pools with a query.
	auto warm_a = conn_a.Query("SELECT * FROM mssql_scan('mssql', 'SELECT 1')");
	check_no_error(warm_a, "Instance A warm-up");
	auto warm_b = conn_b.Query("SELECT * FROM mssql_scan('mssql', 'SELECT 1')");
	check_no_error(warm_b, "Instance B warm-up");

	// Instance A detaches.
	auto detach_a = conn_a.Query("DETACH mssql");
	check_no_error(detach_a, "Instance A DETACH");
	std::cout << "  Instance A: DETACH mssql succeeded" << std::endl;

	// Instance B's pool MUST still be alive and routable.
	auto db_b_name = query_single_string(conn_b, "SELECT * FROM mssql_scan('mssql', 'SELECT DB_NAME()')");
	std::cout << "  Instance B DB_NAME() (after A's DETACH): " << db_b_name << " (expected: TestDB)" << std::endl;

	if (db_b_name != "TestDB") {
		throw std::runtime_error("Scenario 2 FAILED: Instance B routed to '" + db_b_name +
								 "' after A's DETACH, expected 'TestDB'");
	}

	std::cout << "  PASSED (SC-002)" << std::endl;
}

// ---------------------------------------------------------------------------
// Scenario 3 — Silent shutdown closes sockets.
//
// Verifies SC-003 (silent-shutdown reliability).
//
// 100 iterations of `{ DuckDB db; ATTACH; SELECT 1; }` with no explicit
// DETACH. After the loop, a separate verifier connection counts active
// SQL Server sessions on this server attributed to our test user. With
// the spec 045 band-aid + spec 047 RAII teardown, that count should be
// 1 (just the verifier itself).
//
// Pre-047: the singleton `pools_["mssql"]` entry survives every iteration;
// each iteration adds N idle TDS sockets to the pool that nothing ever
// closes. After 100 iterations the residual session count grows linearly.
// Post-047: ~MSSQLCatalog drops the owned `unique_ptr<ConnectionPool>`;
// idle sockets close at end-of-scope. Residual count stays at the
// verifier-only baseline.
// ---------------------------------------------------------------------------
void scenario_3_silent_shutdown(const TestConfig &cfg) {
	std::cout << "\n=== Scenario 3: silent-shutdown reliability (100 iterations) ===" << std::endl;

	// Verifier instance — kept alive throughout the test so its own
	// session is excluded from the post-loop count.
	DuckDB verifier_db(nullptr);
	Connection verifier_conn(verifier_db);
	load_extension(verifier_conn);
	std::ostringstream verifier_attach;
	verifier_attach << "ATTACH '" << cfg.DsnFor("master") << "' AS verifier (TYPE mssql)";
	auto vr = verifier_conn.Query(verifier_attach.str());
	check_no_error(vr, "Verifier ATTACH");

	// Build the verifier query via mssql_scan; T-SQL string literals get the
	// quote-doubling escape inside the outer SQL literal.
	std::ostringstream baseline_query;
	baseline_query << "SELECT * FROM mssql_scan('verifier', "
				   << "'SELECT COUNT(*) FROM sys.dm_exec_sessions WHERE login_name = ''" << cfg.user
				   << "'' AND program_name LIKE ''%DuckDB%''')";

	auto baseline = query_single_int(verifier_conn, baseline_query.str());
	std::cout << "  Baseline residual sessions (DuckDB-program_name, user " << cfg.user << "): " << baseline
			  << std::endl;

	const int iterations = 100;
	for (int i = 0; i < iterations; ++i) {
		DuckDB db(nullptr);
		Connection conn(db);
		load_extension(conn);

		std::ostringstream attach_sql;
		attach_sql << "ATTACH '" << cfg.DsnFor("TestDB") << "' AS mssql (TYPE mssql)";
		auto ar = conn.Query(attach_sql.str());
		check_no_error(ar, "Iter " + std::to_string(i) + " ATTACH");

		auto sr = conn.Query("SELECT * FROM mssql_scan('mssql', 'SELECT 1')");
		check_no_error(sr, "Iter " + std::to_string(i) + " SELECT");
		// db destructs here — silent shutdown, no explicit DETACH.
	}
	std::cout << "  Completed " << iterations << " iterations" << std::endl;

	// Re-poll: residual count should equal baseline (the verifier itself).
	// With a per-catalog pool, ~MSSQLCatalog has already closed all
	// in-loop sockets; the only DuckDB-program-named sessions left
	// belong to the verifier.
	auto residual = query_single_int(verifier_conn, baseline_query.str());
	std::cout << "  Residual sessions after loop: " << residual << " (expected: " << baseline << ")" << std::endl;

	if (residual > baseline) {
		throw std::runtime_error("Scenario 3 FAILED: " + std::to_string(residual - baseline) +
								 " residual SQL Server sessions after 100 silent-shutdown iterations");
	}

	std::cout << "  PASSED (SC-003)" << std::endl;
}

}  // namespace

int main() {
	std::cout << "==========================================" << std::endl;
	std::cout << "Spec 047 — Multi-Instance Pool Isolation" << std::endl;
	std::cout << "==========================================" << std::endl;

	auto cfg = TestConfig::FromEnv();
	if (!cfg.IsValid()) {
		std::cerr << "\nSKIPPED: MSSQL_TEST_PASS is not set." << std::endl;
		std::cerr << "Set MSSQL_TEST_HOST / PORT / USER / PASS (defaults match docker test container)." << std::endl;
		return 0;
	}

	std::cout << "\nConnection: " << cfg.user << "@" << cfg.host << ":" << cfg.port << std::endl;

	try {
		scenario_1_routing_correctness(cfg);
		scenario_2_detach_isolation(cfg);
		scenario_3_silent_shutdown(cfg);
	} catch (const std::exception &e) {
		std::cerr << "\nTEST FAILED: " << e.what() << std::endl;
		return 1;
	}

	std::cout << "\n==========================================" << std::endl;
	std::cout << "All scenarios PASSED (SC-001, SC-002, SC-003)" << std::endl;
	std::cout << "==========================================" << std::endl;
	return 0;
}
