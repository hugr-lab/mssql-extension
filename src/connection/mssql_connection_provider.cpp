#include "connection/mssql_connection_provider.hpp"

#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_transaction.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "query/mssql_simple_query.hpp"
#include "tds/tds_connection.hpp"
#include "tds/tds_connection_pool.hpp"
#include "tds/tds_socket.hpp"
#include "tds/tds_token_parser.hpp"

#include <cstdio>
#include <cstdlib>

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetConnProviderDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
	return level;
}

#define MSSQL_CONN_LOG(fmt, ...)                                           \
	do {                                                                   \
		if (GetConnProviderDebugLevel() >= 1) {                            \
			fprintf(stderr, "[MSSQL_CONN_PROV] " fmt "\n", ##__VA_ARGS__); \
		}                                                                  \
	} while (0)

namespace duckdb {

//===----------------------------------------------------------------------===//
// Helper: Get MSSQLTransaction from context if in transaction
//===----------------------------------------------------------------------===//

static MSSQLTransaction *TryGetMSSQLTransaction(ClientContext &context, MSSQLCatalog &catalog) {
	// Check if we're in a transaction context
	auto &meta_transaction = MetaTransaction::Get(context);

	// Get the attached database for this catalog
	auto &db = catalog.GetAttached();

	// If we're in an explicit transaction (not autocommit), use GetTransaction
	// to ensure the transaction is created for this catalog.
	// This is needed because mssql_exec bypasses the normal binder path
	// which would normally create the transaction when accessing the catalog.
	if (!context.transaction.IsAutoCommit()) {
		// GetTransaction creates the transaction if it doesn't exist
		auto &transaction = meta_transaction.GetTransaction(db);
		return &transaction.Cast<MSSQLTransaction>();
	}

	// In autocommit mode, try to get existing transaction (should be nullptr)
	auto transaction = meta_transaction.TryGetTransaction(db);
	if (!transaction) {
		return nullptr;
	}

	return &transaction->Cast<MSSQLTransaction>();
}

//===----------------------------------------------------------------------===//
// ConnectionProvider::IsInTransaction
//===----------------------------------------------------------------------===//

bool ConnectionProvider::IsInTransaction(ClientContext &context, MSSQLCatalog &catalog) {
	// In autocommit mode, we're not in an explicit transaction
	if (context.transaction.IsAutoCommit()) {
		return false;
	}
	auto *txn = TryGetMSSQLTransaction(context, catalog);
	return txn != nullptr;
}

//===----------------------------------------------------------------------===//
// ConnectionProvider::HasUsedAnyMSSQLCatalogInTransaction
//===----------------------------------------------------------------------===//

bool ConnectionProvider::HasUsedAnyMSSQLCatalogInTransaction(ClientContext &context) {
	if (context.transaction.IsAutoCommit()) {
		return false;
	}
	// TryGetTransaction, never GetTransaction: the question is what the
	// transaction has ALREADY touched, and GetTransaction -- which
	// TryGetMSSQLTransaction above calls deliberately, so mssql_exec gets a
	// transaction when it bypasses the binder -- would create one and answer yes
	// for a catalog nobody has named. That is why IsInTransaction scopes nothing
	// and this exists (review of 7f13a0a).
	//
	// ANY MSSQL catalog, not just the one the caller named (review of 0e12914):
	// two ATTACHes of the same DSN under different aliases are independent
	// catalogs with independent pools, so uncommitted DDL done through alias A is
	// invisible to a per-catalog test on B -- and a pool connection taken on B
	// then blocks on A's schema lock until mssql_metadata_timeout, which is the
	// #380 hang this refusal exists to prevent. Comparing the underlying server
	// and database would be narrower; this is the conservative form, and it
	// still admits the case the scoping was for: a transaction that wraps
	// unrelated DuckDB work in BEGIN ... COMMIT and has touched no MSSQL catalog
	// at all.
	auto &meta_transaction = MetaTransaction::Get(context);
	auto &db_manager = DatabaseManager::Get(context);
	auto attached_dbs = db_manager.GetDatabases(context);
	for (auto &db : attached_dbs) {
		if (!db) {
			continue;
		}
		if (db->GetCatalog().GetCatalogType() != "mssql") {
			continue;
		}
		if (meta_transaction.TryGetTransaction(*db)) {
			return true;
		}
	}
	return false;
}

//===----------------------------------------------------------------------===//
// ConnectionProvider::ShouldResetOnRelease
//===----------------------------------------------------------------------===//

bool ConnectionProvider::ShouldResetOnRelease(ClientContext &context) {
	Value val;
	if (context.TryGetCurrentSetting("mssql_reset_connection", val)) {
		return val.GetValue<bool>();
	}
	return tds::DEFAULT_RESET_CONNECTION;
}

//===----------------------------------------------------------------------===//
// ConnectionProvider::IsSqlServerTransactionActive
//===----------------------------------------------------------------------===//

bool ConnectionProvider::IsSqlServerTransactionActive(ClientContext &context, MSSQLCatalog &catalog) {
	// In autocommit mode, SQL Server transactions are never active (each statement is independent)
	if (context.transaction.IsAutoCommit()) {
		return false;
	}
	auto *txn = TryGetMSSQLTransaction(context, catalog);
	if (!txn) {
		return false;
	}
	return txn->IsSqlServerTransactionActive();
}

std::shared_ptr<tds::TdsConnection> ConnectionProvider::GetConnection(ClientContext &context, MSSQLCatalog &catalog,
																	  int timeout_ms) {
	auto *txn = TryGetMSSQLTransaction(context, catalog);

	// Check if we're in autocommit mode (implicit transaction per statement)
	// In autocommit mode, each statement is independent - no need to pin a connection
	bool is_autocommit = context.transaction.IsAutoCommit();

	MSSQL_CONN_LOG("GetConnection: context=%p, txn=%p, is_autocommit=%d", (void *)&context, (void *)txn, is_autocommit);

	if (!txn || is_autocommit) {
		// Not in a transaction OR in autocommit mode - acquire from pool
		MSSQL_CONN_LOG("GetConnection: Autocommit mode (txn=%p, is_autocommit=%d), acquiring from pool", (void *)txn,
					   is_autocommit);
		auto &pool = catalog.GetConnectionPool();
		auto stats_before = pool.GetStats();
		MSSQL_CONN_LOG("GetConnection: Pool before acquire - total=%zu, active=%zu, idle=%zu",
					   stats_before.total_connections, stats_before.active_connections, stats_before.idle_connections);
		std::string why;
		auto conn = pool.Acquire(timeout_ms, &why);
		if (!conn) {
			throw IOException("MSSQL: Failed to acquire connection: " + why);
		}
		auto stats_after = pool.GetStats();
		MSSQL_CONN_LOG("GetConnection: Pool connection acquired, tds_conn=%p, spid=%d, has_txn_desc=%d",
					   (void *)conn.get(), conn->GetSpid(), conn->HasTransactionDescriptor());
		MSSQL_CONN_LOG("GetConnection: Pool after acquire - total=%zu, active=%zu, idle=%zu",
					   stats_after.total_connections, stats_after.active_connections, stats_after.idle_connections);
		return conn;
	}

	// In an explicit DuckDB transaction (BEGIN was issued) - use pinned connection
	MSSQL_CONN_LOG("GetConnection: Explicit transaction mode (context=%p, txn=%p)", (void *)&context, (void *)txn);

	// Check if we already have a pinned connection
	// One critical section for the whole lazy pin (issue #356): check, acquire,
	// BEGIN, publish. A second thread arriving mid-sequence waits here and then
	// finds a connection that is pinned, begun and Idle — instead of either
	// executing on one that is mid-BEGIN, or starting a second transaction of
	// its own on a second connection.
	auto pin_lock = txn->LockForPin();

	auto pinned = txn->GetPinnedConnection();
	if (pinned) {
		MSSQL_CONN_LOG("GetConnection: Returning existing pinned tds_conn=%p, spid=%d", (void *)pinned.get(),
					   pinned->GetSpid());
		return pinned;
	}

	// First access in this transaction - need to pin a connection
	MSSQL_CONN_LOG("GetConnection: First access in transaction, acquiring and pinning connection");

	// Acquire connection from pool
	auto &pool = catalog.GetConnectionPool();
	auto stats_before = pool.GetStats();
	MSSQL_CONN_LOG("GetConnection: Pool before acquire - total=%zu, active=%zu, idle=%zu",
				   stats_before.total_connections, stats_before.active_connections, stats_before.idle_connections);
	std::string why;
	auto conn = pool.Acquire(timeout_ms, &why);
	if (!conn) {
		throw IOException("MSSQL: Failed to acquire connection for transaction: " + why);
	}
	MSSQL_CONN_LOG("GetConnection: Acquired tds_conn=%p, spid=%d for pinning", (void *)conn.get(), conn->GetSpid());

	// NOT pinned yet: publishing here would expose a connection that BEGIN has
	// not finished with. It is published below, once the transaction is open and
	// the connection is back to Idle.

	// The isolation level first (issue #331): SET TRANSACTION ISOLATION LEVEL has
	// to precede BEGIN -- a transaction cannot be switched to SNAPSHOT once it has
	// started -- and it goes as its own statement so a refusal is seen as one.
	// In the BEGIN batch a failed SET would let BEGIN run at the old level, and
	// the reply scan below stops at the ERROR token before the descriptor.
	// Only when the option asks for a level; none means no round trip, as before.
	auto isolation = catalog.TransactionIsolationStatement();
	if (!isolation.empty()) {
		auto result = MSSQLSimpleQuery::Execute(*conn, isolation);
		if (!result.success) {
			pool.Release(conn);
			throw IOException("MSSQL: %s failed: %s", isolation, result.error_message);
		}
	}
	// From here the session carries the level. RESET_CONNECTION does not clear it,
	// so if BEGIN fails the connection must not go back to the pool as it is: it
	// is closed first (review of #381), on every failure path below.
	const bool close_on_failure = !isolation.empty();

	// Start SQL Server transaction lazily (BEGIN TRANSACTION)
	MSSQL_CONN_LOG("GetConnection: Starting SQL Server transaction");

	if (!conn->ExecuteBatch("BEGIN TRANSACTION", "BEGIN TRANSACTION")) {
		// Failed to start transaction - release connection and throw
		MSSQL_CONN_LOG("GetConnection: ExecuteBatch failed: %s", conn->GetLastError().c_str());
		if (close_on_failure) {
			conn->Close();
		}
		pool.Release(conn);
		throw IOException("MSSQL: Failed to start SQL Server transaction: " + conn->GetLastError());
	}

	// Receive the complete TDS response (should be a simple DONE token)
	auto *socket = conn->GetSocket();
	if (!socket) {
		if (close_on_failure) {
			conn->Close();
		}
		pool.Release(conn);
		throw IOException("MSSQL: Socket is null after BEGIN TRANSACTION");
	}

	std::vector<uint8_t> response;
	if (!socket->ReceiveMessage(response, 5000)) {
		MSSQL_CONN_LOG("GetConnection: ReceiveMessage failed: %s", socket->GetLastError().c_str());
		conn->Close();
		pool.Release(conn);
		throw IOException("MSSQL: Failed to receive BEGIN TRANSACTION response: " + socket->GetLastError());
	}

	// Parse the (untrusted) server response for the ENVCHANGE BEGIN_TRANS 8-byte
	// transaction descriptor. tds::FindBeginTxnDescriptor is a hardened, fuzzed,
	// unit-tested scan that never reads past the buffer no matter what token
	// lengths the server advertises (replaces a hand-rolled inline loop).
	uint8_t descriptor[8];
	bool found_transaction_descriptor = tds::FindBeginTxnDescriptor(response.data(), response.size(), descriptor);
	if (found_transaction_descriptor) {
		// SetTransactionDescriptor copies the 8 bytes (the local response/buffer
		// here goes out of scope when GetConnection returns).
		txn->SetTransactionDescriptor(descriptor);
		conn->SetTransactionDescriptor(descriptor);
		MSSQL_CONN_LOG("GetConnection: Found transaction descriptor");
	}

	if (!found_transaction_descriptor) {
		MSSQL_CONN_LOG("GetConnection: WARNING - No transaction descriptor found in response");
	}

	// Transition connection back to Idle (ExecuteBatch left it in Executing state).
	// The label reaches a reader through an MSSQL_CONN_STATE=1 log explaining a
	// race, so it says which operation ended here: this is the BEGIN finishing,
	// not a COMMIT or ROLLBACK letting the connection go.
	conn->TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle, "BEGIN TRANSACTION done");

	// Now it is safe to publish: the transaction is open and the connection is
	// Idle, so the next thread through the early return above gets something it
	// can use immediately.
	txn->SetPinnedConnection(conn);

	// Mark SQL Server transaction as active
	txn->SetSqlServerTransactionActive(true);

	MSSQL_CONN_LOG("GetConnection: SQL Server transaction started, connection pinned");
	return conn;
}

//===----------------------------------------------------------------------===//
// ConnectionProvider::ReleaseConnection
//===----------------------------------------------------------------------===//

void ConnectionProvider::ReleaseConnection(ClientContext &context, MSSQLCatalog &catalog,
										   std::shared_ptr<tds::TdsConnection> conn) {
	if (!conn) {
		return;
	}

	auto *txn = TryGetMSSQLTransaction(context, catalog);
	bool is_autocommit = context.transaction.IsAutoCommit();

	if (!txn || is_autocommit) {
		// Not in a transaction OR in autocommit mode - flag for reset and return to pool
		// The RESET_CONNECTION flag will be set on the TDS header of the next SQL_BATCH,
		// which is how ADO.NET/JDBC drivers reset session state (temp tables, variables, SET options)
		//
		// `mssql_reset_connection = false` skips it (issue #189). The bit has no
		// selective form — one bit, two variants, and RESET_CONNECTION_SKIP_TRAN
		// drops `##g` and `#loc` alike — so keeping a global temp table alive
		// across statements means not sending it at all, and the session state it
		// would have cleared becomes the user's to manage.
		//
		// Read per release rather than cached: SET must take effect on the next
		// statement, and this is once per statement, not per row.
		const bool reset_on_release = ShouldResetOnRelease(context);
		MSSQL_CONN_LOG("ReleaseConnection: Autocommit mode, reset=%d", reset_on_release ? 1 : 0);
		conn->SetNeedsReset(reset_on_release);
		auto &pool = catalog.GetConnectionPool();
		pool.Release(conn);
		return;
	}

	// In a transaction - no-op (connection stays pinned until commit/rollback)
	MSSQL_CONN_LOG("ReleaseConnection: Transaction mode, keeping connection pinned (no-op)");
	// Verify it's the pinned connection
	auto pinned = txn->GetPinnedConnection();
	if (pinned != conn) {
		MSSQL_CONN_LOG("WARNING: ReleaseConnection called with non-pinned connection in transaction");
	}
	// Do nothing - connection stays pinned
}

namespace mssql {

void ReleaseBcpConnectionOnError(std::shared_ptr<tds::TdsConnection> &connection,
								 const duckdb::weak_ptr<tds::ConnectionPool> &pool_handle, bool transaction_pinned,
								 bool reset_on_release) noexcept {
	if (!connection) {
		return;
	}

	// Closing ends the session, rolls back the INSERT BULK transaction and
	// drops the locks held on the target table (see the header contract).
	auto conn_state = connection->GetState();
	if (conn_state != tds::ConnectionState::Idle && conn_state != tds::ConnectionState::Disconnected) {
		try {
			connection->Close();
		} catch (...) {
			// Ignore — the shared_ptr destructor closes the socket regardless.
		}
	}

	if (transaction_pinned) {
		// The MSSQLTransaction owns the pin; just drop our reference.
		connection.reset();
		return;
	}

	if (auto pool = pool_handle.lock()) {
		try {
			// The answer comes from the caller, which captured it on the client
			// thread: there is no ClientContext to ask here by design (issue #178
			// — this runs from destructors that may be on a worker thread).
			connection->SetNeedsReset(reset_on_release);
			pool->Release(std::move(connection));
		} catch (...) {
			// Release failed — drop it; the shared_ptr destructor closes the socket.
			connection.reset();
		}
	} else {
		// Catalog torn down — dropping is the safe failure mode.
		connection.reset();
	}
}

}  // namespace mssql

}  // namespace duckdb
