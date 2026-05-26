// test/cpp/test_concurrent_reads.cpp
//
// Stress test for multi-threaded concurrent reads against a single attached
// MSSQL catalog. Reproduces the dbt-with-threads>=2 scenario reported as
// "segfaults the worker process mid-suite (exit 139, leaked semaphores)".
//
// Mirrors what dbt-duckdb does:
//   - One DuckDB instance shared across N worker threads.
//   - Each thread creates its OWN Connection (DuckDB Connection objects are
//     per-thread; the DatabaseInstance is shared).
//   - Threads issue read queries (`mssql_scan` AND catalog-bound
//     `SELECT * FROM mssql.dbo.t`) in parallel against the same ATTACHed
//     catalog. Pool, metadata cache, statistics provider, result-stream
//     registry, and the catalog itself are all shared.
//
// REQUIRES: SQL Server with `master` + `TestDB` (Docker test container is
// fine — the test only touches `sys.tables` which is always present).
//
// Env: MSSQL_TEST_HOST / MSSQL_TEST_PORT / MSSQL_TEST_USER / MSSQL_TEST_PASS
//      (skips with exit 0 if MSSQL_TEST_PASS is unset).
//
// Build + run via `make test-concurrent-reads`.

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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

	std::string Dsn(const std::string &db) const {
		std::ostringstream oss;
		oss << "Server=" << host << "," << port << ";Database=" << db << ";User Id=" << user << ";Password=" << pass;
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

// ---------------------------------------------------------------------------
// Worker: issues a mix of mssql_scan and catalog-bound queries in a loop.
// Aggregates first error per thread; tallies iteration count.
// ---------------------------------------------------------------------------
struct WorkerResult {
	int thread_id;
	int iterations_done;
	std::string first_error;	// empty = no error
};

void worker(DuckDB &db, int thread_id, int iterations, std::atomic<bool> &abort_flag, WorkerResult &out) {
	Connection conn(db);
	out.thread_id = thread_id;
	out.iterations_done = 0;

	for (int i = 0; i < iterations && !abort_flag.load(std::memory_order_relaxed); ++i) {
		// Alternate query shapes — both via mssql_scan to exercise the
		// RegisterStream / RetrieveStream UUID handoff under contention.
		// Different SQL strings prevent any DuckDB plan-cache reuse from
		// hiding races by serializing on a single cached plan.
		std::string sql;
		if (i % 2 == 0) {
			sql = "SELECT COUNT(*) FROM mssql_scan('mssql', 'SELECT TOP 50 name FROM sys.tables')";
		} else {
			sql = "SELECT COUNT(*) FROM mssql_scan('mssql', 'SELECT TOP " + std::to_string(10 + i % 30) +
			      " object_id FROM sys.objects')";
		}

		auto result = conn.Query(sql);
		if (result->HasError()) {
			out.first_error = "iter " + std::to_string(i) + ": " + result->GetError();
			abort_flag.store(true, std::memory_order_relaxed);
			return;
		}
		auto row_count = result->GetValue(0, 0).GetValue<int64_t>();
		if (row_count <= 0) {
			out.first_error = "iter " + std::to_string(i) + ": unexpected zero row count";
			abort_flag.store(true, std::memory_order_relaxed);
			return;
		}
		out.iterations_done = i + 1;
	}
}

// ---------------------------------------------------------------------------
// Scenario 1: N threads, single ATTACH, mixed mssql_scan + catalog reads.
// ---------------------------------------------------------------------------
bool scenario_concurrent_mixed_reads(const TestConfig &cfg, int num_threads, int iterations_per_thread) {
	std::cout << "\n=== Concurrent mixed reads: " << num_threads << " threads × " << iterations_per_thread
			  << " iters ===" << std::endl;

	DuckDB db(nullptr);
	{
		Connection setup(db);
		load_extension(setup);
		std::ostringstream attach;
		attach << "ATTACH '" << cfg.Dsn("master") << "' AS mssql (TYPE mssql)";
		auto r = setup.Query(attach.str());
		if (r->HasError()) {
			std::cerr << "  ATTACH failed: " << r->GetError() << std::endl;
			return false;
		}
	}

	std::atomic<bool> abort_flag(false);
	std::vector<std::thread> threads;
	std::vector<WorkerResult> results(num_threads);
	auto start = std::chrono::steady_clock::now();

	threads.reserve(num_threads);
	for (int t = 0; t < num_threads; ++t) {
		threads.emplace_back(worker, std::ref(db), t, iterations_per_thread, std::ref(abort_flag), std::ref(results[t]));
	}
	for (auto &th : threads) {
		th.join();
	}

	auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

	int total_done = 0;
	bool any_error = false;
	for (auto &r : results) {
		total_done += r.iterations_done;
		if (!r.first_error.empty()) {
			std::cerr << "  Thread " << r.thread_id << " ERROR after " << r.iterations_done << " iters: "
					  << r.first_error << std::endl;
			any_error = true;
		}
	}

	std::cout << "  Completed " << total_done << " / " << (num_threads * iterations_per_thread) << " iterations in "
			  << elapsed << " ms" << std::endl;

	if (any_error) {
		std::cerr << "  FAILED" << std::endl;
		return false;
	}
	std::cout << "  PASSED" << std::endl;
	return true;
}

// ---------------------------------------------------------------------------
// Scenario 2: N threads, EACH ATTACHing its own catalog (same DSN, different
// alias). Exercises the per-catalog pool ownership invariant from spec 047
// under concurrent setup.
// ---------------------------------------------------------------------------
bool scenario_concurrent_attach(const TestConfig &cfg, int num_threads) {
	std::cout << "\n=== Concurrent ATTACHes: " << num_threads << " threads, one ATTACH + N reads each ==="
			  << std::endl;

	DuckDB db(nullptr);
	{
		Connection setup(db);
		load_extension(setup);
	}

	std::atomic<bool> abort_flag(false);
	std::vector<std::thread> threads;
	std::vector<std::string> errors(num_threads);

	auto start = std::chrono::steady_clock::now();
	for (int t = 0; t < num_threads; ++t) {
		threads.emplace_back([&, t]() {
			Connection conn(db);
			std::string alias = "mssql_t" + std::to_string(t);
			std::ostringstream attach;
			attach << "ATTACH '" << cfg.Dsn("master") << "' AS " << alias << " (TYPE mssql)";
			auto r = conn.Query(attach.str());
			if (r->HasError()) {
				errors[t] = "ATTACH: " + r->GetError();
				abort_flag.store(true);
				return;
			}
			for (int i = 0; i < 10 && !abort_flag.load(); ++i) {
				std::string sql = "SELECT COUNT(*) FROM mssql_scan('" + alias +
				                  "', 'SELECT TOP 10 name FROM sys.tables')";
				auto r2 = conn.Query(sql);
				if (r2->HasError()) {
					errors[t] = "iter " + std::to_string(i) + ": " + r2->GetError();
					abort_flag.store(true);
					return;
				}
			}
		});
	}
	for (auto &th : threads) {
		th.join();
	}
	auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

	bool any_error = false;
	for (int t = 0; t < num_threads; ++t) {
		if (!errors[t].empty()) {
			std::cerr << "  Thread " << t << " ERROR: " << errors[t] << std::endl;
			any_error = true;
		}
	}
	std::cout << "  Elapsed " << elapsed << " ms" << std::endl;
	if (any_error) {
		std::cerr << "  FAILED" << std::endl;
		return false;
	}
	std::cout << "  PASSED" << std::endl;
	return true;
}

// ---------------------------------------------------------------------------
// Scenario 3: dbt-style — concurrent CATALOG-BOUND reads on a real table.
// Goes through MSSQLTableScan (projection/filter pushdown) instead of
// mssql_scan. This exercises the metadata cache + TableEntry / SchemaEntry
// concurrently across threads.
// ---------------------------------------------------------------------------
bool scenario_concurrent_catalog_reads(const TestConfig &cfg, int num_threads, int iterations_per_thread) {
	std::cout << "\n=== Concurrent CATALOG-bound reads: " << num_threads << " threads × " << iterations_per_thread
			  << " iters ===" << std::endl;

	DuckDB db(nullptr);
	{
		Connection setup(db);
		load_extension(setup);
		std::ostringstream attach;
		attach << "ATTACH '" << cfg.Dsn("TestDB") << "' AS mssql (TYPE mssql)";
		auto r = setup.Query(attach.str());
		if (r->HasError()) {
			std::cerr << "  ATTACH failed: " << r->GetError() << std::endl;
			return false;
		}
		// Drop + recreate test table with a few rows.
		setup.Query("SELECT mssql_exec('mssql', 'DROP TABLE IF EXISTS dbo.concurrent_read_test')");
		auto cr = setup.Query("SELECT mssql_exec('mssql', 'CREATE TABLE dbo.concurrent_read_test (id INT PRIMARY KEY, "
		                       "name NVARCHAR(100), v INT)')");
		if (cr->HasError()) {
			std::cerr << "  CREATE TABLE failed: " << cr->GetError() << std::endl;
			return false;
		}
		for (int i = 0; i < 100; ++i) {
			auto ins = setup.Query("SELECT mssql_exec('mssql', 'INSERT INTO dbo.concurrent_read_test VALUES (" +
			                       std::to_string(i) + ", N''row " + std::to_string(i) + "'', " +
			                       std::to_string(i * 7) + ")')");
			if (ins->HasError()) {
				std::cerr << "  INSERT failed: " << ins->GetError() << std::endl;
				return false;
			}
		}
		setup.Query("SELECT mssql_refresh_cache('mssql')");
	}

	std::atomic<bool> abort_flag(false);
	std::vector<std::thread> threads;
	std::vector<WorkerResult> results(num_threads);
	auto start = std::chrono::steady_clock::now();

	for (int t = 0; t < num_threads; ++t) {
		threads.emplace_back([&, t]() {
			Connection conn(db);
			results[t].thread_id = t;
			for (int i = 0; i < iterations_per_thread && !abort_flag.load(); ++i) {
				std::string sql;
				switch (i % 4) {
				case 0:
					sql = "SELECT COUNT(*) FROM mssql.dbo.concurrent_read_test";
					break;
				case 1:
					sql = "SELECT MAX(v) FROM mssql.dbo.concurrent_read_test WHERE id < " + std::to_string(50 + t);
					break;
				case 2:
					sql = "SELECT name FROM mssql.dbo.concurrent_read_test WHERE id = " + std::to_string(t % 100);
					break;
				case 3:
					sql = "SELECT id, v FROM mssql.dbo.concurrent_read_test WHERE v > " + std::to_string(i * 10) +
					      " ORDER BY id LIMIT 10";
					break;
				}
				auto r = conn.Query(sql);
				if (r->HasError()) {
					results[t].first_error = "iter " + std::to_string(i) + ": " + r->GetError();
					abort_flag.store(true);
					return;
				}
				results[t].iterations_done = i + 1;
			}
		});
	}
	for (auto &th : threads) {
		th.join();
	}
	auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

	int total_done = 0;
	bool any_error = false;
	for (auto &r : results) {
		total_done += r.iterations_done;
		if (!r.first_error.empty()) {
			std::cerr << "  Thread " << r.thread_id << " ERROR after " << r.iterations_done << " iters: "
			          << r.first_error << std::endl;
			any_error = true;
		}
	}

	// Cleanup
	{
		Connection cleanup(db);
		cleanup.Query("SELECT mssql_exec('mssql', 'DROP TABLE IF EXISTS dbo.concurrent_read_test')");
	}

	std::cout << "  Completed " << total_done << " / " << (num_threads * iterations_per_thread) << " iterations in "
			  << elapsed << " ms" << std::endl;
	if (any_error) {
		std::cerr << "  FAILED" << std::endl;
		return false;
	}
	std::cout << "  PASSED" << std::endl;
	return true;
}

// ---------------------------------------------------------------------------
// Scenario 5 (Spec 052 US2 SC-003): concurrent invalidation injection.
// N reader threads in a tight loop of catalog-bound SELECTs while a separate
// writer thread calls mssql_refresh_cache('mssql') every `invalidator_ms`.
// Pre-fix this UAFs on the first invalidation hit (entries_.clear() drops
// every outstanding raw pointer the reader binders hold). Post-fix the
// retired entries flow into MSSQLCatalog::table_graveyard_; binders survive
// to the end of their bind/execute. Must run for `duration_seconds` clean.
// ---------------------------------------------------------------------------
bool scenario_invalidation_race(const TestConfig &cfg, int num_readers, int duration_seconds, int invalidator_ms) {
	std::cout << "\n=== Concurrent invalidation race: " << num_readers << " readers + invalidator @ "
	          << invalidator_ms << "ms for " << duration_seconds << "s ===" << std::endl;

	DuckDB db(nullptr);
	{
		Connection setup(db);
		load_extension(setup);
		std::ostringstream attach;
		attach << "ATTACH '" << cfg.Dsn("TestDB") << "' AS mssql (TYPE mssql)";
		auto r = setup.Query(attach.str());
		if (r->HasError()) {
			std::cerr << "  ATTACH failed: " << r->GetError() << std::endl;
			return false;
		}
		setup.Query("SELECT mssql_exec('mssql', 'DROP TABLE IF EXISTS dbo.invalidation_race_test')");
		auto cr = setup.Query("SELECT mssql_exec('mssql', 'CREATE TABLE dbo.invalidation_race_test (id INT PRIMARY KEY, "
		                       "name NVARCHAR(100), v INT)')");
		if (cr->HasError()) {
			std::cerr << "  CREATE TABLE failed: " << cr->GetError() << std::endl;
			return false;
		}
		for (int i = 0; i < 100; ++i) {
			auto ins = setup.Query("SELECT mssql_exec('mssql', 'INSERT INTO dbo.invalidation_race_test VALUES (" +
			                       std::to_string(i) + ", N''row " + std::to_string(i) + "'', " +
			                       std::to_string(i * 7) + ")')");
			if (ins->HasError()) {
				std::cerr << "  INSERT failed: " << ins->GetError() << std::endl;
				return false;
			}
		}
		setup.Query("SELECT mssql_refresh_cache('mssql')");
	}

	std::atomic<bool> stop_flag(false);
	std::atomic<bool> abort_flag(false);
	std::vector<std::thread> threads;
	std::vector<WorkerResult> results(num_readers);
	std::atomic<int> invalidations(0);
	std::string invalidator_error;

	auto start = std::chrono::steady_clock::now();

	for (int t = 0; t < num_readers; ++t) {
		threads.emplace_back([&, t]() {
			Connection conn(db);
			results[t].thread_id = t;
			int i = 0;
			while (!stop_flag.load() && !abort_flag.load()) {
				std::string sql;
				switch (i % 4) {
				case 0:
					sql = "SELECT COUNT(*) FROM mssql.dbo.invalidation_race_test";
					break;
				case 1:
					sql = "SELECT MAX(v) FROM mssql.dbo.invalidation_race_test WHERE id < " + std::to_string(50 + t);
					break;
				case 2:
					sql = "SELECT name FROM mssql.dbo.invalidation_race_test WHERE id = " + std::to_string(i % 100);
					break;
				case 3:
					sql = "SELECT id, v FROM mssql.dbo.invalidation_race_test WHERE v > " + std::to_string((i * 10) % 700) +
					      " ORDER BY id LIMIT 10";
					break;
				}
				auto r = conn.Query(sql);
				if (r->HasError()) {
					results[t].first_error = "iter " + std::to_string(i) + ": " + r->GetError();
					abort_flag.store(true);
					return;
				}
				++i;
				results[t].iterations_done = i;
			}
		});
	}

	// Invalidator thread — fires mssql_refresh_cache every invalidator_ms.
	threads.emplace_back([&]() {
		Connection conn(db);
		while (!stop_flag.load() && !abort_flag.load()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(invalidator_ms));
			auto r = conn.Query("SELECT mssql_refresh_cache('mssql')");
			if (r->HasError()) {
				invalidator_error = r->GetError();
				abort_flag.store(true);
				return;
			}
			++invalidations;
		}
	});

	std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
	stop_flag.store(true);
	for (auto &th : threads) {
		th.join();
	}
	auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

	int total_done = 0;
	bool any_error = false;
	for (auto &r : results) {
		total_done += r.iterations_done;
		if (!r.first_error.empty()) {
			std::cerr << "  Reader " << r.thread_id << " ERROR after " << r.iterations_done << " iters: "
			          << r.first_error << std::endl;
			any_error = true;
		}
	}
	if (!invalidator_error.empty()) {
		std::cerr << "  Invalidator ERROR: " << invalidator_error << std::endl;
		any_error = true;
	}

	// Cleanup
	{
		Connection cleanup(db);
		cleanup.Query("SELECT mssql_exec('mssql', 'DROP TABLE IF EXISTS dbo.invalidation_race_test')");
	}

	std::cout << "  Readers completed " << total_done << " queries; " << invalidations.load() << " invalidations injected; "
	          << elapsed << " ms total" << std::endl;
	if (any_error) {
		std::cerr << "  FAILED" << std::endl;
		return false;
	}
	std::cout << "  PASSED" << std::endl;
	return true;
}

