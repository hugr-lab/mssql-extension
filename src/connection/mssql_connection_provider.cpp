#include "connection/mssql_connection_provider.hpp"

#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_transaction.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "tds/tds_connection.hpp"
#include "tds/tds_connection_pool.hpp"
#include "tds/tds_socket.hpp"

#ifdef MSSQL_DEBUG
#include <iostream>
#define MSSQL_CONN_LOG(msg) std::cerr << "[MSSQL_CONN] " << msg << std::endl
#else
#define MSSQL_CONN_LOG(msg)
#endif

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

//===----------------------------------------------------------------------===//
// ConnectionProvider::GetConnection
//===----------------------------------------------------------------------===//

std::shared_ptr<tds::TdsConnection> ConnectionProvider::GetConnection(ClientContext &context, MSSQLCatalog &catalog,
                                                                      int timeout_ms) {
	auto *txn = TryGetMSSQLTransaction(context, catalog);

	// Check if we're in autocommit mode (implicit transaction per statement)
	// In autocommit mode, each statement is independent - no need to pin a connection
	bool is_autocommit = context.transaction.IsAutoCommit();

	if (!txn || is_autocommit) {
		// Not in a transaction OR in autocommit mode - acquire from pool
		MSSQL_CONN_LOG("GetConnection: Autocommit mode, acquiring from pool (txn=" << (txn ? "yes" : "no")
		               << ", autocommit=" << (is_autocommit ? "yes" : "no") << ")");
		auto &pool = catalog.GetConnectionPool();
		auto conn = pool.Acquire(timeout_ms);
		if (!conn) {
			throw IOException("MSSQL: Failed to acquire connection from pool (timeout)");
		}
		return conn;
	}

	// In an explicit DuckDB transaction (BEGIN was issued) - use pinned connection
	MSSQL_CONN_LOG("GetConnection: Explicit transaction mode");

	// Check if we already have a pinned connection
	auto pinned = txn->GetPinnedConnection();
	if (pinned) {
		MSSQL_CONN_LOG("GetConnection: Returning existing pinned connection");
		return pinned;
	}

	// First access in this transaction - need to pin a connection
	MSSQL_CONN_LOG("GetConnection: First access in transaction, acquiring and pinning connection");

	// Acquire connection from pool
	auto &pool = catalog.GetConnectionPool();
	auto conn = pool.Acquire(timeout_ms);
	if (!conn) {
		throw IOException("MSSQL: Failed to acquire connection from pool for transaction (timeout)");
	}

	// Pin the connection to this transaction
	txn->SetPinnedConnection(conn);

	// Start SQL Server transaction lazily (BEGIN TRANSACTION)
	MSSQL_CONN_LOG("GetConnection: Starting SQL Server transaction");

	if (!conn->ExecuteBatch("BEGIN TRANSACTION")) {
		// Failed to start transaction - release connection and throw
		MSSQL_CONN_LOG("GetConnection: ExecuteBatch failed: " << conn->GetLastError());
		txn->SetPinnedConnection(nullptr);
		pool.Release(conn);
		throw IOException("MSSQL: Failed to start SQL Server transaction: " + conn->GetLastError());
	}

	// Receive the complete TDS response (should be a simple DONE token)
	auto *socket = conn->GetSocket();
	if (!socket) {
		txn->SetPinnedConnection(nullptr);
		pool.Release(conn);
		throw IOException("MSSQL: Socket is null after BEGIN TRANSACTION");
	}

	std::vector<uint8_t> response;
	if (!socket->ReceiveMessage(response, 5000)) {
		MSSQL_CONN_LOG("GetConnection: ReceiveMessage failed: " << socket->GetLastError());
		txn->SetPinnedConnection(nullptr);
		conn->Close();
		pool.Release(conn);
		throw IOException("MSSQL: Failed to receive BEGIN TRANSACTION response: " + socket->GetLastError());
	}

	// Parse the response to extract the transaction descriptor from ENVCHANGE token
	// ENVCHANGE token: 0xE3, length (2 bytes LE), type, newlen, newvalue, oldlen, [oldvalue]
	// For BEGIN_TRANS (type 0x08): newvalue is the 8-byte transaction descriptor
	bool found_transaction_descriptor = false;
	size_t offset = 0;
	while (offset < response.size()) {
		uint8_t token_type = response[offset++];

		if (token_type == 0xE3) {  // ENVCHANGE
			if (offset + 2 > response.size()) break;
			uint16_t token_len = response[offset] | (response[offset + 1] << 8);
			offset += 2;

			if (offset >= response.size()) break;
			uint8_t env_type = response[offset++];

			if (env_type == 0x08) {  // BEGIN_TRANS
				if (offset >= response.size()) break;
				uint8_t new_len = response[offset++];
				if (new_len == 8 && offset + 8 <= response.size()) {
					// Store transaction descriptor in both transaction and connection
					// Transaction stores it for reference, connection uses it in ExecuteBatch
					txn->SetTransactionDescriptor(&response[offset]);
					conn->SetTransactionDescriptor(&response[offset]);
					found_transaction_descriptor = true;
					MSSQL_CONN_LOG("GetConnection: Found transaction descriptor");
				}
			}
			// Skip rest of this ENVCHANGE token
			offset += token_len - 1;  // -1 because we already read env_type
		} else if (token_type == 0xFD || token_type == 0xFE || token_type == 0xFF) {  // DONE/DONEPROC/DONEINPROC
			// DONE tokens are 12 bytes total (1 type + 2 status + 2 curcmd + 8 rowcount) but we already read type
			offset += 12;
		} else {
			// Unknown token, skip (this is a simplified parser)
			break;
		}
	}

	if (!found_transaction_descriptor) {
		MSSQL_CONN_LOG("GetConnection: WARNING - No transaction descriptor found in response");
	}

	// Transition connection back to Idle (ExecuteBatch left it in Executing state)
	conn->TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);

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
		// Not in a transaction OR in autocommit mode - return to pool
		MSSQL_CONN_LOG("ReleaseConnection: Autocommit mode, returning to pool");
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

}  // namespace duckdb
