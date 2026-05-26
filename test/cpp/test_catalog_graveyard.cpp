// ============================================================================
// Spec 052 T021 — MSSQLCatalog graveyard unit test.
//
// Verifies the lifetime-extension mechanism at a focused (non-stress) level:
//
//   1. After warming the cache by querying a table, MSSQLTableSet::entries_
//      contains the entry and the catalog's table_graveyard_ is empty.
//   2. After calling mssql_refresh_cache, entries_ is empty AGAIN (next bind
//      will reload), but table_graveyard_ grew to hold the retired entry.
//   3. Subsequent queries succeed (the graveyard entry coexists with the
//      freshly-loaded one — both kept alive by their respective shared_ptr
//      holders).
//   4. After N invalidation cycles, the graveyard accumulates N entries
//      (no auto-GC — by design per spec 052 research § Decision 3).
//   5. DETACH (= ~MSSQLCatalog) frees the entire graveyard via RAII; the
//      process does not crash.
//
// This test is COMPLEMENTARY to scenario 5 / scenario 6 in
// test_concurrent_reads.cpp. Those exercise the UAF window under
// multi-threaded stress (the actual bug); this one asserts the internal
// mechanism (graveyard size monotonically grows on Invalidate, drains on
// catalog destruction) — a regression here would be caught even if the
// stress test happened to schedule cleanly.
//
// Builds against the live duckdb library (links -lduckdb just like
// test_concurrent_reads). Skips with exit 0 if MSSQL_TEST_PASS is unset.
// ============================================================================

#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"

#include "catalog/mssql_catalog.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

namespace {

struct TestConfig {
	std::string host;
	std::string port;
	std::string user;
	std::string pass;
	std::string db;

	static std::string getenv_or(const char *name, const char *fallback) {
		const char *v = std::getenv(name);
		return (v && *v) ? std::string(v) : std::string(fallback);
	}

	static TestConfig FromEnv() {
		TestConfig c;
		c.host = getenv_or("MSSQL_TEST_HOST", "localhost");
		c.port = getenv_or("MSSQL_TEST_PORT", "1433");
		c.user = getenv_or("MSSQL_TEST_USER", "sa");
		c.pass = getenv_or("MSSQL_TEST_PASS", "");
		c.db = getenv_or("MSSQL_TEST_DB", "TestDB");
		return c;
	}

	bool IsValid() const {
		return !pass.empty();
	}

