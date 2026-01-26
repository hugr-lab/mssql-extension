#pragma once

#include <memory>

namespace duckdb {

class ClientContext;
class MSSQLCatalog;

namespace tds {
class TdsConnection;
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
// - GetConnection() in autocommit: Acquires from pool
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

	//! Check if the context is in an active DuckDB transaction with MSSQL
	//! @param context The DuckDB client context
	//! @param catalog The MSSQL catalog
	//! @return true if in a DuckDB transaction that has accessed this catalog
	static bool IsInTransaction(ClientContext &context, MSSQLCatalog &catalog);

	//! Check if the context has an active SQL Server transaction (BEGIN TRANSACTION sent)
	//! @param context The DuckDB client context
	//! @param catalog The MSSQL catalog
	//! @return true if SQL Server transaction is active on pinned connection
	static bool IsSqlServerTransactionActive(ClientContext &context, MSSQLCatalog &catalog);
};

}  // namespace duckdb
