// Review of #539: a metadata load that waits on another session's lock stops when the
// statement is interrupted.
//
// A load reads the catalog under LOCK_TIMEOUT 0 and, on a row another session's uncommitted
// DDL holds, reads again until mssql_metadata_timeout. At 0, which means no limit, nothing but
// the user ends that wait, so the retry's pauses ask ClientContext::IsInterrupted (the context
// is passed down every load). Connection::Interrupt() is what Ctrl-C calls.
//
// One connection holds an uncommitted CREATE TABLE; the other lists the catalog, which reads
// every object and so meets the held row, in autocommit and in a transaction. An interrupter
// fires after 2 s; a watchdog releases the hold after 30 s, so a regression fails here
// instead of hanging the job.
//
// Skips (exit 0) when MSSQL_TEST_PASS is unset, like the other live C++ tests.

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "duckdb.hpp"
#include "mssql_extension.hpp"

using namespace duckdb;

namespace {

std::string getenv_or(const char *name, const char *fallback) {
	const char *v = std::getenv(name);
	return v && *v ? v : fallback;
}

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

int64_t Scalar(Connection &conn, const std::string &sql) {
	auto result = conn.Query(sql);
	if (result->HasError()) {
		std::cerr << "query failed: " << result->GetError().substr(0, 300) << std::endl;
		return -1;
	}
	auto chunk = result->Fetch();
	if (!chunk || chunk->size() == 0) {
		return -1;
	}
	return chunk->GetValue(0, 0).GetValue<int64_t>();
}

bool Run(Connection &conn, const std::string &sql) {
	auto result = conn.Query(sql);
	if (result->HasError()) {
		std::cerr << "query failed: " << result->GetError().substr(0, 300) << std::endl;
		return false;
	}
	return true;
}

std::string Dsn() {
	return "Server=" + getenv_or("MSSQL_TEST_HOST", "localhost") + "," + getenv_or("MSSQL_TEST_PORT", "1433") +
		   ";Database=TestDB;User Id=" + getenv_or("MSSQL_TEST_USER", "sa") +
		   ";Password=" + getenv_or("MSSQL_TEST_PASS", "") + ";TrustServerCertificate=yes";
}

//! The listing under the hold, interrupted after 2 s. `label` names the case.
void InterruptedListing(Connection &conn, Connection &hold, const std::string &catalog, const std::string &label) {
	std::mutex m;
	std::condition_variable cv;
	bool finished = false;
	bool watchdog_fired = false;
	std::thread interrupter([&]() {
		std::unique_lock<std::mutex> lock(m);
		if (!cv.wait_for(lock, std::chrono::seconds(2), [&]() { return finished; })) {
			conn.Interrupt();
		}
	});
	std::thread watchdog([&]() {
		std::unique_lock<std::mutex> lock(m);
		if (!cv.wait_for(lock, std::chrono::seconds(30), [&]() { return finished; })) {
			watchdog_fired = true;
			Run(hold, "ROLLBACK");
		}
	});
	const auto started = std::chrono::steady_clock::now();
	auto result = conn.Query("SELECT count(*) FROM duckdb_tables() WHERE database_name = '" + catalog + "'");
	const double seconds =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() /
		1000.0;
	{
		std::lock_guard<std::mutex> lock(m);
		finished = true;
	}
	cv.notify_all();
	interrupter.join();
	watchdog.join();

	Check(result->HasError(), label + ": the listing fails");
	Check(result->HasError() && result->GetError().find("Interrupt") != std::string::npos, label + ": as interrupted");
	Check(!watchdog_fired && seconds < 10,
		  label + ": soon after the interrupt, not when the hold ends (" + std::to_string(seconds) + " s)");
	if (result->HasError()) {
		std::cout << "       " << result->GetError().substr(0, 160) << std::endl;
	}
}

}  // namespace

int main() {
	if (getenv_or("MSSQL_TEST_PASS", "").empty()) {
		std::cout << "MSSQL_TEST_PASS unset: skipping the metadata interrupt test" << std::endl;
		return 0;
	}
	DuckDB db(nullptr);
	db.LoadStaticExtension<MssqlExtension>();
	Connection hold(db);
	Connection conn(db);

	if (!Run(hold, "ATTACH '" + Dsn() + "' AS mi_hold (TYPE mssql)") ||
		!Run(hold, "SELECT mssql_exec('mi_hold', 'DROP TABLE IF EXISTS dbo.mi_held, dbo.mi_seen')") ||
		// The concurrency job's TestDB is created empty: the listing at the end looks for this one.
		!Run(hold, "SELECT mssql_exec('mi_hold', 'CREATE TABLE dbo.mi_seen (id int)')") || !Run(hold, "BEGIN") ||
		!Run(hold, "SELECT mssql_exec('mi_hold', 'CREATE TABLE dbo.mi_held (id int)')")) {
		std::cout << "FAIL: could not set up the hold" << std::endl;
		return 1;
	}
	Run(conn, "SET mssql_metadata_timeout = 0");

	std::cout << "[1] autocommit" << std::endl;
	Run(conn, "ATTACH '" + Dsn() + "' AS mi_auto (TYPE mssql)");
	InterruptedListing(conn, hold, "mi_auto", "autocommit");
	Run(conn, "DETACH mi_auto");

	std::cout << "[2] in a transaction, on its pinned connection" << std::endl;
	Run(conn, "ATTACH '" + Dsn() + "' AS mi_tx (TYPE mssql)");
	Run(conn, "BEGIN");
	Run(conn, "SELECT * FROM mssql_scan('mi_tx', 'SELECT 1 AS one')");
	InterruptedListing(conn, hold, "mi_tx", "transaction");
	Check(Run(conn, "ROLLBACK"), "transaction: ROLLBACK after the interrupt");

	Run(hold, "ROLLBACK");
	Run(conn, "RESET mssql_metadata_timeout");
	Check(Scalar(conn,
				 "SELECT count(*) FROM duckdb_tables() WHERE database_name = 'mi_tx' AND table_name = 'mi_seen'") == 1,
		  "the hold released, the listing loads");
	Check(Scalar(conn, "SELECT active_connections FROM mssql_pool_stats('mi_tx')") == 0, "nothing is left checked out");
	Run(conn, "DETACH mi_tx");
	Run(hold, "SELECT mssql_exec('mi_hold', 'DROP TABLE IF EXISTS dbo.mi_seen')");

	std::cout << (failures ? "FAILED: " + std::to_string(failures) + " check(s)" : "all checks passed") << std::endl;
	return failures ? 1 : 0;
}
