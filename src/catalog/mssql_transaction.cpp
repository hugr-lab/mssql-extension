#include "catalog/mssql_transaction.hpp"
#include <cstring>
#include "catalog/mssql_catalog.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "query/mssql_simple_query.hpp"
#include "tds/tds_connection.hpp"
#include "tds/tds_connection_pool.hpp"
#include "tds/tds_socket.hpp"

#include <cstdio>
#include <cstdlib>

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetTxnDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
	return level;
}

#define MSSQL_TXN_LOG(fmt, ...)                                      \
	do {                                                             \
		if (GetTxnDebugLevel() >= 1) {                               \
			fprintf(stderr, "[MSSQL_TXN] " fmt "\n", ##__VA_ARGS__); \
		}                                                            \
	} while (0)

namespace {

//===----------------------------------------------------------------------===//
// Helper: put back an isolation level the transaction's BEGIN set (issue #331)
//===----------------------------------------------------------------------===//

// SQL Server keeps a session's isolation level across the RESET_CONNECTION the
// pool sends on reuse (measured: SNAPSHOT still in force on the next autocommit
// statement, the reset having run -- its #temp table gone), so a level set
// before BEGIN would follow the connection into every later autocommit
// statement and DML load. Sent on its own, after the transaction descriptor is
// cleared: appended to the COMMIT / ROLLBACK batch it does not run when the
// server has already aborted the transaction (a snapshot update conflict,
// 3960) -- that batch goes out with the stale descriptor and the SET is lost.
// A connection whose level could not be put back is closed, never pooled.
void RestoreIsolationLevel(duckdb::MSSQLCatalog &catalog, duckdb::tds::TdsConnection &conn) {
	auto restore = catalog.TransactionIsolationRestoreStatement();
	if (restore.empty()) {
		return;
	}
	auto result = duckdb::MSSQLSimpleQuery::Execute(conn, restore);
	if (!result.success) {
		MSSQL_TXN_LOG("RestoreIsolationLevel: %s failed (%s) -- closing the connection", restore.c_str(),
					  result.error_message.c_str());
		conn.Close();
	}
}

// Issue #380: the transaction's own metadata goes with it (it is owned by the
// MSSQLTransaction erased right after); what the shared cache must forget --
// the names the transaction changed -- is invalidated there now, at COMMIT and
// at ROLLBACK alike. After COMMIT another connection may hold the pre-commit
// state of those names; after ROLLBACK the invalidation is merely redundant.
void ForgetTransactionMetadata(duckdb::MSSQLCatalog &catalog, duckdb::MSSQLTransaction &txn) {
	auto *metadata = txn.TryMetadata();
	if (metadata) {
		catalog.ForgetTransactionChanges(*metadata);
	}
}

// Issue #383: what the transaction loaded itself, and did not change, is
// committed state -- publish it into the shared cache before the transaction's
// metadata goes. In memory only: no round trip on COMMIT's or ROLLBACK's path.
void PublishTransactionMetadata(duckdb::MSSQLCatalog &catalog, duckdb::MSSQLTransaction &txn) {
	auto *metadata = txn.TryMetadata();
	if (metadata) {
		catalog.PublishTransactionMetadata(*metadata);
	}
}

}  // anonymous namespace

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLTransaction Implementation
//===----------------------------------------------------------------------===//

MSSQLTransaction::MSSQLTransaction(TransactionManager &manager, ClientContext &context, MSSQLCatalog &catalog)
	: Transaction(manager, context),
	  catalog_(catalog),
	  pinned_connection_(nullptr),
	  reset_on_release_(ConnectionProvider::ShouldResetOnRelease(context)),
	  sql_server_transaction_active_(false),
	  savepoint_counter_(0) {}

MSSQLTransaction::~MSSQLTransaction() {
	// If we still have a pinned connection with an active SQL Server transaction,
	// it means the transaction was abandoned (DuckDB crashed or didn't properly commit/rollback).
	// We simply close the connection - SQL Server will automatically rollback when the
	// connection is closed. We don't try to execute ROLLBACK here because during shutdown
	// the socket or other resources may already have been destroyed.
	if (pinned_connection_ && sql_server_transaction_active_) {
		MSSQL_TXN_LOG("WARNING: Abandoned transaction detected in destructor, closing connection");
		try {
			// Just close the connection - SQL Server will auto-rollback
			pinned_connection_->Close();
		} catch (...) {
			// Ignore errors during cleanup - we're in a destructor
			MSSQL_TXN_LOG("WARNING: Error closing connection during abandoned transaction cleanup");
		}
		sql_server_transaction_active_ = false;
	}
	// Spec 047: every SetPinnedConnection(non-null) on `pinned_connection_`
	// incremented the per-catalog `pinned_count_` atomic. The destructor path
	// (commit/rollback/crash/abandoned) drops the shared_ptr WITHOUT going
	// through SetPinnedConnection(nullptr), so the matching DecrementPinned
	// would never fire and `pool_stats.pinned_count` would leak +1 forever.
	// Decrement here under try/catch — the catalog is still alive (transaction
	// destruction precedes catalog destruction in DuckDB's teardown order).
	if (pinned_connection_) {
		try {
			catalog_.GetConnectionPool().DecrementPinned();
		} catch (...) {
			// Catalog or pool already destroyed (unexpected order) — accept the
			// stat leak rather than escape the destructor.
		}
	}
	// Note: pinned_connection_ shared_ptr will be released here, decreasing ref count.
	// The connection will be destroyed if this was the last reference.
}

MSSQLTransaction &MSSQLTransaction::Get(ClientContext &context, Catalog &catalog) {
	return Transaction::Get(context, catalog).Cast<MSSQLTransaction>();
}

MSSQLTransactionMetadata &MSSQLTransaction::Metadata(ClientContext &context) {
	lock_guard<mutex> lock(metadata_mutex_);
	if (!metadata_) {
		metadata_ = make_uniq<MSSQLTransactionMetadata>(catalog_.CreateTransactionMetadataCache(context),
														catalog_.GetMetadataCache().GetInvalidationEpoch());
	}
	return *metadata_;
}

MSSQLTransactionMetadata *MSSQLTransaction::TryMetadata() {
	lock_guard<mutex> lock(metadata_mutex_);
	return metadata_.get();
}

std::shared_ptr<tds::TdsConnection> MSSQLTransaction::GetPinnedConnection() {
	lock_guard<mutex> lock(connection_mutex_);
	return pinned_connection_;
}

bool MSSQLTransaction::HasPinnedConnection() const {
	lock_guard<mutex> lock(connection_mutex_);
	return pinned_connection_ != nullptr;
}

bool MSSQLTransaction::IsSqlServerTransactionActive() const {
	lock_guard<mutex> lock(connection_mutex_);
	return sql_server_transaction_active_;
}

void MSSQLTransaction::SetPinnedConnection(std::shared_ptr<tds::TdsConnection> conn) {
	lock_guard<mutex> lock(connection_mutex_);

	// Track pinned connection count for pool statistics
	bool was_pinned = (pinned_connection_ != nullptr);
	bool will_be_pinned = (conn != nullptr);

	if (!was_pinned && will_be_pinned) {
		// Pinning a connection - increment count on the per-catalog pool (spec 047 T014).
		catalog_.GetConnectionPool().IncrementPinned();
		MSSQL_TXN_LOG("Pinned connection set for transaction (pinned_count incremented)");
	} else if (was_pinned && !will_be_pinned) {
		// Unpinning a connection - decrement count.
		catalog_.GetConnectionPool().DecrementPinned();
		MSSQL_TXN_LOG("Pinned connection cleared for transaction (pinned_count decremented)");
	} else {
		MSSQL_TXN_LOG("Pinned connection set for transaction (no count change)");
	}

	pinned_connection_ = std::move(conn);
}

void MSSQLTransaction::SetSqlServerTransactionActive(bool active) {
	lock_guard<mutex> lock(connection_mutex_);
	sql_server_transaction_active_ = active;
	MSSQL_TXN_LOG("SQL Server transaction active: %s", active ? "true" : "false");
}

const uint8_t *MSSQLTransaction::GetTransactionDescriptor() const {
	lock_guard<mutex> lock(connection_mutex_);
	if (!has_transaction_descriptor_) {
		return nullptr;
	}
	return transaction_descriptor_;
}

void MSSQLTransaction::SetTransactionDescriptor(const uint8_t *descriptor) {
	lock_guard<mutex> lock(connection_mutex_);
	if (descriptor) {
		std::memcpy(transaction_descriptor_, descriptor, 8);
		has_transaction_descriptor_ = true;
		MSSQL_TXN_LOG("Transaction descriptor set: %02x %02x %02x %02x %02x %02x %02x %02x", transaction_descriptor_[0],
					  transaction_descriptor_[1], transaction_descriptor_[2], transaction_descriptor_[3],
					  transaction_descriptor_[4], transaction_descriptor_[5], transaction_descriptor_[6],
					  transaction_descriptor_[7]);
	} else {
		std::memset(transaction_descriptor_, 0, 8);
		has_transaction_descriptor_ = false;
		MSSQL_TXN_LOG("Transaction descriptor cleared");
	}
}

string MSSQLTransaction::GetNextSavepointName() {
	lock_guard<mutex> lock(connection_mutex_);
	return "sp_" + std::to_string(++savepoint_counter_);
}

void MSSQLTransaction::JoinStatement() {
	lock_guard<mutex> lock(connection_mutex_);
	statement_joined_ = true;
}

bool MSSQLTransaction::IsStatementJoined() const {
	lock_guard<mutex> lock(connection_mutex_);
	return statement_joined_;
}

void MSSQLTransaction::SetIsolationSent() {
	lock_guard<mutex> lock(connection_mutex_);
	isolation_sent_ = true;
}

bool MSSQLTransaction::IsolationSent() const {
	lock_guard<mutex> lock(connection_mutex_);
	return isolation_sent_;
}

void MSSQLTransaction::DiscardConnectionAtEnd() {
	lock_guard<mutex> lock(connection_mutex_);
	discard_connection_ = true;
}

bool MSSQLTransaction::ShouldDiscardConnection() const {
	lock_guard<mutex> lock(connection_mutex_);
	return discard_connection_;
}

//===----------------------------------------------------------------------===//
// MSSQLTransactionManager Implementation
//===----------------------------------------------------------------------===//

MSSQLTransactionManager::MSSQLTransactionManager(AttachedDatabase &db, MSSQLCatalog &catalog)
	: TransactionManager(db), catalog_(catalog) {}

MSSQLTransactionManager::~MSSQLTransactionManager() = default;

Transaction &MSSQLTransactionManager::StartTransaction(ClientContext &context) {
	lock_guard<mutex> lock(transaction_lock_);

	MSSQL_TXN_LOG("StartTransaction: context=%p, is_autocommit=%d", (void *)&context,
				  context.transaction.IsAutoCommit());

	auto transaction = make_uniq<MSSQLTransaction>(*this, context, catalog_);
	auto &result = *transaction;
	MSSQL_TXN_LOG("StartTransaction: created MSSQLTransaction=%p", (void *)&result);
	transactions_[context] = std::move(transaction);
	return result;
}

//! Ends the transaction's server transaction on its pinned connection -- COMMIT
//! or ROLLBACK -- and gives the connection back. Returns the server's refusal of
//! a COMMIT, empty otherwise.
//!
//! The answer is read (MSSQLSimpleQuery, as mssql::LoadTransaction does), not
//! drained: a refused COMMIT used to count as done. Since issue #451 every
//! autocommit write commits here, so it matters. A COMMIT the server refused is
//! followed by a ROLLBACK; a ROLLBACK it did not confirm -- refused, timed out,
//! the connection mid-response -- closes the connection, which ends the session
//! and with it the transaction, rather than pooling one in an unknown state.
//! The connection goes back to the pool on every path (a failed COMMIT used to
//! keep it out of the pool for good).
//!
//! No transaction_lock_ here: StartTransaction takes it for every statement on
//! the catalog, and holding it across a round trip queued them all behind this
//! COMMIT.
static string EndServerTransaction(MSSQLCatalog &catalog, MSSQLTransaction &txn, bool commit) {
	auto pinned_conn = txn.GetPinnedConnection();
	if (!pinned_conn || !txn.IsSqlServerTransactionActive()) {
		MSSQL_TXN_LOG("%s: No active SQL Server transaction (no-op)",
					  commit ? "CommitTransaction" : "RollbackTransaction");
		return string();
	}
	string commit_error;
	bool rolled_back = !commit;
	bool confirmed = false;
	if (commit) {
		MSSQL_TXN_LOG("CommitTransaction: Committing SQL Server transaction");
		bool server_refused = false;
		bool sent = false;
		if (pinned_conn->GetState() == tds::ConnectionState::Idle) {
			sent = true;
			try {
				auto result = MSSQLSimpleQuery::Execute(*pinned_conn, "COMMIT TRANSACTION");
				confirmed = result.success;
				if (!result.success) {
					commit_error = result.DescribeError();
					server_refused = result.error_number != 0;
				}
			} catch (std::exception &ex) {
				commit_error = ex.what();
			}
		} else {
			commit_error = string("the connection is ") + tds::ConnectionStateToString(pinned_conn->GetState());
		}
		if (!confirmed) {
			// A refusal leaves nothing committed, and so does a COMMIT never sent
			// (the server rolls back a session that ends). No answer at all -- a
			// timeout, a dropped connection -- does not say whether the COMMIT
			// reached the server before the connection went.
			if (!sent) {
				commit_error = "the COMMIT could not be sent (" + commit_error + "), nothing was written";
			} else if (server_refused) {
				commit_error = "the server refused the COMMIT, nothing was written: " + commit_error;
			} else {
				commit_error = "no answer to the COMMIT (" + commit_error +
							   "): the server may or may not have committed; check the data";
			}
		}
		if (!confirmed) {
			MSSQL_TXN_LOG("CommitTransaction: COMMIT TRANSACTION failed: %s", commit_error.c_str());
			rolled_back = true;
		}
	}
	if (rolled_back) {
		MSSQL_TXN_LOG("%s: Rolling back SQL Server transaction", commit ? "CommitTransaction" : "RollbackTransaction");
		confirmed = false;
		bool server_answered = false;
		if (pinned_conn->GetState() == tds::ConnectionState::Idle) {
			try {
				auto result = MSSQLSimpleQuery::Execute(*pinned_conn, "IF @@TRANCOUNT > 0 ROLLBACK TRANSACTION");
				confirmed = result.success;
				server_answered = result.error_number != 0;
			} catch (...) {
				confirmed = false;
			}
		}
		if (!confirmed && server_answered && pinned_conn->GetState() == tds::ConnectionState::Idle) {
			// The server refused it -- typically because it had aborted the
			// transaction already (an update conflict, 3960) and the descriptor is
			// stale. Without the descriptor, ask whether anything is still open;
			// a clean session keeps its place in the pool (issue #331's restore
			// below runs on it).
			pinned_conn->ClearTransactionDescriptor();
			try {
				auto open = MSSQLSimpleQuery::Execute(*pinned_conn, "SELECT @@TRANCOUNT");
				confirmed = open.success && open.rows.size() == 1 && !open.rows[0].empty() && open.rows[0][0] == "0";
			} catch (...) {
				confirmed = false;
			}
		}
		if (!confirmed || txn.ShouldDiscardConnection()) {
			MSSQL_TXN_LOG(
				"WARNING: ROLLBACK not confirmed, or the session must not be reused -- closing the connection");
			try {
				pinned_conn->Close();
			} catch (...) {
				// The shared_ptr destructor closes the socket regardless.
			}
		}
	}

	if (confirmed && !rolled_back && txn.ShouldDiscardConnection()) {
		// IDENTITY_INSERT left on (spec 077 W2): the session goes, not back to the pool.
		MSSQL_TXN_LOG("CommitTransaction: the session must not be reused -- closing the connection");
		try {
			pinned_conn->Close();
		} catch (...) {
		}
	}
	txn.SetSqlServerTransactionActive(false);
	pinned_conn->ClearTransactionDescriptor();
	if (confirmed && txn.IsolationSent() && pinned_conn->GetState() == tds::ConnectionState::Idle) {
		RestoreIsolationLevel(catalog, *pinned_conn);
	}

	// Flag connection for reset — RESET_CONNECTION will be set on next SQL_BATCH TDS header.
	//
	// Honours `mssql_reset_connection` like every other release path
	// (issue #189). A setting that meant "I own this session's state"
	// everywhere EXCEPT after COMMIT would leave a user watching their
	// temp objects survive in autocommit and vanish here, with nothing
	// to read that explains it. The transaction itself is already
	// resolved at this point — COMMIT or ROLLBACK ran, and the descriptor is
	// cleared above — so what the reset drops here is session state,
	// which is exactly what the setting is about.
	const bool reset_on_release = txn.ShouldResetOnRelease();
	MSSQL_TXN_LOG("%s: reset=%d, returning connection to pool", commit ? "CommitTransaction" : "RollbackTransaction",
				  reset_on_release ? 1 : 0);
	pinned_conn->SetNeedsReset(reset_on_release);
	catalog.GetConnectionPool().Release(pinned_conn);
	txn.SetPinnedConnection(nullptr);
	return commit_error;
}

ErrorData MSSQLTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	auto &mssql_txn = transaction.Cast<MSSQLTransaction>();

	MSSQL_TXN_LOG("CommitTransaction: context=%p, txn=%p, has_pinned=%d, sql_txn_active=%d, statement=%d",
				  (void *)&context, (void *)&mssql_txn, mssql_txn.HasPinnedConnection(),
				  mssql_txn.IsSqlServerTransactionActive(), mssql_txn.IsStatementJoined());

	auto commit_error = EndServerTransaction(catalog_, mssql_txn, true);
	if (!commit_error.empty()) {
		// DuckDB marks this transaction rolled back and does not call
		// RollbackTransaction for it (MetaTransaction::Commit), so everything
		// is cleaned up above and here.
		ForgetTransactionMetadata(catalog_, mssql_txn);
		lock_guard<mutex> lock(transaction_lock_);
		transactions_.erase(context);
		return ErrorData(ExceptionType::IO, "MSSQL: Failed to commit transaction: " + commit_error);
	}

	PublishTransactionMetadata(catalog_, mssql_txn);
	ForgetTransactionMetadata(catalog_, mssql_txn);
	lock_guard<mutex> lock(transaction_lock_);
	transactions_.erase(context);
	return ErrorData();
}

