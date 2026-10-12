#pragma once

#include "duckdb/common/shared_ptr.hpp"

#include <memory>

namespace duckdb {

class ClientContext;
class MSSQLCatalog;

namespace tds {
class TdsConnection;
class ConnectionPool;
}  // namespace tds

//===----------------------------------------------------------------------===//
// ConnectionProvider - Utility class for acquiring connections based on
// transaction context (Spec 001-mssql-transactions)
//===----------------------------------------------------------------------===//
//
// This class provides a unified interface for connection acquisition that
// respects DuckDB transaction boundaries. When called from within a DuckDB
// transaction, it returns (and potentially pins) a connection to that
// transaction. When called outside a transaction (autocommit mode), it
// acquires and releases connections from the pool normally.
//
// Key behaviors:
// - GetConnection() in transaction: Returns pinned connection (lazy-pins on first call)
// - GetConnection() in autocommit: Acquires from pool -- unless the statement
//   writes to the catalog and has joined (JoinStatementTransaction, issue #451):
//   then it is a transaction of one statement, pinned like the above
// - ReleaseConnection() in transaction: No-op (connection stays pinned)
// - ReleaseConnection() in autocommit: Returns to pool
// - IsInTransaction(): Checks if context has an active DuckDB transaction
//
// Usage pattern in DML executors:
//   auto conn = ConnectionProvider::GetConnection(context, catalog, timeout);
//   // ... execute SQL ...
//   ConnectionProvider::ReleaseConnection(context, catalog, conn);
//

class ConnectionProvider {
public:
	//! Get a connection for the current context
	//! If in a DuckDB transaction, returns the pinned connection (pins one on first call)
	//! If in autocommit mode, acquires from pool
	//! The SQL Server BEGIN TRANSACTION is lazily started on first GetConnection in a transaction
	//! @param context The DuckDB client context
	//! @param catalog The MSSQL catalog (for pool access)
	//! @param timeout_ms Connection acquisition timeout (-1 = use default)
	//! @return Shared pointer to a TDS connection
	//! @throws Exception if connection cannot be acquired
	static std::shared_ptr<tds::TdsConnection> GetConnection(ClientContext &context, MSSQLCatalog &catalog,
															 int timeout_ms = -1);

	//! Release a connection back to the pool (no-op if in transaction)
	//! If in a DuckDB transaction, this is a no-op - connection stays pinned
	//! If in autocommit mode, returns connection to pool
	//! @param context The DuckDB client context
	//! @param catalog The MSSQL catalog (for pool access)
	//! @param conn The connection to release
	static void ReleaseConnection(ClientContext &context, MSSQLCatalog &catalog,
								  std::shared_ptr<tds::TdsConnection> conn);

	//! Check if the context is in an active DuckDB transaction with MSSQL.
	//! NOTE: this CREATES the catalog's transaction if the context is in one
	//! and it does not exist yet -- mssql_exec depends on that, because it
	//! bypasses the binder path that would otherwise create it. It is therefore
	//! equivalent to "not autocommit" for any MSSQL catalog, and is NOT the way
	//! to ask whether a transaction has already touched MSSQL: use
	//! HasUsedAnyMSSQLCatalogInTransaction for that, which is deliberately NOT
	//! per-catalog (review of 7f13a0a).
	//! Issue #451: also true in autocommit once the statement has joined
	//! (JoinStatementTransaction) -- the statement then runs on the catalog's
	//! pinned connection exactly as inside BEGIN ... COMMIT. So this answers
	//! "is the connection pinned", which is what every caller asks.
	//! @param context The DuckDB client context
	//! @param catalog The MSSQL catalog
	//! @return true in an explicit DuckDB transaction, or in a joined statement
	static bool IsInTransaction(ClientContext &context, MSSQLCatalog &catalog);

