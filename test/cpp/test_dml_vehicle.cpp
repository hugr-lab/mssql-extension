// Spec 080 PR 3b: the at-least-once gate of a pushed DML (spec 080 D1, W8),
// and issue #451: an autocommit write commits and rolls back with DuckDB.
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
// Since DuckDB ef5b57f94ad a result closed before any call ended the query is
// ABORTED: QueryResult::Close -> AbortInternal rolls back the statement's
// transaction, and Poll() does not end a query (Complete(), a fetch to the
// end, or ExecuteTask() on a RETAINED result does). A native UPDATE polled to
// FINISHED and dropped is rolled back. An autocommit UPDATE / DELETE / MERGE
// on SQL Server -- and an INSERT / CTAS when their use_bcp setting is off --
// runs as a transaction of one statement on a pinned connection (#451), so
// the server's COMMIT comes with DuckDB's and the same drop rolls it back.
//
// What this checks, through the public API as such a client would:
//   1. control: a plain SELECT submitted and never read stays READY (the test
//      can tell the two apart);
//   2. the vehicle's handle carries what a native UPDATE's does: FORCED and
//      CHANGED_ROWS, and a stream cannot be opened on either;
//   3. the gate: the vehicle submitted and never read runs to FINISHED on the
//      engine's threads; dropped without ending the query it is rolled back,
//      as a native UPDATE is in the same sequence (#451);
//   4. Complete() and drop, unread: the rows changed, exactly once;
//   5. the same drop for the shipped UPDATE, for an INSERT under
//      mssql_insert_use_bcp = false (which joins the statement's transaction),
//      and inside an explicit transaction (aborted, then ROLLBACK); by default
//      an INSERT commits on its own and the drop leaves its rows;
//   6. a vehicle dropped mid-flight is rolled back;
//   7. Ctrl-C cancels a statement that has no deadline;
// and after every drop the pool holds no checked-out connection.
// A FORCED statement starts running on the engine's threads as soon as it is
// submitted, so the handle checks of (2) submit one that writes nothing.
// Checked against the bind without FORCED: (2)'s eagerness and stream checks
// and (3)'s FINISHED fail -- the handle sits at READY for the whole wait.
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

// Submit, poll until execution is done, and drop the handle without ending the
// query -- which DuckDB then aborts (#451).
void DropAfterFinished(Connection &conn, const std::string &sql) {
	auto handle = conn.Submit(sql);
	Check(!handle->HasError(), "submitted: " + sql.substr(0, 60));
	Check(PollUntilTerminal(*handle, 30) == QueryResultState::FINISHED, "FINISHED, then dropped unended");
}

