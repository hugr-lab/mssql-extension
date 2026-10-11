// Spec 080 PR 3b: the at-least-once gate of a pushed DML (spec 080 D1, W8).
//
// The remote-pushdown rewriter turns a DML into `SELECT * FROM mssql_dml(...)`,
// and DuckDB binds a SELECT with ResultEagerness::AUTO. On the 2.0 line the
// producers of a submitted AUTO statement wait for the client to decide how
// to take the result -- a stream or a retained result (QueryResult::ExecuteTask:
// "READY means the engine is waiting for the retention decision"); whether the
// source was initialised by then is a race, and with no spare engine thread
// nothing runs. A client that submits and never reads could leave the UPDATE
// the vehicle stands for undone, where a native UPDATE (FORCED) runs whole on
// the engine's threads.
// The vehicle's bind sets FORCED to keep that promise.
//
// What this checks, through the public API as such a client would:
//   1. control: a plain SELECT submitted and never read stays READY (the test
//      can tell the two apart);
//   2. the vehicle's handle carries what a native UPDATE's does: FORCED and
//      CHANGED_ROWS, and a stream cannot be opened on either;
//   3. the gate: the vehicle submitted and never read runs to FINISHED on the
//      engine's threads, and the rows changed -- once;
//   4. Complete() and drop, unread: the rows changed once more.
// A FORCED statement starts running on the engine's threads as soon as it is
// submitted, so the handle checks of (2) submit one that writes nothing.
// Checked against the bind without FORCED: (2)'s eagerness and stream checks
// and (3)'s FINISHED fail -- the handle sits at READY for the whole wait. The
// row count after (3) does not tell the two apart (dropping the undecided
// handle still ran it here), which is why (3) asserts the state, not only the
// rows.
// sqllogictest cannot submit without reading, which is why this is C++.
//
// Skips (exit 0) when MSSQL_TEST_PASS is unset, like the other live C++ tests.

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "duckdb.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "mssql_extension.hpp"

using namespace duckdb;

namespace {

std::string getenv_or(const char *name, const char *fallback) {
	const char *v = std::getenv(name);
	return v && *v ? v : fallback;
}

std::string TestDsn() {
	return "Server=" + getenv_or("MSSQL_TEST_HOST", "localhost") + "," + getenv_or("MSSQL_TEST_PORT", "1433") +
		   ";Database=TestDB;User Id=" + getenv_or("MSSQL_TEST_USER", "sa") +
		   ";Password=" + getenv_or("MSSQL_TEST_PASS", "") + ";TrustServerCertificate=yes";
}

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

bool Run(Connection &conn, const std::string &sql) {
	auto result = conn.Query(sql);
	if (result->HasError()) {
		std::cerr << "query failed: " << result->GetError() << std::endl;
		return false;
	}
	return true;
}

//! The first column of the first row of `sql` as a BIGINT; -1 on an error.
int64_t Scalar(Connection &conn, const std::string &sql) {
	auto result = conn.Query(sql);
	if (result->HasError()) {
		std::cerr << "query failed: " << result->GetError() << std::endl;
		return -1;
	}
	auto chunk = result->Fetch();
	if (!chunk || chunk->size() == 0) {
		return -1;
	}
	return chunk->GetValue(0, 0).GetValue<int64_t>();
}

int64_t RowsAt(Connection &conn, int v) {
	return Scalar(conn, "SELECT count(*) FROM mssql.dbo.dml_vehicle_gate WHERE v = " + std::to_string(v));
}

//! The same statement writing nothing: the handle checks below submit it and
//! end it at once, and a FORCED statement already runs on the engine's threads
//! by then -- whether it got to the server first is a race.
const char *UPDATE_NOOP =
	"SELECT * FROM mssql_dml('mssql', 'UPDATE [dbo].[dml_vehicle_gate] SET [v] = [v]', "
	"kind := 'update', target_schema := 'dbo', target_table := 'dml_vehicle_gate')";

const char *UPDATE_ALL =
	"SELECT * FROM mssql_dml('mssql', 'UPDATE [dbo].[dml_vehicle_gate] SET [v] = [v] + 1', "
	"kind := 'update', target_schema := 'dbo', target_table := 'dml_vehicle_gate')";

}  // namespace

