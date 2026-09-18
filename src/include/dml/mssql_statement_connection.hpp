//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// dml/mssql_statement_connection.hpp
//
// The ONE connection a DML statement runs on, and its server transaction
// (spec 062 W1c, issue #344).
//
// The INSERT, UPDATE and DELETE executors used to take a connection per batch
// through ConnectionProvider and hand it back after each: inside a DuckDB
// transaction that was the pinned connection every time, in autocommit it was
// whichever pool connection came up, and each batch committed on its own — so a
// failure in batch K left batches 1..K-1 applied, with no way back (#344). Now
// an executor takes its connection once, on the first batch, keeps it to the
// end of the statement, and in autocommit brackets the batches with a server
// transaction. One statement, one connection, one transaction.
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>

#include "copy/load_transaction.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types.hpp"
#include "tds/tds_connection.hpp"
#include "tds/tds_connection_pool.hpp"

namespace duckdb {

class ClientContext;
class MSSQLCatalog;

class MSSQLStatementConnection {
public:
	MSSQLStatementConnection() = default;

	//! Last resort: a statement that failed without reaching Fail() — or a
	//! sink destroyed on an unwind. Rolls the transaction back and returns the
	//! connection through the release targets captured at Acquire, touching
	//! no ClientContext (issue #178).
	~MSSQLStatementConnection() noexcept;

	MSSQLStatementConnection(const MSSQLStatementConnection &) = delete;
	MSSQLStatementConnection &operator=(const MSSQLStatementConnection &) = delete;

	//! The statement's connection: taken on the first call through
	//! ConnectionProvider (so it is the pinned one inside a DuckDB transaction),
	//! with BEGIN TRANSACTION sent in autocommit; returned as is on every later
	//! call. Throws IOException when none can be had.
	std::shared_ptr<tds::TdsConnection> Acquire(ClientContext &context, MSSQLCatalog &catalog);

	//! Does the statement run inside a DuckDB transaction (on its pinned
	//! connection)? Meaningful after Acquire.
	bool IsPinned() const {
		return transaction_pinned_;
	}

	//! After the last batch: COMMIT in autocommit, then the connection goes back
	//! through ConnectionProvider (a no-op for the pinned one). Throws if the
	//! server refuses the commit — the transaction is then rolled back and the
	//! connection released before the throw.
	void Commit(ClientContext &context, MSSQLCatalog &catalog);

	//! On a failed batch: ROLLBACK in autocommit and the connection goes back.
	//! A connection left mid-response is closed rather than returned. Never
	//! throws; a no-op when nothing was acquired.
	void Fail(ClientContext &context, MSSQLCatalog &catalog) noexcept;

	//! Spec 077 W2: `SET IDENTITY_INSERT [schema].[table] ON` on the statement's
	//! connection, once, before its first batch — the INSERT names the identity
	//! column, and without this the server refuses the value (error 544). It
	//! is session state, so every path that lets go of the connection turns it
	//! OFF again or discards the connection: Commit (before COMMIT), Fail
	//! (after ROLLBACK), and the destructor's ReleaseWithoutContext (bounded by
	//! a timeout, since that path has no ClientContext to carry one — issue
	//! #178). A refusal is rethrown with the server's error explained
	//! (1088 ALTER, 8106 stale catalog, 8107 another table) — see
	//! dml/mssql_identity_insert.hpp. Must be called after Acquire.
	void EnableIdentityInsert(const string &schema_name, const string &table_name);

	//! Whether this statement turned IDENTITY_INSERT on (and not yet off).
	bool IdentityInsertOn() const {
		return identity_insert_on_;
	}

private:
	void ReleaseWithoutContext() noexcept;
	//! Send the OFF for the table ON was sent for. Returns false when the
	//! server did not confirm it (any error, a non-Idle connection, a timeout):
	//! the caller then must not return the connection to the pool.
	bool DisableIdentityInsert(int timeout_ms) noexcept;

	std::shared_ptr<tds::TdsConnection> connection_;
	mssql::LoadTransaction transaction_;
	bool transaction_pinned_ = false;
	bool identity_insert_on_ = false;
	string identity_schema_;
	string identity_table_;
	//! Captured at Acquire for the destructor (issue #178).
	weak_ptr<tds::ConnectionPool> pool_handle_;
	bool reset_on_release_ = tds::DEFAULT_RESET_CONNECTION;
};

}  // namespace duckdb