void MSSQLTransactionManager::RollbackTransaction(Transaction &transaction) {
	auto &mssql_txn = transaction.Cast<MSSQLTransaction>();

	MSSQL_TXN_LOG("RollbackTransaction: txn=%p, has_pinned=%d, sql_txn_active=%d, statement=%d", (void *)&mssql_txn,
				  mssql_txn.HasPinnedConnection(), mssql_txn.IsSqlServerTransactionActive(),
				  mssql_txn.IsStatementJoined());

	EndServerTransaction(catalog_, mssql_txn, false);

	PublishTransactionMetadata(catalog_, mssql_txn);
	ForgetTransactionMetadata(catalog_, mssql_txn);

	// Try to get the context to remove from our transaction map
	// The context may have been destroyed during shutdown, in which case
	// lock() returns an empty shared_ptr
	auto context_ptr = transaction.context.lock();
	if (context_ptr) {
		lock_guard<mutex> lock(transaction_lock_);
		transactions_.erase(*context_ptr);
	}
	// If context is gone, the transaction map entry will be cleaned up when
	// the TransactionManager is destroyed
}

void MSSQLTransactionManager::Checkpoint(ClientContext &context, bool force) {
	// Read-only external catalog - checkpoint is a no-op
}

}  // namespace duckdb
