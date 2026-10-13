// Spec 080 PR 3b: a pushed DML's vehicle is consumed exactly as the native DML
// it replaces, and issue #451: an autocommit write commits and rolls back with
// DuckDB.
//
// The remote-pushdown rewriter turns a DML into `SELECT * FROM mssql_dml(...)`.
// Until DuckDB #26880 the planner decided how a result was consumed: a SELECT
// was AUTO (its producers waited for the client) and a DML FORCED (it ran whole
// on the engine's threads), and the vehicle's bind set FORCED so a client that
// submitted and never read still had its UPDATE run. #26880 removed that
// (ResultEagerness is gone): Submit leaves every result to its consumer, so a
// submitted write -- native or the vehicle -- waits until Complete /
// Materialize / a stream settles it, and Query / Execute retain at submission.
// What the vehicle owes is now parity: in every sequence a client can drive,
// it ends as a native UPDATE does.
//
// And since DuckDB ef5b57f94ad a result closed before its query ended is
// ABORTED (QueryResult::Close -> AbortInternal): the statement's transaction is
// rolled back. An autocommit UPDATE / DELETE / MERGE on SQL Server -- and an
// INSERT / CTAS when their use_bcp setting is off -- runs as a transaction of
// one statement on a pinned connection (#451), so the same drop leaves the
// server unchanged.
//
// What this checks, through the public API as such a client would:
//   1. control: a plain SELECT submitted and never read stays READY, its
//      result collector parked for the client;
//   2. the vehicle's handle reads like a native UPDATE's: CHANGED_ROWS, a
//      write, and StreamsByDefault (what DuckDB's own readers ask, given the
//      statement as written) answers no for both;
//   3. submitted and never read: the vehicle reaches the state a native UPDATE
//      reaches, and dropped, neither changes a row;
//   4. Complete(): the rows changed, exactly once;
//   5. the same drop for the shipped UPDATE, an INSERT and the pushed
//      INSERT ... SELECT's vehicle joined under mssql_insert_use_bcp = false,
//      and inside an explicit transaction (aborted, then ROLLBACK); by default
//      an INSERT commits on its own and the drop leaves its rows. Every drop
//      happens once the write has run and parked at its count (READY), so
//      "nothing written" cannot pass because nothing ran yet;
//   6. the vehicle dropped soon after submission changes nothing;
//   7. Ctrl-C cancels a statement that has no deadline;
// and after every drop the pool holds no checked-out connection.
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

//! The same statement writing nothing: the handle checks below submit and
//! complete it, so whatever it does on the server leaves the rows as they are.
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

// Poll until the engine has nothing more to do without the client: READY (the
// result collector parked -- the write ran and waits at its count) or a
// terminal state, for at most `seconds`.
QueryResultState PollUntilObservable(QueryResult &result, int seconds) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	auto state = result.Poll();
	while (!IsObservable(state) && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		state = result.Poll();
	}
	return state;
}

// Submit, wait until the write has run and parked at its count (READY), and
// drop the handle without ending the query -- which DuckDB then aborts (#451).
// READY is asserted: a check that "nothing was written" must not pass merely
// because the statement had not run yet.
QueryResultState DropAfterRun(Connection &conn, const std::string &sql) {
	auto handle = conn.Submit(sql);
	Check(!handle->HasError(), "submitted: " + sql.substr(0, 60));
	const auto state = PollUntilObservable(*handle, 30);
	Check(state == QueryResultState::READY, "ran, and waits at its count (READY)");
	return state;
}

// The pool of the `mssql` catalog holds no checked-out connection: an aborted
// statement gave its pinned connection back.
bool NoCheckedOutConnection(Connection &conn) {
	return Scalar(conn, "SELECT active_connections FROM mssql_pool_stats('mssql')") == 0;
}