// The pool of the `mssql` catalog holds no checked-out connection: an aborted
// statement gave its pinned connection back.
bool NoCheckedOutConnection(Connection &conn) {
	return Scalar(conn, "SELECT active_connections FROM mssql_pool_stats('mssql')") == 0;
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
			 "SELECT TOP (10) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)), 0 FROM sys.all_objects; DROP TABLE IF EXISTS "
			 "dbo.dml_vehicle_ins; CREATE TABLE dbo.dml_vehicle_ins (i int)')") ||
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

	std::cout << "=== the gate: submitted, never read, run to FINISHED, dropped: rolled back (#451) ===" << std::endl;
	{
		auto handle = conn.Submit(UPDATE_ALL);
		Check(!handle->HasError(), "submitted");
		Check(PollUntilTerminal(*handle, 30) == QueryResultState::FINISHED,
			  "it runs to FINISHED on the engine's threads with no read");
		// Dropped here, unended: DuckDB aborts it.
	}
	Check(RowsAt(conn, 0) == 10, "no row changed: the server rolled back with DuckDB");
	Check(NoCheckedOutConnection(conn), "the statement's connection went back to the pool");
	{
		auto handle = conn.Submit("UPDATE local_t SET v = v + 1");
		Check(PollUntilTerminal(*handle, 30) == QueryResultState::FINISHED, "native UPDATE: FINISHED");
	}
	Check(Scalar(conn, "SELECT sum(v) FROM local_t") == 0, "native UPDATE, the same drop: rolled back too");

	std::cout << "=== Complete() and drop, unread ===" << std::endl;
	{
		auto handle = conn.Submit(UPDATE_ALL);
		handle->Complete();
		Check(!handle->HasError(), "completed");
	}
	Check(RowsAt(conn, 1) == 10, "every row changed, exactly once");

	std::cout << "=== the other autocommit write paths, dropped after FINISHED ===" << std::endl;
	{
		Run(conn, "SET mssql_dml_pushdown = false");
		DropAfterFinished(conn, "UPDATE mssql.dbo.dml_vehicle_gate SET v = v + 1");
		Run(conn, "RESET mssql_dml_pushdown");
		Check(RowsAt(conn, 1) == 10, "shipped UPDATE (VALUES join): rolled back");
		// An INSERT joins the statement's transaction under
		// mssql_insert_use_bcp = false; by default it commits on its own (and
		// keeps the bulk path's parallel writers).
		Run(conn, "SET mssql_insert_use_bcp = false");
		DropAfterFinished(conn, "INSERT INTO mssql.dbo.dml_vehicle_ins VALUES (1), (2)");
		Check(Scalar(conn, "SELECT count(*) FROM mssql.dbo.dml_vehicle_ins") == 0, "INSERT, joined: rolled back");
		DropAfterFinished(conn, "INSERT INTO mssql.dbo.dml_vehicle_ins SELECT range::INT FROM range(5000)");
		Check(Scalar(conn, "SELECT count(*) FROM mssql.dbo.dml_vehicle_ins") == 0,
			  "INSERT ... SELECT, joined: rolled back");
		Check(NoCheckedOutConnection(conn), "no connection left checked out");
		{
			auto handle = conn.Submit("INSERT INTO mssql.dbo.dml_vehicle_ins SELECT range::INT FROM range(5000)");
			handle->Complete();
			Check(!handle->HasError(), "the same INSERT, completed");
		}
		Check(Scalar(conn, "SELECT count(*) FROM mssql.dbo.dml_vehicle_ins") == 5000, "and committed");
		Run(conn, "RESET mssql_insert_use_bcp");
		DropAfterFinished(conn, "INSERT INTO mssql.dbo.dml_vehicle_ins SELECT range::INT FROM range(5000)");
		Check(Scalar(conn, "SELECT count(*) FROM mssql.dbo.dml_vehicle_ins") == 10000,
			  "by default an INSERT through INSERT BULK commits on its own: the drop leaves it");
		Check(NoCheckedOutConnection(conn), "no connection left checked out");
	}

	std::cout << "=== inside an explicit transaction: aborted, then ROLLBACK ===" << std::endl;
	{
		Run(conn, "BEGIN");
		DropAfterFinished(conn, UPDATE_ALL);
		auto next = conn.Query("SELECT 42");
		Check(next->HasError(), "the transaction is aborted, as for a native table");
		Check(Run(conn, "ROLLBACK"), "ROLLBACK");
	}
	Check(RowsAt(conn, 1) == 10, "no row changed");

	std::cout << "=== the vehicle dropped mid-flight ===" << std::endl;
	{
		auto handle = conn.Submit(
			"SELECT * FROM mssql_dml('mssql', 'WAITFOR DELAY ''00:00:02''; UPDATE [dbo].[dml_vehicle_gate] SET [v] = "
			"[v] + 1000', kind := 'update', target_schema := 'dbo', target_table := 'dml_vehicle_gate')");
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		// The drop waits for the running statement (Executor::CancelTasks
		// drains it), then DuckDB rolls back -- and the server with it.
	}
	Check(RowsAt(conn, 1) == 10, "rolled back: no row changed");
	Check(NoCheckedOutConnection(conn), "no connection left checked out");

	std::cout << "=== Ctrl-C cancels a statement that has no deadline ===" << std::endl;
	{
		// Interrupt() from another thread while the server sits in WAITFOR: the
		// vehicle's wait sees it within a second, sends ATTENTION, and the
		// server rolls the statement back.
		std::thread interrupter([&conn]() {
			std::this_thread::sleep_for(std::chrono::seconds(2));
			conn.Interrupt();
		});
		const auto started = std::chrono::steady_clock::now();
		auto result = conn.Query(
			"SELECT * FROM mssql_dml('mssql', 'WAITFOR DELAY ''00:00:30''; UPDATE [dbo].[dml_vehicle_gate] SET [v] = "
			"[v] + 100', kind := 'update', target_schema := 'dbo', target_table := 'dml_vehicle_gate')");
		const auto seconds =
			std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
		interrupter.join();
		Check(result->HasError() && result->GetErrorType() == ExceptionType::INTERRUPT, "the statement is interrupted");
		Check(seconds < 15, "within seconds, not the 30 of the WAITFOR");
	}
	Check(RowsAt(conn, 1) == 10, "no row changed");
	Check(Scalar(conn, "SELECT count(*) FROM mssql.dbo.dml_vehicle_gate") == 10, "the next statement runs");

	Run(conn, "SELECT mssql_exec('mssql', 'DROP TABLE dbo.dml_vehicle_gate; DROP TABLE dbo.dml_vehicle_ins')");
	if (failures) {
		std::cout << failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "all checks passed" << std::endl;
	return 0;
}
