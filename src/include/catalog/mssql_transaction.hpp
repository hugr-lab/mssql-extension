#pragma once

#include <memory>
#include <mutex>
#include "catalog/mssql_transaction_metadata.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {

namespace tds {
class TdsConnection;
}  // namespace tds

class MSSQLCatalog;
class MSSQLTransactionManager;

//===----------------------------------------------------------------------===//
// MSSQLTransaction - Transaction for MSSQL catalog with SQL Server transaction support
//===----------------------------------------------------------------------===//

class MSSQLTransaction : public Transaction {
public:
	//! Take the pin lock for the whole lazy-pin sequence — see pin_mutex_.
	//!
	//! Hands back the guard rather than the mutex so a caller cannot hold it
	//! out of order or forget to release it: the lock order this class needs
	//! (pin_mutex_ before connection_mutex_) is unenforceable if the mutex
	//! itself is public, which is what the #357 review pointed out.
	std::unique_lock<mutex> LockForPin() const {
		return std::unique_lock<mutex>(pin_mutex_);
	}

	MSSQLTransaction(TransactionManager &manager, ClientContext &context, MSSQLCatalog &catalog);
	~MSSQLTransaction() override;

	MSSQLCatalog &GetCatalog() {
		return catalog_;
	}

	static MSSQLTransaction &Get(ClientContext &context, Catalog &catalog);

	//! `mssql_reset_connection` as it stood when this transaction began
	//! (issue #189). Captured rather than read at COMMIT/ROLLBACK because
	//! TransactionManager::RollbackTransaction receives NO ClientContext — and a
	//! rule that held on COMMIT but not on ROLLBACK would be worse than either
	//! answer alone. Reading it at BEGIN is also the semantics one would choose:
	//! the transaction behaves as it was configured when it started.
	bool ShouldResetOnRelease() const {
		return reset_on_release_;
	}

	//===--------------------------------------------------------------------===//
	// Transaction support (Spec 001-mssql-transactions)
	//===--------------------------------------------------------------------===//

	//! Get the pinned connection for this transaction (may be nullptr if not pinned yet)
	std::shared_ptr<tds::TdsConnection> GetPinnedConnection();

	//! Check if this transaction has a pinned connection
	bool HasPinnedConnection() const;

	//! Check if SQL Server transaction has been started on the pinned connection
	bool IsSqlServerTransactionActive() const;

	//! Set the pinned connection for this transaction
	//! Called by ConnectionProvider on first DML/scan operation
	void SetPinnedConnection(std::shared_ptr<tds::TdsConnection> conn);

	//! Mark SQL Server transaction as started
	void SetSqlServerTransactionActive(bool active);

	//! Get the transaction descriptor (8 bytes) returned by SQL Server
	//! Returns pointer to 8 bytes, or nullptr if not set
	const uint8_t *GetTransactionDescriptor() const;

	//! Set the transaction descriptor from ENVCHANGE response
	void SetTransactionDescriptor(const uint8_t *descriptor);

	//! Generate next savepoint name (for future savepoint support)
	string GetNextSavepointName();

	//! Issue #451: an autocommit statement that writes to this catalog runs on
	//! it as a transaction of one statement -- one pinned connection, BEGIN at
	//! its first use, COMMIT / ROLLBACK when DuckDB commits or rolls back the
	//! statement's own transaction -- as postgres_scanner and mysql_scanner do
	//! for every DuckDB transaction. Without it the server COMMIT came at the
	//! write's Finalize, and a result DuckDB then aborted (a dropped handle,
	//! QueryResult::Close since DuckDB ef5b57f94ad) left an MSSQL table changed
	//! and a DuckDB one not. Set by ConnectionProvider::JoinStatementTransaction
	//! when the write's sink is initialised -- before the scans of its pipeline,
	//! which then take the pinned connection -- and never cleared: the
	//! transaction lives exactly as long as the statement.
	void JoinStatement();
	bool IsStatementJoined() const;

	//! Whether GetConnection sent `transaction_isolation`'s SET before BEGIN
	//! (issue #331), so COMMIT / ROLLBACK put the level back only then. A
	//! statement-joined transaction sends none: autocommit keeps its level.
	void SetIsolationSent();
	bool IsolationSent() const;