// Poll a submitted statement until it reaches a terminal state, running
// nothing on this thread, for at most `seconds`.
QueryResultState PollUntilTerminal(QueryResult &result, int seconds) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	auto state = result.Poll();
	while (!IsTerminal(state) && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		state = result.Poll();
	}
	return state;
}

int main() {
	if (getenv_or("MSSQL_TEST_PASS", "").empty()) {
		std::cout << "MSSQL_TEST_PASS unset: skipping the mssql_dml at-least-once test" << std::endl;
		return 0;
	}
	DuckDB db(nullptr);
	db.LoadStaticExtension<MssqlExtension>();
	Connection conn(db);

	// Engine threads beyond the client's: a submitted statement runs on them.
	if (!Run(conn, "SET threads = 4") || !Run(conn, "ATTACH '" + TestDsn() + "' AS mssql (TYPE mssql)") ||
		!Run(conn,
			 "SELECT mssql_exec('mssql', 'DROP TABLE IF EXISTS dbo.dml_vehicle_gate; CREATE TABLE "
			 "dbo.dml_vehicle_gate (id int PRIMARY KEY, v int NOT NULL); INSERT INTO dbo.dml_vehicle_gate "
			 "SELECT TOP (10) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)), 0 FROM sys.all_objects')") ||
		!Run(conn, "CREATE TABLE local_t AS SELECT range AS id, 0 AS v FROM range(10)")) {
		return 2;
	}

	std::cout << "=== control: a SELECT submitted and never read waits for the client ===" << std::endl;
	{
		auto select = conn.Submit("SELECT * FROM range(3)");
		Check(!select->HasError(), "the SELECT is submitted");
		Check(PollUntilTerminal(*select, 1) == QueryResultState::READY, "and stays READY, running nothing");
	}

	std::cout << "=== the vehicle's handle is a native UPDATE's ===" << std::endl;
	for (int native = 1; native >= 0; native--) {
		const std::string label = native ? "native UPDATE" : "mssql_dml";
		const std::string sql = native ? "UPDATE local_t SET v = v" : UPDATE_NOOP;
		{
			auto handle = conn.Submit(sql);
			Check(!handle->HasError(), label + ": submitted");
			Check(handle->GetStatementProperties().result_eagerness == ResultEagerness::FORCED,
				  label + ": ResultEagerness::FORCED");
			Check(handle->GetStatementProperties().return_type == StatementReturnType::CHANGED_ROWS,
				  label + ": StatementReturnType::CHANGED_ROWS");
			handle->Complete();
		}
		auto handle = conn.Submit(sql);
		bool refused = false;
		try {
			QueryResultStream<> stream(std::move(handle));
		} catch (const InvalidInputException &) {
			refused = true;
		}
		Check(refused, label + ": no stream can be opened on it");
	}
	Check(RowsAt(conn, 0) == 10, "the handle checks changed no row");

	std::cout << "=== the gate: submitted, never read, the UPDATE still ran ===" << std::endl;
	{
		auto handle = conn.Submit(UPDATE_ALL);
		Check(!handle->HasError(), "submitted");
		Check(PollUntilTerminal(*handle, 30) == QueryResultState::FINISHED,
			  "it runs to FINISHED on the engine's threads with no read");
		// Dropped here, unread.
	}
	Check(RowsAt(conn, 1) == 10, "all 10 rows changed, exactly once");

	std::cout << "=== Complete() and drop, unread ===" << std::endl;
	{
		auto handle = conn.Submit(UPDATE_ALL);
		handle->Complete();
		Check(!handle->HasError(), "completed");
	}
	Check(RowsAt(conn, 2) == 10, "and each row exactly once more");

	Run(conn, "SELECT mssql_exec('mssql', 'DROP TABLE dbo.dml_vehicle_gate')");
	if (failures) {
		std::cout << failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "all checks passed" << std::endl;
	return 0;
}