// ---------------------------------------------------------------------------
// Scenario 6 (Spec 052 US3 SC-004): sibling-cache stress.
// Scenario 5 (readers + invalidator) PLUS a sixth thread that loops over
// duckdb_schemas() / duckdb_tables() — these exercise MSSQLCatalog::
// schema_entries_, MSSQLMetadataCache::GetTableMetadata, and the schema-
// level invalidation paths. If any sibling cache still hands out raw
// pointers under concurrent invalidation, this scenario UAFs.
// ---------------------------------------------------------------------------
bool scenario_sibling_cache_stress(const TestConfig &cfg, int num_readers, int duration_seconds, int invalidator_ms) {
	std::cout << "\n=== Sibling-cache stress: " << num_readers << " readers + invalidator + schema-walker for "
	          << duration_seconds << "s ===" << std::endl;

	DuckDB db(nullptr);
	{
		Connection setup(db);
		load_extension(setup);
		std::ostringstream attach;
		attach << "ATTACH '" << cfg.Dsn("TestDB") << "' AS mssql (TYPE mssql)";
		auto r = setup.Query(attach.str());
		if (r->HasError()) {
			std::cerr << "  ATTACH failed: " << r->GetError() << std::endl;
			return false;
		}
		setup.Query("SELECT mssql_exec('mssql', 'DROP TABLE IF EXISTS dbo.sibling_cache_test')");
		auto cr = setup.Query("SELECT mssql_exec('mssql', 'CREATE TABLE dbo.sibling_cache_test (id INT PRIMARY KEY, "
		                       "name NVARCHAR(100), v INT)')");
		if (cr->HasError()) {
			std::cerr << "  CREATE TABLE failed: " << cr->GetError() << std::endl;
			return false;
		}
		for (int i = 0; i < 50; ++i) {
			auto ins = setup.Query("SELECT mssql_exec('mssql', 'INSERT INTO dbo.sibling_cache_test VALUES (" +
			                       std::to_string(i) + ", N''row " + std::to_string(i) + "'', " +
			                       std::to_string(i * 7) + ")')");
			if (ins->HasError()) {
				std::cerr << "  INSERT failed: " << ins->GetError() << std::endl;
				return false;
			}
		}
		setup.Query("SELECT mssql_refresh_cache('mssql')");
	}

	std::atomic<bool> stop_flag(false);
	std::atomic<bool> abort_flag(false);
	std::vector<std::thread> threads;
	std::vector<WorkerResult> results(num_readers);
	std::atomic<int> invalidations(0);
	std::atomic<int> schema_walks(0);
	std::string invalidator_error;
	std::string walker_error;

	auto start = std::chrono::steady_clock::now();

	// Reader threads.
	for (int t = 0; t < num_readers; ++t) {
		threads.emplace_back([&, t]() {
			Connection conn(db);
			results[t].thread_id = t;
			int i = 0;
			while (!stop_flag.load() && !abort_flag.load()) {
				std::string sql = "SELECT id, name, v FROM mssql.dbo.sibling_cache_test WHERE id < " +
				                  std::to_string(20 + (i % 10)) + " ORDER BY id";
				auto r = conn.Query(sql);
				if (r->HasError()) {
					results[t].first_error = "iter " + std::to_string(i) + ": " + r->GetError();
					abort_flag.store(true);
					return;
				}
				++i;
				results[t].iterations_done = i;
			}
		});
	}

	// Invalidator thread.
	threads.emplace_back([&]() {
		Connection conn(db);
		while (!stop_flag.load() && !abort_flag.load()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(invalidator_ms));
			auto r = conn.Query("SELECT mssql_refresh_cache('mssql')");
			if (r->HasError()) {
				invalidator_error = r->GetError();
				abort_flag.store(true);
				return;
			}
			++invalidations;
		}
	});

	// Schema-walker thread — exercises MSSQLCatalog::schema_entries_,
	// MSSQLMetadataCache::GetTableNames, and the schema-level cache paths.
	threads.emplace_back([&]() {
		Connection conn(db);
		int i = 0;
		while (!stop_flag.load() && !abort_flag.load()) {
			auto kind = i % 2;
			auto r = (kind == 0) ? conn.Query("SELECT database_name, schema_name FROM duckdb_schemas() WHERE database_name = 'mssql'")
			                     : conn.Query("SELECT database_name, schema_name, table_name FROM duckdb_tables() WHERE database_name = 'mssql' LIMIT 100");
			if (r->HasError()) {
				walker_error = "iter " + std::to_string(i) + ": " + r->GetError();
				abort_flag.store(true);
				return;
			}
			++i;
			++schema_walks;
		}
	});

	std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
	stop_flag.store(true);
	for (auto &th : threads) {
		th.join();
	}
	auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

	int total_done = 0;
	bool any_error = false;
	for (auto &r : results) {
		total_done += r.iterations_done;
		if (!r.first_error.empty()) {
			std::cerr << "  Reader " << r.thread_id << " ERROR after " << r.iterations_done << " iters: "
			          << r.first_error << std::endl;
			any_error = true;
		}
	}
	if (!invalidator_error.empty()) {
		std::cerr << "  Invalidator ERROR: " << invalidator_error << std::endl;
		any_error = true;
	}
	if (!walker_error.empty()) {
		std::cerr << "  Walker ERROR: " << walker_error << std::endl;
		any_error = true;
	}

	// Cleanup
	{
		Connection cleanup(db);
		cleanup.Query("SELECT mssql_exec('mssql', 'DROP TABLE IF EXISTS dbo.sibling_cache_test')");
	}

	std::cout << "  Readers completed " << total_done << " queries; " << invalidations.load()
	          << " invalidations; " << schema_walks.load() << " schema walks; " << elapsed << " ms total" << std::endl;
	if (any_error) {
		std::cerr << "  FAILED" << std::endl;
		return false;
	}
	std::cout << "  PASSED" << std::endl;
	return true;
}