int main() {
	if (getenv_or("MSSQL_TEST_PASS", "").empty()) {
		std::cout << "MSSQL_TEST_PASS unset: skipping the mssql_dml vehicle test" << std::endl;
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
		Check(PollUntilTerminal(*select, 1) == QueryResultState::READY, "and stays READY, waiting for the client");
	}

	std::cout << "=== the vehicle's handle reads like a native UPDATE's ===" << std::endl;
	for (int native = 1; native >= 0; native--) {
		const std::string label = native ? "native UPDATE" : "mssql_dml";
		const std::string sql = native ? "UPDATE local_t SET v = v" : UPDATE_NOOP;
		auto handle = conn.Submit(sql);
		Check(!handle->HasError(), label + ": submitted");
		auto &properties = handle->GetStatementProperties();
		Check(properties.return_type == StatementReturnType::CHANGED_ROWS,
			  label + ": StatementReturnType::CHANGED_ROWS");
		Check(!properties.IsReadOnly(), label + ": a write (RegisterDBModify)");
		// The statement as written: UPDATE for the native one, SELECT for the
		// vehicle -- whose CHANGED_ROWS is what makes DuckDB's readers say no.
		Check(!StreamsByDefault(handle->GetStatementType(), properties, true) &&
				  !StreamsByDefault(handle->GetStatementType(), properties, false),
			  label + ": StreamsByDefault answers no, read to the end or not");
		handle->Complete();
		Check(!handle->HasError(), label + ": completed");
	}
	Check(RowsAt(conn, 0) == 10, "the handle checks changed no row");

	std::cout << "=== submitted, never read, dropped: as a native UPDATE (#26880, #451) ===" << std::endl;
	const auto native_state = DropAfterRun(conn, "UPDATE local_t SET v = v + 1");
	Check(Scalar(conn, "SELECT sum(v) FROM local_t") == 0, "native UPDATE: no row changed");
	const auto vehicle_state = DropAfterRun(conn, UPDATE_ALL);
	Check(vehicle_state == native_state, "the vehicle waits where the native UPDATE does");
	Check(RowsAt(conn, 0) == 10, "mssql_dml: no row changed on the server");
	Check(NoCheckedOutConnection(conn), "the statement's connection went back to the pool");

	std::cout << "=== Complete() and drop, unread ===" << std::endl;
	{
		auto handle = conn.Submit(UPDATE_ALL);
		handle->Complete();
		Check(!handle->HasError(), "completed");
	}
	Check(RowsAt(conn, 1) == 10, "every row changed, exactly once");

	std::cout << "=== the other autocommit write paths, dropped unread ===" << std::endl;
	{
		Run(conn, "SET mssql_dml_pushdown = false");
		DropAfterRun(conn, "UPDATE mssql.dbo.dml_vehicle_gate SET v = v + 1");
		Run(conn, "RESET mssql_dml_pushdown");
		Check(RowsAt(conn, 1) == 10, "shipped UPDATE (VALUES join): no row changed");
		// An INSERT joins the statement's transaction under
		// mssql_insert_use_bcp = false; by default it commits on its own once it
		// runs (and keeps the bulk path's parallel writers).
		Run(conn, "SET mssql_insert_use_bcp = false");
		DropAfterRun(conn, "INSERT INTO mssql.dbo.dml_vehicle_ins VALUES (1), (2)");
		Check(Scalar(conn, "SELECT count(*) FROM mssql.dbo.dml_vehicle_ins") == 0, "INSERT, joined: nothing written");
		DropAfterRun(conn, "INSERT INTO mssql.dbo.dml_vehicle_ins SELECT range::INT FROM range(5000)");
		Check(Scalar(conn, "SELECT count(*) FROM mssql.dbo.dml_vehicle_ins") == 0,
			  "INSERT ... SELECT, joined: nothing written");
		Check(NoCheckedOutConnection(conn), "no connection left checked out");
		{
			auto handle = conn.Submit("INSERT INTO mssql.dbo.dml_vehicle_ins SELECT range::INT FROM range(5000)");
			handle->Complete();
			Check(!handle->HasError(), "the same INSERT, completed");
		}
		Check(Scalar(conn, "SELECT count(*) FROM mssql.dbo.dml_vehicle_ins") == 5000, "and committed");
		Run(conn, "RESET mssql_insert_use_bcp");
		DropAfterRun(conn, "INSERT INTO mssql.dbo.dml_vehicle_ins SELECT range::INT FROM range(5000)");
		// A submitted write runs and waits at its count; by default the INSERT
		// committed on its own at Finalize, so DuckDB's rollback of the dropped
		// statement leaves it.
		Check(Scalar(conn, "SELECT count(*) FROM mssql.dbo.dml_vehicle_ins") == 10000,
			  "by default an INSERT through INSERT BULK commits on its own: the drop leaves it");
		// The same for the pushed INSERT ... SELECT's vehicle (kind 'insert'),
		// which reads the setting at bind: by default it commits on its own at
		// InitGlobal, under mssql_insert_use_bcp = false it joins.
		const std::string pushed_insert =
			"SELECT * FROM mssql_dml('mssql', 'INSERT INTO [dbo].[dml_vehicle_ins] ([i]) SELECT TOP (3) [id] FROM "
			"[dbo].[dml_vehicle_gate]', kind := 'insert', target_schema := 'dbo', target_table := 'dml_vehicle_ins')";
		DropAfterRun(conn, pushed_insert);
		Check(Scalar(conn, "SELECT count(*) FROM mssql.dbo.dml_vehicle_ins") == 10003,
			  "pushed INSERT ... SELECT, by default: commits on its own, the drop leaves it");
		Run(conn, "SET mssql_insert_use_bcp = false");
		DropAfterRun(conn, pushed_insert);
		Run(conn, "RESET mssql_insert_use_bcp");
		Check(Scalar(conn, "SELECT count(*) FROM mssql.dbo.dml_vehicle_ins") == 10003,
			  "pushed INSERT ... SELECT, joined: nothing written");
		Check(NoCheckedOutConnection(conn), "no connection left checked out");
	}

	std::cout << "=== inside an explicit transaction: aborted, then ROLLBACK ===" << std::endl;
	{
		Run(conn, "BEGIN");
		DropAfterRun(conn, UPDATE_ALL);
		auto next = conn.Query("SELECT 42");
		Check(next->HasError(), "the transaction is aborted, as for a native table");
		Check(Run(conn, "ROLLBACK"), "ROLLBACK");
	}
	Check(RowsAt(conn, 1) == 10, "no row changed");

	std::cout << "=== the vehicle dropped soon after submission ===" << std::endl;
	{
		auto handle = conn.Submit(
			"SELECT * FROM mssql_dml('mssql', 'WAITFOR DELAY ''00:00:02''; UPDATE [dbo].[dml_vehicle_gate] SET [v] = "
			"[v] + 1000', kind := 'update', target_schema := 'dbo', target_table := 'dml_vehicle_gate')");
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		// Whether it ran yet or not, the drop leaves the server unchanged: an
		// unstarted statement never runs, a running one is rolled back.
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
