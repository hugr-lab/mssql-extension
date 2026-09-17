// Issue #356: ConnectionProvider::GetConnection inside a transaction, under the
// worst interleaving the engine can produce — several threads asking for the
// transaction's connection at the same instant.
//
// The contract under test. Inside a DuckDB transaction every operator that
// touches an MSSQL catalog goes through ConnectionProvider::GetConnection for
// the ONE connection pinned to that transaction, and the first caller is the
// one that acquires it from the pool, sends BEGIN TRANSACTION on it, and
// publishes it. DuckDB initialises a plan's source and sink on different
// threads, so two callers can arrive together. What must hold whatever the
// interleaving:
//
//   1. every caller gets the SAME connection — a second one would sit outside
//      the transaction, and its rows would not roll back with the rest;
//   2. the connection a caller gets is IDLE, with BEGIN already finished on it
//      — handing it out mid-BEGIN is `Cannot execute: connection not in Idle
//      state`, the defect this test exists for;
//   3. exactly one pool connection is active for the transaction, and none
//      after ROLLBACK — the double-acquire leaves one leaked with an open
//      server transaction.
//
// Why this is a logical test and not a statistical one. The SQL-level
// reproduction depends on DuckDB happening to schedule source and sink on
// different threads at the right moment (~4% per run), and a loop of it only
// buys probability. Here the callers are released from a barrier straight into
// GetConnection: their start skew is microseconds, and the window they must
// land in — the BEGIN round trip — is milliseconds. Every one of them is in the
// window, every round. With the critical section they queue on it and each
// gets the finished connection; without it they get the broken one or acquire
// their own, and the assertions below name which.
//
// No test hook in production code, no DuckDB pipeline, no extension loaded
// from disk: the extension is linked statically so this binary and the
// provider share one set of classes, and the DuckDB transaction is opened
// through the public API exactly as a user's BEGIN would.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_transaction.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "duckdb.hpp"
#include "mssql_extension.hpp"
#include "tds/tds_connection.hpp"
#include "tds/tds_connection_pool.hpp"

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

struct Outcome {
	std::shared_ptr<tds::TdsConnection> conn;
	tds::ConnectionState state = tds::ConnectionState::Idle;
	std::string error;
};

}  // namespace