// ---------------------------------------------------------------------------
// Scenario 7 (Spec 052): concurrent writes into a SHARED table.
// N writer threads + 1 reader thread, all hitting dbo.write_race_shared.
// Writers cycle catalog-bound INSERT → UPDATE → SELECT → DELETE on disjoint
// PK ranges (thread t uses ids [t*1000, t*1000+999]) so SQL Server's
// row-level locks don't deadlock between writers. Reader does
// SELECT COUNT(*) over the full table.
//
// What's stressed beyond scenario 5/6: the SAME MSSQLTableEntry pointer is
// reached concurrently from N+1 binders simultaneously, each going through
// LookupSchema → LookupEntry → GetScanFunction (read) or DML binder path
// (writes). Concurrent shared_from_this() on the same entry, concurrent
// EnsurePKLoaded (pk_load_mutex_ from US2), and any race in the catalog
// integration layer between writes and reads would surface here.
// ---------------------------------------------------------------------------
bool scenario_concurrent_writes_shared(const TestConfig &cfg, int num_writers, int duration_seconds) {
	std::cout << "\n=== Concurrent writes to ONE table: " << num_writers
	          << " writers + 1 reader for " << duration_seconds << "s ===" << std::endl;

	DuckDB db(nullptr);
	{
		Connection setup(db);
		load_extension(setup);
		std::ostringstream attach;
		attach << "ATTACH '" << cfg.Dsn("TestDB") << "' AS mssql (TYPE mssql)";
		auto r = setup.Query(attach.str());
		if (r->HasError()) {
			std::cerr << "  ATTACH failed: " << r->GetError() << std::endl;
			return false;
		}
		setup.Query("SELECT mssql_exec('mssql', 'DROP TABLE IF EXISTS dbo.write_race_shared')");
		auto cr = setup.Query(
		    "SELECT mssql_exec('mssql', 'CREATE TABLE dbo.write_race_shared (id INT PRIMARY KEY, v INT, name NVARCHAR(64))')");
		if (cr->HasError()) {
			std::cerr << "  CREATE shared table failed: " << cr->GetError() << std::endl;
			return false;
		}
		// Pre-populate so the reader always sees rows even if writers are mid-cycle.
		for (int i = 0; i < 100; ++i) {
			setup.Query("SELECT mssql_exec('mssql', 'INSERT INTO dbo.write_race_shared VALUES (" +
			            std::to_string(i) + ", " + std::to_string(i * 3) + ", N''seed " + std::to_string(i) +
			            "'')')");
		}
		setup.Query("SELECT mssql_refresh_cache('mssql')");
	}

	std::atomic<bool> stop_flag(false);
	std::atomic<bool> abort_flag(false);
	std::vector<std::thread> threads;
	std::vector<WorkerResult> results(num_writers);
	std::atomic<int> reader_iters(0);
	std::atomic<int> total_inserts(0);
	std::atomic<int> total_updates(0);
	std::atomic<int> total_deletes(0);
	std::string reader_error;

	auto start = std::chrono::steady_clock::now();

	for (int t = 0; t < num_writers; ++t) {
		threads.emplace_back([&, t]() {
			Connection conn(db);
			results[t].thread_id = t;
			// Each writer owns PK range [base, base + 999].
			int base = 1000 + t * 1000;
			int i = 0;
			while (!stop_flag.load() && !abort_flag.load()) {
				int row_id = base + (i % 100);  // recycle 100 IDs per writer
				auto step = [&](const std::string &label, const std::string &sql) -> bool {
					auto r = conn.Query(sql);
					if (r->HasError()) {
						results[t].first_error = label + " iter " + std::to_string(i) + ": " + r->GetError();
						abort_flag.store(true);
						return false;
					}
					return true;
				};
				// INSERT — the DELETE at end of previous cycle removed any prior row
				// at this id. If the very first cycle's id collides with a seeded row
				// it'd PK-fail, but base ids are 1000+ so no collision with seeds (0-99).
				if (!step("INSERT",
				          "INSERT INTO mssql.dbo.write_race_shared (id, v, name) VALUES (" +
				              std::to_string(row_id) + ", " + std::to_string(i) + ", 'writer_" +
				              std::to_string(t) + "_" + std::to_string(i) + "')")) {
					return;
				}
				++total_inserts;
				// UPDATE
				if (!step("UPDATE", "UPDATE mssql.dbo.write_race_shared SET v = v + 1 WHERE id = " +
				                        std::to_string(row_id))) {
					return;
				}
				++total_updates;
				// SELECT (catalog-bound read of our own row)
				if (!step("SELECT", "SELECT id, v FROM mssql.dbo.write_race_shared WHERE id = " +
				                        std::to_string(row_id))) {
					return;
				}
				// DELETE
				if (!step("DELETE", "DELETE FROM mssql.dbo.write_race_shared WHERE id = " + std::to_string(row_id))) {
					return;
				}
				++total_deletes;
				++i;
				results[t].iterations_done = i;
			}
		});
	}

	// Reader thread.
	threads.emplace_back([&]() {
		Connection conn(db);
		int i = 0;
		while (!stop_flag.load() && !abort_flag.load()) {
			auto r = conn.Query("SELECT COUNT(*), MAX(v) FROM mssql.dbo.write_race_shared");
			if (r->HasError()) {
				reader_error = "iter " + std::to_string(i) + ": " + r->GetError();
				abort_flag.store(true);
				return;
			}
			++i;
			reader_iters = i;
		}
	});

	std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
	stop_flag.store(true);
	for (auto &th : threads) {
		th.join();
	}
	auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

	bool any_error = false;
	int total_writer_iters = 0;
	for (auto &r : results) {
		total_writer_iters += r.iterations_done;
		if (!r.first_error.empty()) {
			std::cerr << "  Writer " << r.thread_id << " ERROR after " << r.iterations_done << " cycles: "
			          << r.first_error << std::endl;
			any_error = true;
		}
	}
	if (!reader_error.empty()) {
		std::cerr << "  Reader ERROR: " << reader_error << std::endl;
		any_error = true;
	}

	// Cleanup
	{
		Connection cleanup(db);
		cleanup.Query("SELECT mssql_exec('mssql', 'DROP TABLE IF EXISTS dbo.write_race_shared')");
	}

	std::cout << "  Writers: " << total_writer_iters << " complete cycles ("
	          << total_inserts.load() << " INSERT, " << total_updates.load() << " UPDATE, "
	          << total_deletes.load() << " DELETE); reader: " << reader_iters.load() << " iters; "
	          << elapsed << " ms total" << std::endl;
	if (any_error) {
		std::cerr << "  FAILED" << std::endl;
		return false;
	}
	std::cout << "  PASSED" << std::endl;
	return true;
}