	//! Issue #451: an autocommit statement that writes to `catalog` runs on it as
	//! a transaction of one statement, as postgres_scanner and mysql_scanner run
	//! every DuckDB transaction: one pinned connection, BEGIN at its first use,
	//! COMMIT / ROLLBACK when DuckDB commits or rolls back the statement's own
	//! transaction (MSSQLTransactionManager). Called where a write starts -- the
	//! sinks' global state, mssql_dml's InitGlobal, a joining CTAS -- AND at the
	//! top of the InitGlobal of every scan the optimizer marked
	//! (`joins_statement`: a scan of a catalog the statement writes to and
	//! joins, also materialised): DuckDB may initialise a join's build side or a
	//! MERGE's source before the sink (every pipeline's initialise event is
	//! dependency-free), so whichever of them comes first pins and the rest find
	//! it pinned. A scan the optimizer did not mark (optimizer off, a plan copy)
	//! may still take a pool connection first; IsPinnedConnection classifies it
	//! by identity, so it goes back to the pool. Idempotent; a no-op inside an
	//! explicit transaction, which is pinned already.
	static void JoinStatementTransaction(ClientContext &context, MSSQLCatalog &catalog);
	//! The same, for the catalog a DML target names.
	static void JoinStatementTransaction(ClientContext &context, const string &catalog_name);

	//! Is `conn` the transaction's pinned connection? Asked of a connection in
	//! hand, by identity -- not "is the context pinned now": an autocommit
	//! statement can join (another thread's sink) between a scan's
	//! GetConnection taking a POOL connection and the scan labelling it, and a
	//! pool connection labelled pinned never went back to the pool (review of
	//! #451). Use this wherever a connection is classified after GetConnection.
	static bool IsPinnedConnection(ClientContext &context, MSSQLCatalog &catalog,
								   const std::shared_ptr<tds::TdsConnection> &conn);

	//! Has an explicit transaction ALREADY touched ANY attached MSSQL catalog?
	//! Asks without creating anything, so a transaction that has only done
	//! DuckDB-side work answers false -- it holds no pinned connection and
	//! nothing uncommitted on any server. Deliberately NOT per-catalog: see the
	//! implementation for the aliasing hazard that makes that unsafe.
	//! @param context The DuckDB client context
	//! @return true only in an explicit transaction that has used MSSQL
	static bool HasUsedAnyMSSQLCatalogInTransaction(ClientContext &context);

	//! Check if the context has an active SQL Server transaction (BEGIN TRANSACTION sent)
	//! @param context The DuckDB client context
	//! @param catalog The MSSQL catalog
	//! @return true if SQL Server transaction is active on pinned connection
	static bool IsSqlServerTransactionActive(ClientContext &context, MSSQLCatalog &catalog);

	//! `mssql_reset_connection` — should a connection returned to the pool have
	//! its SESSION reset before the next user sees it (issue #189)?
	//!
	//! Read on the CLIENT thread and carried to wherever the release happens.
	//! MSSQLResultStream's destructor is a release path and may run on a worker
	//! thread, where touching the ClientContext is a data race (issue #178) — so
	//! the answer travels with the stream exactly as `transaction_pinned` does.
	static bool ShouldResetOnRelease(ClientContext &context);
};

namespace mssql {

//! Release a connection that may have died mid-BCP bulk-load back to its pool.
//! A COPY/CTAS that dies mid-stream leaves the server awaiting bulk data on
//! the session, so the connection is closed first (ends the session, rolls
//! back the INSERT BULK transaction, drops the target-table locks); returning
//! it as-is would hand the next caller a connection stuck mid-bulk-load.
//! Shared by ~MSSQLCopyGlobalState (issue #191) and
//! CTASExecutionState::ReleaseBCPConnectionOnError — keep ONE copy of this
//! protocol. Safe on worker threads (touches no ClientContext, issue #178);
//! `connection` is always null on return.
//! @param connection The (possibly mid-BCP) connection; reset on return
//! @param pool_handle weak_ptr to the owning catalog's pool; a failed lock()
//!        (catalog torn down) drops the connection instead
//! @param transaction_pinned true if an MSSQLTransaction owns the pin — the
//!        reference is dropped without a pool Release
//! @param reset_on_release `mssql_reset_connection` as resolved on the CLIENT
//!        thread. Deliberately has NO default: this was the one reset site the
//!        setting did not reach, and a defaulted parameter is exactly how it
//!        would come to be half-honoured again. Every caller states its answer.
//!
//!        Mostly it decides nothing here — the branch below Closes the
//!        connection unless it was already Idle, and closing ends the session
//!        whatever the flag says — but "every release path" is the setting's
//!        contract, so the Idle sub-case obeys it too.
void ReleaseBcpConnectionOnError(std::shared_ptr<tds::TdsConnection> &connection,
								 const duckdb::weak_ptr<tds::ConnectionPool> &pool_handle, bool transaction_pinned,
								 bool reset_on_release) noexcept;

}  // namespace mssql

}  // namespace duckdb