int main() {
	const int threads = 8;
	const int rounds = 20;

	DuckDB db(nullptr);
	db.LoadStaticExtension<MssqlExtension>();
	Connection conn(db);

	auto attached = conn.Query("ATTACH '" + TestDsn() + "' AS mssql (TYPE mssql)");
	if (attached->HasError()) {
		std::cerr << "ATTACH failed: " << attached->GetError() << std::endl;
		return 2;
	}
	// Catalog lookup is transactional in DuckDB; the catalog object itself
	// outlives the transaction (the DatabaseManager owns it), so the reference
	// stays good after the commit.
	conn.BeginTransaction();
	auto &catalog = Catalog::GetCatalog(*conn.context, Identifier("mssql")).Cast<MSSQLCatalog>();
	conn.Commit();
	auto &pool = catalog.GetConnectionPool();

	std::cout << "=== GetConnection under contention: " << threads << " threads x " << rounds
			  << " rounds ===" << std::endl;

	// Two ways to release the callers, because the two things the critical
	// section prevents show up under different timings and one release pattern
	// finds only one of them:
	//   - all at once: every caller passes "is there a pin yet?" before the
	//     first has acquired, so without the lock each acquires its own —
	//     the double-acquire, seen as N active connections and a leak;
	//   - staggered: the first caller goes alone and the rest arrive while its
	//     BEGIN is on the wire, so with publish-before-BEGIN they are handed a
	//     connection that is still Executing — the "not in Idle state" defect.
	// Measured against the pre-fix provider: the first pattern hits the
	// double-acquire in round 1, the second the mid-BEGIN handout.
	int failures = 0;
	for (int round = 1; round <= rounds; round++) {
		const bool staggered = (round % 2 == 1);
		conn.BeginTransaction();
		// The binder runs on the client thread and is what starts the catalog's
		// transaction, before any operator asks for a connection. Mirror that.
		auto &txn = MSSQLTransaction::Get(*conn.context, catalog);
		if (txn.HasPinnedConnection()) {
			std::cerr << "  round " << round << ": pinned before anyone asked" << std::endl;
			return 1;
		}
		const auto before = pool.GetStats();

		std::vector<Outcome> got(threads);
		std::atomic<int> ready{0};
		std::atomic<bool> go{false};
		std::vector<std::thread> workers;
		std::atomic<bool> go_rest{false};
		for (int t = 0; t < threads; t++) {
			workers.emplace_back([&, t]() {
				ready.fetch_add(1);
				// Yield rather than spin hot: on a 2-vCPU CI runner eight busy
				// loops would take the CPU away from the one thread that is
				// doing the BEGIN round trip, and the staggered pattern depends
				// on that thread getting ahead.
				while (!go.load(std::memory_order_acquire)) {
					std::this_thread::yield();
				}
				if (staggered && t > 0) {
					while (!go_rest.load(std::memory_order_acquire)) {
						std::this_thread::yield();
					}
				}
				try {
					got[t].conn = ConnectionProvider::GetConnection(*conn.context, catalog);
					got[t].state = got[t].conn->GetState();
				} catch (std::exception &e) {
					got[t].error = e.what();
				}
			});
		}
		while (ready.load() < threads) {
			std::this_thread::yield();
		}
		const auto t0 = std::chrono::steady_clock::now();
		go.store(true, std::memory_order_release);
		if (staggered) {
			// Long enough for thread 0 to be inside its BEGIN round trip, far
			// shorter than that round trip.
			std::this_thread::sleep_for(std::chrono::microseconds(100));
			go_rest.store(true, std::memory_order_release);
		}
		for (auto &w : workers) {
			w.join();
		}
		const auto ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

		// (2) nobody was handed a connection mid-BEGIN, and nobody threw.
		for (int t = 0; t < threads; t++) {
			if (!got[t].error.empty()) {
				std::cerr << "  round " << round << " thread " << t << ": " << got[t].error << std::endl;
				failures++;
			} else if (got[t].state != tds::ConnectionState::Idle) {
				std::cerr << "  round " << round << " thread " << t << ": got the connection in state "
						  << static_cast<int>(got[t].state) << " — handed out before BEGIN finished" << std::endl;
				failures++;
			}
		}
		// (1) one connection for everyone, and it is the one the transaction pinned.
		const auto pinned = txn.GetPinnedConnection();
		for (int t = 0; t < threads; t++) {
			if (got[t].conn && got[t].conn != pinned) {
				std::cerr << "  round " << round << " thread " << t
						  << ": got a connection that is not the pinned one — two were acquired" << std::endl;
				failures++;
			}
		}
		if (!txn.IsSqlServerTransactionActive()) {
			std::cerr << "  round " << round << ": no server transaction on the pinned connection" << std::endl;
			failures++;
		}
		// (3) the pool sees exactly one connection active for this transaction.
		const auto during = pool.GetStats();
		if (during.active_connections != 1) {
			std::cerr << "  round " << round << ": " << during.active_connections
					  << " pool connections active during the transaction, expected 1" << std::endl;
			failures++;
		}
		if (during.connections_created > before.connections_created + 1) {
			std::cerr << "  round " << round << ": pool created "
					  << (during.connections_created - before.connections_created) << " connections for one transaction"
					  << std::endl;
			failures++;
		}
		// And the connection everyone got really is inside a server transaction:
		// a statement on it, through the ordinary path, sees @@TRANCOUNT = 1.
		auto tc = conn.Query("SELECT * FROM mssql_scan('mssql', 'SELECT @@TRANCOUNT AS tc')");
		if (tc->HasError() || tc->RowCount() != 1 || tc->GetValue(0, 0).GetValue<int32_t>() != 1) {
			std::cerr << "  round " << round
					  << ": @@TRANCOUNT check failed: " << (tc->HasError() ? tc->GetError() : tc->ToString())
					  << std::endl;
			failures++;
		}

		conn.Rollback();
		const auto after = pool.GetStats();
		if (after.active_connections != 0) {
			std::cerr << "  round " << round << ": " << after.active_connections
					  << " pool connections still active after ROLLBACK — leaked" << std::endl;
			failures++;
		}
		std::cout << "  round " << round << (staggered ? " (staggered)" : " (all at once)") << ": " << threads
				  << " callers in " << ms << " ms, " << (failures ? "FAIL" : "ok") << std::endl;
		if (failures) {
			break;
		}
	}

	if (failures) {
		std::cout << "FAIL: " << failures << " assertion(s)" << std::endl;
		return 1;
	}
	std::cout << "PASS: every caller got the one pinned connection, begun and idle, and nothing leaked" << std::endl;
	return 0;
}
