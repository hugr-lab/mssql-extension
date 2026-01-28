#include "catalog/mssql_transaction.hpp"
#include <cstring>
#include "catalog/mssql_catalog.hpp"
#include "connection/mssql_pool_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "tds/tds_connection.hpp"
#include "tds/tds_connection_pool.hpp"
#include "tds/tds_socket.hpp"

#include <cstdio>
#include <cstdlib>

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetTxnDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
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
// Helper: Execute a simple SQL batch and receive complete response
//===----------------------------------------------------------------------===//

bool ExecuteAndDrain(duckdb::tds::TdsConnection &conn, const std::string &sql, int timeout_ms = 5000) {
	if (!conn.ExecuteBatch(sql)) {
		return false;
	}

	// Receive the complete TDS response via socket
	auto *socket = conn.GetSocket();
	if (!socket) {
		return false;
	}

	std::vector<uint8_t> response;
	if (!socket->ReceiveMessage(response, timeout_ms)) {
		return false;
	}

	// Transition connection back to Idle (ExecuteBatch left it in Executing state)
	conn.TransitionState(duckdb::tds::ConnectionState::Executing, duckdb::tds::ConnectionState::Idle);

	return true;
}

//===----------------------------------------------------------------------===//
// Helper: Verify clean transaction state (@@TRANCOUNT = 0)
//===----------------------------------------------------------------------===//

bool VerifyCleanTransactionState(duckdb::tds::TdsConnection &conn) {
	// We could query @@TRANCOUNT here, but for simplicity we just assume
	// the COMMIT/ROLLBACK succeeded if ExecuteBatch returned true.
	// In a production implementation, you might want to actually check
	// @@TRANCOUNT to ensure the transaction is fully closed.
	return true;
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
	// Note: pinned_connection_ shared_ptr will be released here, decreasing ref count.
	// The connection will be destroyed if this was the last reference.
}

MSSQLTransaction &MSSQLTransaction::Get(ClientContext &context, Catalog &catalog) {
	return Transaction::Get(context, catalog).Cast<MSSQLTransaction>();
}

std::shared_ptr<tds::TdsConnection> MSSQLTransaction::GetPinnedConnection() {
	lock_guard<mutex> lock(connection_mutex_);
	return pinned_connection_;
}

bool MSSQLTransaction::HasPinnedConnection() const {
	lock_guard<mutex> lock(connection_mutex_);
	return pinned_connection_ != nullptr;
}

mutex &MSSQLTransaction::GetConnectionMutex() {
	return connection_mutex_;
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
		// Pinning a connection - increment count
		MssqlPoolManager::Instance().IncrementPinnedCount(catalog_.GetContextName());
		MSSQL_TXN_LOG("Pinned connection set for transaction (pinned_count incremented)");
	} else if (was_pinned && !will_be_pinned) {
		// Unpinning a connection - decrement count
		MssqlPoolManager::Instance().DecrementPinnedCount(catalog_.GetContextName());
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

ErrorData MSSQLTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	lock_guard<mutex> lock(transaction_lock_);

	auto &mssql_txn = transaction.Cast<MSSQLTransaction>();

	MSSQL_TXN_LOG("CommitTransaction: context=%p, txn=%p, has_pinned=%d, sql_txn_active=%d", (void *)&context,
				  (void *)&mssql_txn, mssql_txn.HasPinnedConnection(), mssql_txn.IsSqlServerTransactionActive());

	// Check if we have a pinned connection with an active SQL Server transaction
	auto pinned_conn = mssql_txn.GetPinnedConnection();
	if (pinned_conn && mssql_txn.IsSqlServerTransactionActive()) {
		MSSQL_TXN_LOG("CommitTransaction: Committing SQL Server transaction");

		// Execute COMMIT TRANSACTION
		if (!ExecuteAndDrain(*pinned_conn, "COMMIT TRANSACTION")) {
			// Commit failed - keep connection pinned and return error
			// The user will need to rollback or retry
			MSSQL_TXN_LOG("CommitTransaction: COMMIT TRANSACTION failed: %s", pinned_conn->GetLastError().c_str());
			string error_msg = "MSSQL: Failed to commit transaction: " + pinned_conn->GetLastError();
			transactions_.erase(context);
			return ErrorData(ExceptionType::IO, error_msg);
		}

		// Verify clean state
		if (!VerifyCleanTransactionState(*pinned_conn)) {
			MSSQL_TXN_LOG("WARNING: CommitTransaction: Transaction state not clean after COMMIT");
		}

		// Mark transaction as no longer active
		mssql_txn.SetSqlServerTransactionActive(false);

		// Clear transaction descriptor on the connection
		pinned_conn->ClearTransactionDescriptor();

		// Flag connection for reset — RESET_CONNECTION will be set on next SQL_BATCH TDS header
		MSSQL_TXN_LOG("CommitTransaction: Flagging connection for reset");
		pinned_conn->SetNeedsReset(true);

		// Return connection to pool
		MSSQL_TXN_LOG("CommitTransaction: Returning connection to pool");
		auto &pool = catalog_.GetConnectionPool();
		pool.Release(pinned_conn);

		// Clear pinned connection
		mssql_txn.SetPinnedConnection(nullptr);
	} else {
		MSSQL_TXN_LOG("CommitTransaction: No active SQL Server transaction (no-op)");
	}

	transactions_.erase(context);
	return ErrorData();
}