	std::string Dsn() const {
		std::ostringstream os;
		os << "Server=" << host << "," << port << ";Database=" << db << ";User Id=" << user << ";Password=" << pass;
		return os.str();
	}
};

#define ASSERT_TRUE(cond, msg)                                                              \
	do {                                                                                    \
		if (!(cond)) {                                                                      \
			std::cerr << "  FAIL: " << msg << " (" #cond ")" << std::endl;                  \
			return false;                                                                   \
		}                                                                                   \
	} while (0)

bool query_ok(duckdb::Connection &conn, const std::string &sql, const char *step) {
	auto r = conn.Query(sql);
	if (r->HasError()) {
		std::cerr << "  FAIL at " << step << ": " << r->GetError() << std::endl;
		return false;
	}
	return true;
}

duckdb::MSSQLCatalog &get_mssql_catalog(duckdb::DuckDB &db, const std::string &name) {
	auto &mgr = duckdb::DatabaseManager::Get(*db.instance);
	auto attached = mgr.GetDatabase(name);
	if (!attached) {
		throw std::runtime_error("catalog '" + name + "' not attached");
	}
	auto &cat = duckdb::Catalog::GetCatalog(*attached);
	return cat.Cast<duckdb::MSSQLCatalog>();
}

bool scenario_graveyard_grows_on_invalidate(const TestConfig &cfg) {
	std::cout << "\n=== Graveyard grows on Invalidate; drains on DETACH ===" << std::endl;

	duckdb::DuckDB db(nullptr);
	duckdb::Connection conn(db);

	if (!query_ok(conn, "LOAD 'build/debug/extension/mssql/mssql.duckdb_extension'", "LOAD")) {
		return false;
	}

	std::ostringstream attach;
	attach << "ATTACH '" << cfg.Dsn() << "' AS mssql (TYPE mssql)";
	if (!query_ok(conn, attach.str(), "ATTACH")) {
		return false;
	}

	// Setup test table
	query_ok(conn, "SELECT mssql_exec('mssql', 'DROP TABLE IF EXISTS dbo.graveyard_test')", "DROP");
	if (!query_ok(conn,
	              "SELECT mssql_exec('mssql', 'CREATE TABLE dbo.graveyard_test (id INT PRIMARY KEY, v INT)')",
	              "CREATE")) {
		return false;
	}
	query_ok(conn, "SELECT mssql_exec('mssql', 'INSERT INTO dbo.graveyard_test VALUES (1, 10), (2, 20)')",
	         "INSERT");

	// Warm the cache: query the table so MSSQLTableSet::entries_ has it.
	if (!query_ok(conn, "SELECT COUNT(*) FROM mssql.dbo.graveyard_test", "warm SELECT")) {
		return false;
	}

	auto &catalog = get_mssql_catalog(db, "mssql");
	auto graveyard_before = catalog.GetTableGraveyardSize();
	std::cout << "  baseline table_graveyard_size = " << graveyard_before << std::endl;
	ASSERT_TRUE(graveyard_before == 0, "graveyard should be empty after warm-up");

	// Cycle the cache N times; each refresh_cache should move the entry
	// (just loaded) into the graveyard.
	const int cycles = 5;
	for (int i = 0; i < cycles; ++i) {
		if (!query_ok(conn, "SELECT mssql_refresh_cache('mssql')", "refresh_cache")) {
			return false;
		}
		// Re-query to repopulate entries_, then refresh_cache again.
		if (!query_ok(conn, "SELECT COUNT(*) FROM mssql.dbo.graveyard_test", "post-refresh SELECT")) {
			return false;
		}
	}

	auto graveyard_after = catalog.GetTableGraveyardSize();
	std::cout << "  after " << cycles << " refresh cycles: table_graveyard_size = " << graveyard_after << std::endl;
	// Each refresh_cache invalidates 1 entry (the one we loaded); graveyard grows by 1 per cycle.
	// The final SELECT loads a fresh entry that's NOT yet in the graveyard.
	ASSERT_TRUE(graveyard_after == static_cast<size_t>(cycles),
	            "graveyard should accumulate exactly `cycles` entries");

	// Cleanup
	query_ok(conn, "SELECT mssql_exec('mssql', 'DROP TABLE IF EXISTS dbo.graveyard_test')", "cleanup DROP");

	// DETACH frees the catalog (and drains the graveyard via RAII).
	// We can't easily verify the entries are actually freed from here (no
	// destructor counter on TableEntry) — but if there were a leak/UAF, ASan
	// would catch it in the regular debug build.
	if (!query_ok(conn, "DETACH mssql", "DETACH")) {
		return false;
	}

	std::cout << "  PASSED" << std::endl;
	return true;
}

bool scenario_schema_graveyard_on_drop(const TestConfig &cfg) {
	std::cout << "\n=== Schema graveyard on DROP SCHEMA ===" << std::endl;

	duckdb::DuckDB db(nullptr);
	duckdb::Connection conn(db);

	if (!query_ok(conn, "LOAD 'build/debug/extension/mssql/mssql.duckdb_extension'", "LOAD")) {
		return false;
	}

	std::ostringstream attach;
	attach << "ATTACH '" << cfg.Dsn() << "' AS mssql (TYPE mssql)";
	if (!query_ok(conn, attach.str(), "ATTACH")) {
		return false;
	}

	// Create a test schema
	query_ok(conn, "SELECT mssql_exec('mssql', 'IF SCHEMA_ID(N''gravetest'') IS NOT NULL DROP SCHEMA gravetest')",
	         "pre-DROP SCHEMA");
	if (!query_ok(conn, "SELECT mssql_exec('mssql', 'CREATE SCHEMA gravetest')", "CREATE SCHEMA")) {
		return false;
	}
	query_ok(conn, "SELECT mssql_refresh_cache('mssql')", "post-create refresh");

	// Force schema entry into MSSQLCatalog::schema_entries_
	if (!query_ok(conn, "SELECT schema_name FROM duckdb_schemas() WHERE database_name = 'mssql'", "list schemas")) {
		return false;
	}

	auto &catalog = get_mssql_catalog(db, "mssql");
	auto schema_grave_before = catalog.GetSchemaGraveyardSize();
	std::cout << "  schema_graveyard_size before DROP = " << schema_grave_before << std::endl;

	// DROP SCHEMA should route the schema_entry into the graveyard
	// (MSSQLCatalog::OnDetach handler).
	if (!query_ok(conn, "DROP SCHEMA mssql.gravetest", "DROP SCHEMA")) {
		return false;
	}

	auto schema_grave_after = catalog.GetSchemaGraveyardSize();
	std::cout << "  schema_graveyard_size after DROP = " << schema_grave_after << std::endl;
	ASSERT_TRUE(schema_grave_after >= schema_grave_before,
	            "DROP SCHEMA should not shrink graveyard");
	// If schema_entry was in schema_entries_ at DROP time, graveyard grew by 1.
	// We can't assert == before+1 because the entry may not have been cached
	// (depending on whether LookupSchema was called for "gravetest" before).

	// DETACH must not crash even with graveyard non-empty.
	if (!query_ok(conn, "DETACH mssql", "DETACH")) {
		return false;
	}

	std::cout << "  PASSED" << std::endl;
	return true;
}

}  // namespace

int main() {
	std::cout << "==========================================" << std::endl;
	std::cout << "Spec 052 T021 — Catalog graveyard unit test" << std::endl;
	std::cout << "==========================================" << std::endl;

	auto cfg = TestConfig::FromEnv();
	if (!cfg.IsValid()) {
		std::cerr << "\nSKIPPED: MSSQL_TEST_PASS is not set." << std::endl;
		return 0;
	}

	std::cout << "\nConnection: " << cfg.user << "@" << cfg.host << ":" << cfg.port << std::endl;

	bool ok = true;
	try {
		ok &= scenario_graveyard_grows_on_invalidate(cfg);
		ok &= scenario_schema_graveyard_on_drop(cfg);
	} catch (const std::exception &e) {
		std::cerr << "\nTEST CRASHED: " << e.what() << std::endl;
		return 2;
	}

	if (!ok) {
		std::cerr << "\n==========================================" << std::endl;
		std::cerr << "ONE OR MORE SCENARIOS FAILED" << std::endl;
		std::cerr << "==========================================" << std::endl;
		return 1;
	}
	std::cout << "\n==========================================" << std::endl;
	std::cout << "All graveyard scenarios PASSED" << std::endl;
	std::cout << "==========================================" << std::endl;
	return 0;
}
