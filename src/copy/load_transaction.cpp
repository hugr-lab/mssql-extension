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
	auto *socket = connection->GetSocket();
	if (!socket) {
		throw IOException("could not begin the load's transaction: socket is null");
	}
	socket->ClearReceiveBuffer();
	if (!connection->ExecuteBatch("BEGIN TRANSACTION")) {
		throw IOException("could not begin the load's transaction: %s", connection->GetLastError());
	}
	std::vector<uint8_t> response;
	if (!socket->ReceiveMessage(response, 5000)) {
		// Sent, not answered: the transaction may be open on the server. The
		// connection is mid-response, so nothing can be sent to undo it; closing
		// is what ends it.
		connection->Close();
		throw IOException("could not begin the load's transaction: %s", socket->GetLastError());
	}
	uint8_t descriptor[8];
	if (!tds::FindBeginTxnDescriptor(response.data(), response.size(), descriptor)) {
		// The scan knows ENVCHANGE and DONE and stops at anything else; a token
		// in front of the ENVCHANGE (an INFO from the reset that rode on this
		// batch, say) hides the descriptor while the transaction HAS begun. The
		// connection cannot be used without the descriptor and must not go back
		// with a transaction open (spec 062 self-review): undo it, then fail.
		connection->TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
		auto undo = MSSQLSimpleQuery::Execute(*connection, "IF @@TRANCOUNT > 0 ROLLBACK TRANSACTION");
		if (!undo.success) {
			connection->Close();
		}
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
	if (connection_) {
		// A ROLLBACK that the server did not confirm -- refused, timed out, or
		// unsendable because the connection is mid-response -- leaves the
		// transaction's state unknown, and a connection in an unknown state
		// does not go back to the pool: closing ends the session, and with it
		// the transaction (spec 062 self-review). Only a confirmed ROLLBACK
		// leaves the connection reusable.
		bool confirmed = false;
		if (connection_->GetState() == tds::ConnectionState::Idle) {
			try {
				confirmed = MSSQLSimpleQuery::Execute(*connection_, "ROLLBACK TRANSACTION").success;
			} catch (...) {
				confirmed = false;
			}
		}
		if (!confirmed) {
			try {
				connection_->Close();
			} catch (...) {
				// The shared_ptr destructor closes the socket regardless.
			}
		}
		connection_->ClearTransactionDescriptor();
	}
	connection_.reset();
}

}  // namespace mssql
}  // namespace duckdb