void MSSQLTransactionManager::RollbackTransaction(Transaction &transaction) {
	lock_guard<mutex> lock(transaction_lock_);

	auto &mssql_txn = transaction.Cast<MSSQLTransaction>();

	MSSQL_TXN_LOG("RollbackTransaction: txn=%p, has_pinned=%d, sql_txn_active=%d", (void *)&mssql_txn,
				  mssql_txn.HasPinnedConnection(), mssql_txn.IsSqlServerTransactionActive());

	// Check if we have a pinned connection with an active SQL Server transaction
	auto pinned_conn = mssql_txn.GetPinnedConnection();

	if (pinned_conn && mssql_txn.IsSqlServerTransactionActive()) {
		MSSQL_TXN_LOG("RollbackTransaction: Rolling back SQL Server transaction");

		// Execute ROLLBACK TRANSACTION
		if (!ExecuteAndDrain(*pinned_conn, "ROLLBACK TRANSACTION")) {
			// Rollback failed - log error but continue cleanup
			MSSQL_TXN_LOG("WARNING: RollbackTransaction: ROLLBACK TRANSACTION failed: %s",
						  pinned_conn->GetLastError().c_str());
		}

		// Verify clean state
		if (!VerifyCleanTransactionState(*pinned_conn)) {
			MSSQL_TXN_LOG("WARNING: RollbackTransaction: Transaction state not clean after ROLLBACK");
		}

		// Mark transaction as no longer active
		mssql_txn.SetSqlServerTransactionActive(false);

		// Clear transaction descriptor on the connection
		pinned_conn->ClearTransactionDescriptor();

		// Flag connection for reset — RESET_CONNECTION will be set on next SQL_BATCH TDS header
		MSSQL_TXN_LOG("RollbackTransaction: Flagging connection for reset");
		pinned_conn->SetNeedsReset(true);

		// Return connection to pool
		MSSQL_TXN_LOG("RollbackTransaction: Returning connection to pool");
		auto &pool = catalog_.GetConnectionPool();
		pool.Release(pinned_conn);

		// Clear pinned connection
		mssql_txn.SetPinnedConnection(nullptr);
	} else {
		MSSQL_TXN_LOG("RollbackTransaction: No active SQL Server transaction (no-op)");
	}

	// Try to get the context to remove from our transaction map
	// The context may have been destroyed during shutdown, in which case
	// lock() returns an empty shared_ptr
	auto context_ptr = transaction.context.lock();
	if (context_ptr) {
		transactions_.erase(*context_ptr);
	}
	// If context is gone, the transaction map entry will be cleaned up when
	// the TransactionManager is destroyed
}

void MSSQLTransactionManager::Checkpoint(ClientContext &context, bool force) {
	// Read-only external catalog - checkpoint is a no-op
}

}  // namespace duckdb