// ---------------------------------------------------------------------------
// Scenario 8 (Spec 052): pure-write contention on a SHARED table.
// N writer threads, no reader. All threads INSERT into dbo.write_only_shared
// concurrently using disjoint PK ranges (no SQL Server row-lock conflict).
// Pure DML pipeline stress: catalog-bound INSERT bind path runs concurrently
// on the same MSSQLTableEntry across all threads; shared_from_this() racing
// on the same entry; EnsurePKLoaded races (each binder triggers PK discovery
// the first time, pk_load_mutex_ must serialise them).
// ---------------------------------------------------------------------------
bool scenario_pure_concurrent_writes(const TestConfig &cfg, int num_writers, int duration_seconds) {
	std::cout << "\n=== Pure concurrent writes to ONE table: " << num_writers
	          << " writers for " << duration_seconds << "s ===" << std::endl;

	DuckDB db(nullptr);
	{
		Connection setup(db);
		load_extension(setup);
		std::ostringstream attach;
		attach << "ATTACH '" << cfg.Dsn("TestDB") << "' AS mssql (TYPE mssql)";
		auto r = setup.Query(attach.str());
		if (r->HasError()) {
			std::cerr << "  ATTACH failed: " << r->GetError() << std::endl;
			return false;
		}
		setup.Query("SELECT mssql_exec('mssql', 'DROP TABLE IF EXISTS dbo.write_only_shared')");
		auto cr = setup.Query(
		    "SELECT mssql_exec('mssql', 'CREATE TABLE dbo.write_only_shared (id BIGINT PRIMARY KEY, v INT, "
		    "name NVARCHAR(64))')");
		if (cr->HasError()) {
			std::cerr << "  CREATE failed: " << cr->GetError() << std::endl;
			return false;
		}
		setup.Query("SELECT mssql_refresh_cache('mssql')");
	}

	std::atomic<bool> stop_flag(false);
	std::atomic<bool> abort_flag(false);
	std::vector<std::thread> threads;
	std::vector<WorkerResult> results(num_writers);
	std::atomic<int64_t> total_inserts(0);

	auto start = std::chrono::steady_clock::now();

	for (int t = 0; t < num_writers; ++t) {
		threads.emplace_back([&, t]() {
			Connection conn(db);
			results[t].thread_id = t;
			// Disjoint id ranges per writer: [t*1_000_000, ...) so no PK conflict.
			int64_t base = static_cast<int64_t>(t) * 1000000LL;
			int64_t offset = 0;
			while (!stop_flag.load() && !abort_flag.load()) {
				int64_t row_id = base + offset;
				std::string sql = "INSERT INTO mssql.dbo.write_only_shared (id, v, name) VALUES (" +
				                  std::to_string(row_id) + ", " + std::to_string(offset) + ", 'w" +
				                  std::to_string(t) + "_" + std::to_string(offset) + "')";
				auto r = conn.Query(sql);
				if (r->HasError()) {
					results[t].first_error = "iter " + std::to_string(offset) + ": " + r->GetError();
					abort_flag.store(true);
					return;
				}
				++offset;
				++total_inserts;
				results[t].iterations_done = static_cast<int>(offset);
			}
		});
	}

	std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
	stop_flag.store(true);
	for (auto &th : threads) {
		th.join();
	}
	auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

	bool any_error = false;
	for (auto &r : results) {
		if (!r.first_error.empty()) {
			std::cerr << "  Writer " << r.thread_id << " ERROR after " << r.iterations_done << ": "
			          << r.first_error << std::endl;
			any_error = true;
		}
	}

	// Cleanup
	{
		Connection cleanup(db);
		cleanup.Query("SELECT mssql_exec('mssql', 'DROP TABLE IF EXISTS dbo.write_only_shared')");
	}

	std::cout << "  Total INSERTs: " << total_inserts.load() << " across " << num_writers
	          << " threads; " << elapsed << " ms total ("
	          << (total_inserts.load() * 1000 / std::max<int64_t>(elapsed, 1)) << " inserts/sec)" << std::endl;
	if (any_error) {
		std::cerr << "  FAILED" << std::endl;
		return false;
	}
	std::cout << "  PASSED" << std::endl;
	return true;
}

}  // namespace

