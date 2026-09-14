#include "dml/mssql_statement_connection.hpp"

#include "catalog/mssql_catalog.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

MSSQLStatementConnection::~MSSQLStatementConnection() noexcept {
	ReleaseWithoutContext();
}

std::shared_ptr<tds::TdsConnection> MSSQLStatementConnection::Acquire(ClientContext &context, MSSQLCatalog &catalog) {
	if (connection_) {
		return connection_;
	}
	auto connection = ConnectionProvider::GetConnection(context, catalog);
	if (!connection) {
		throw IOException("Failed to acquire a connection for the statement");
	}
	transaction_pinned_ = ConnectionProvider::IsInTransaction(context, catalog);
	pool_handle_ = catalog.GetConnectionPoolHandle();
	reset_on_release_ = ConnectionProvider::ShouldResetOnRelease(context);
	try {
		transaction_.Begin(connection, transaction_pinned_);
	} catch (...) {
		ConnectionProvider::ReleaseConnection(context, catalog, std::move(connection));
		throw;
	}
	connection_ = std::move(connection);
	return connection_;
}

void MSSQLStatementConnection::Commit(ClientContext &context, MSSQLCatalog &catalog) {
	if (!connection_) {
		return;
	}
	try {
		transaction_.Commit();
	} catch (...) {
		Fail(context, catalog);
		throw;
	}
	ConnectionProvider::ReleaseConnection(context, catalog, std::move(connection_));
	connection_.reset();
}

void MSSQLStatementConnection::Fail(ClientContext &context, MSSQLCatalog &catalog) noexcept {
	if (!connection_) {
		return;
	}
	transaction_.Rollback();
	try {
		if (!transaction_pinned_ && connection_->GetState() != tds::ConnectionState::Idle &&
			connection_->GetState() != tds::ConnectionState::Disconnected) {
			// Mid-response: nothing can be sent down it, and nothing should be
			// read from it by the next user. Closing ends the session — and with
			// it any transaction the rollback above could not reach.
			//
			// Never for the PINNED connection: it is the DuckDB transaction's,
			// and non-Idle there may mean another consumer's stream (spec 062
			// self-review) -- closing it would take the transaction down with
			// this statement. It is dropped as it is, which is what the
			// executors did before W1c; the transaction's own ROLLBACK deals
			// with it, and the pool closes a connection that cannot take one.
			connection_->Close();
		}
		ConnectionProvider::ReleaseConnection(context, catalog, std::move(connection_));
	} catch (...) {
		// Dropped: the shared_ptr destructor closes the socket regardless.
	}
	connection_.reset();
}

void MSSQLStatementConnection::ReleaseWithoutContext() noexcept {
	if (!connection_) {
		return;
	}
	transaction_.Rollback();
	// Closes a non-Idle connection, leaves a pinned one to its transaction, and
	// returns a pool connection with the reset flag captured at Acquire.
	mssql::ReleaseBcpConnectionOnError(connection_, pool_handle_, transaction_pinned_, reset_on_release_);
}

}  // namespace duckdb
