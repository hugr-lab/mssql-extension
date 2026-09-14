#include "copy/load_transaction.hpp"

#include <vector>

#include "duckdb/common/exception.hpp"
#include "query/mssql_simple_query.hpp"
#include "tds/tds_socket.hpp"
#include "tds/tds_token_parser.hpp"

namespace duckdb {
namespace mssql {

LoadTransaction::~LoadTransaction() noexcept {
	Rollback();
}

void LoadTransaction::Begin(std::shared_ptr<tds::TdsConnection> connection, bool transaction_pinned) {
	if (transaction_pinned || !connection) {
		// The DuckDB transaction owns the pinned connection: its BEGIN happened
		// at the DuckDB BEGIN, its COMMIT / ROLLBACK are the user's.
		return;
	}
	// The same sequence ConnectionProvider runs for a pinned connection: send
	// BEGIN TRANSACTION, read the whole response, and take the 8-byte
	// transaction descriptor out of its ENVCHANGE -- every later request on
	// this connection must carry it in its TDS headers, or the server refuses
	// it with error 3989 ("should come with valid transaction descriptor").
	// TdsConnection::ExecuteBatch adds the header while the connection holds a
	// descriptor.
	if (!connection->ExecuteBatch("BEGIN TRANSACTION")) {
		throw IOException("could not begin the load's transaction: %s", connection->GetLastError());
	}
	auto *socket = connection->GetSocket();
	std::vector<uint8_t> response;
	if (!socket || !socket->ReceiveMessage(response, 5000)) {
		throw IOException("could not begin the load's transaction: %s",
						  socket ? socket->GetLastError() : std::string("socket is null"));
	}
	uint8_t descriptor[8];
	if (!tds::FindBeginTxnDescriptor(response.data(), response.size(), descriptor)) {
		connection->TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
		throw IOException("could not begin the load's transaction: the server sent no transaction descriptor");
	}
	connection->SetTransactionDescriptor(descriptor);
	connection->TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
	connection_ = std::move(connection);
	open_ = true;
}

void LoadTransaction::Commit() {
	if (!open_) {
		return;
	}
	auto result = MSSQLSimpleQuery::Execute(*connection_, "COMMIT TRANSACTION");
	if (!result.success) {
		throw IOException("could not commit the load's transaction: %s", result.error_message);
	}
	open_ = false;
	connection_->ClearTransactionDescriptor();
	connection_.reset();
}

void LoadTransaction::Rollback() noexcept {
	if (!open_) {
		return;
	}
	open_ = false;
	if (connection_ && connection_->GetState() == tds::ConnectionState::Idle) {
		try {
			MSSQLSimpleQuery::Execute(*connection_, "ROLLBACK TRANSACTION");
		} catch (...) {
			// The connection is about to be closed or reset either way; both end
			// the transaction on the server.
		}
	}
	if (connection_) {
		connection_->ClearTransactionDescriptor();
	}
	connection_.reset();
}

}  // namespace mssql
}  // namespace duckdb