	//! The pinned connection carries session state that must not reach the next
	//! user -- an IDENTITY_INSERT that could not be turned off (spec 077 W2) --
	//! so COMMIT / ROLLBACK close it instead of pooling it.
	void DiscardConnectionAtEnd();
	bool ShouldDiscardConnection() const;

	//! The catalog metadata this transaction loaded or changed (issue #380).
	//! Created on first use; dies with the transaction, so nothing of it
	//! outlives COMMIT or ROLLBACK.
	MSSQLTransactionMetadata &Metadata(ClientContext &context);
	//! The same, or null when this transaction never touched metadata.
	MSSQLTransactionMetadata *TryMetadata();

private:
	MSSQLCatalog &catalog_;

	//===--------------------------------------------------------------------===//
	// Transaction state fields (Spec 001-mssql-transactions)
	//===--------------------------------------------------------------------===//

	//! Pinned SQL Server connection for this transaction
	//! nullptr when not in transaction or autocommit mode
	std::shared_ptr<tds::TdsConnection> pinned_connection_;

	//! See ShouldResetOnRelease().
	bool reset_on_release_;

	//! Mutex for serializing concurrent operations on pinned connection
	mutable mutex connection_mutex_;

	//! Held across the whole "is there a pinned connection; if not, take one
	//! from the pool, BEGIN on it, and publish it" sequence in
	//! ConnectionProvider::GetConnection — see issue #356.
	//!
	//! Two things go wrong without it, and only one of them is obvious.
	//! Publishing the connection before BEGIN has completed lets another
	//! thread — DuckDB initialises a plan's source and sink on different
	//! threads — take the early return and execute on a connection that is
	//! mid-BEGIN, which is the intermittent
	//! `Cannot execute: connection not in Idle state`. Publishing it after,
	//! without this lock, is worse: two threads both miss it, both acquire from
	//! the pool and both send BEGIN, and one connection is left pinned while
	//! the other leaks with an open transaction.
	//!
	//! LOCK ORDER: this one first, `connection_mutex_` second — the accessors
	//! take that one inside themselves and never reach for this. Neither mutex
	//! is reachable from outside: this one only through LockForPin(), and
	//! connection_mutex_ not at all.
	mutable mutex pin_mutex_;

	//! True if BEGIN TRANSACTION has been sent to SQL Server
	bool sql_server_transaction_active_ = false;

	//! Transaction descriptor returned by SQL Server (8 bytes)
	//! Required for ALL_HEADERS in subsequent SQL_BATCH requests
	uint8_t transaction_descriptor_[8] = {0};

	//! Whether transaction descriptor has been set
	bool has_transaction_descriptor_ = false;

	//! Counter for generating unique savepoint names
	uint32_t savepoint_counter_ = 0;

	//! See JoinStatement() / SetIsolationSent(), under connection_mutex_.
	bool statement_joined_ = false;
	bool isolation_sent_ = false;
	bool discard_connection_ = false;

	mutable mutex metadata_mutex_;
	unique_ptr<MSSQLTransactionMetadata> metadata_;

public:
	//! The materialize lock of this transaction's scans and sinks (see
	//! MSSQLCatalog::MaterializeMutexFor): they share its one pinned connection,
	//! and nothing else does (issue #409).
	std::mutex &MaterializeMutex() {
		return materialize_mutex_;
	}

private:
	std::mutex materialize_mutex_;
};

//===----------------------------------------------------------------------===//
// MSSQLTransactionManager - Transaction manager for read-only MSSQL catalog
//===----------------------------------------------------------------------===//

class MSSQLTransactionManager : public TransactionManager {
public:
	MSSQLTransactionManager(AttachedDatabase &db, MSSQLCatalog &catalog);
	~MSSQLTransactionManager() override;

	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;
	void Checkpoint(ClientContext &context, bool force = false) override;

private:
	MSSQLCatalog &catalog_;
	mutex transaction_lock_;
	reference_map_t<ClientContext, unique_ptr<MSSQLTransaction>> transactions_;
};

}  // namespace duckdb