int main() {
	std::cout << "==========================================" << std::endl;
	std::cout << "Concurrent reads stress test" << std::endl;
	std::cout << "(dbt threads>=2 scenario reproduction)" << std::endl;
	std::cout << "==========================================" << std::endl;

	auto cfg = TestConfig::FromEnv();
	if (!cfg.IsValid()) {
		std::cerr << "\nSKIPPED: MSSQL_TEST_PASS is not set." << std::endl;
		return 0;
	}

	std::cout << "\nConnection: " << cfg.user << "@" << cfg.host << ":" << cfg.port << std::endl;

	bool ok = true;
	try {
		// dbt typically defaults to 4 worker threads. Stress further with 8.
		ok &= scenario_concurrent_mixed_reads(cfg, 4, 50);
		ok &= scenario_concurrent_mixed_reads(cfg, 8, 25);
		ok &= scenario_concurrent_attach(cfg, 4);
		ok &= scenario_concurrent_catalog_reads(cfg, 4, 50);
		ok &= scenario_concurrent_catalog_reads(cfg, 8, 25);
		// Scenario 5 (spec 052 US2): 4 readers + invalidator at 50ms cadence
		// for 30 seconds. Reduce duration on CI via INVALIDATION_RACE_SECS env.
		int sec = std::getenv("INVALIDATION_RACE_SECS") ? std::atoi(std::getenv("INVALIDATION_RACE_SECS")) : 30;
		ok &= scenario_invalidation_race(cfg, 4, sec, 50);
		// Scenario 6 (spec 052 US3): scenario 5 + schema walker thread.
		ok &= scenario_sibling_cache_stress(cfg, 4, sec, 50);
		// Scenario 7 (spec 052): concurrent INSERT/UPDATE/SELECT/DELETE on a
		// shared table with disjoint PK ranges + a reader thread on same table.
		ok &= scenario_concurrent_writes_shared(cfg, 4, sec);
		// Scenario 8 (spec 052): pure concurrent INSERTs into one table.
		ok &= scenario_pure_concurrent_writes(cfg, 4, sec);
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
	std::cout << "All concurrent-reads scenarios PASSED" << std::endl;
	std::cout << "==========================================" << std::endl;
	return 0;
}
