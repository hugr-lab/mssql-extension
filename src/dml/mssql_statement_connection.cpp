#include "dml/mssql_statement_connection.hpp"

#include <cstdio>
#include <cstdlib>
#include "catalog/mssql_catalog.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "dml/mssql_identity_insert.hpp"
#include "duckdb/common/exception.hpp"
#include "query/mssql_simple_query.hpp"

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

//! The OFF batch is bounded even though the ordinary paths could carry the
//! session's query timeout: the destructor path cannot (issue #178), and one
//! bound for every path is one fewer thing to get wrong. 30 s is the response
//! timeout the batch executor already uses.
static constexpr int IDENTITY_INSERT_OFF_TIMEOUT_MS = 30000;

void MSSQLStatementConnection::EnableIdentityInsert(const string &schema_name, const string &table_name) {
	if (identity_insert_on_) {
		return;
	}
	if (!connection_) {
		throw InternalException("EnableIdentityInsert called before Acquire");
	}
	const string sql = mssql::IdentityInsertSql(schema_name, table_name, true);
	auto result = MSSQLSimpleQuery::Execute(*connection_, sql);
	if (!result.success) {
		const string explained =
			mssql::ExplainIdentityInsertError(result.error_number, result.error_message, schema_name, table_name);
		if (!explained.empty()) {
			throw InvalidInputException(explained);
		}
		throw IOException("MSSQL: SET IDENTITY_INSERT ON for %s.%s failed: [%u] %s", schema_name, table_name,
						  result.error_number, result.error_message);
	}
	identity_insert_on_ = true;
	identity_schema_ = schema_name;
	identity_table_ = table_name;
}

static int GetDmlDebugLevel() {
	// MSSQL_DML_DEBUG, else the general MSSQL_DEBUG every other DML executor
	// reads — as the CTAS executor does — so the warnings below are not hidden
	// from someone who set only the general switch.
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DML_DEBUG");
		if (!env) {
			env = std::getenv("MSSQL_DEBUG");
		}
		return env ? std::atoi(env) : 0;
	}();
	return level;
}

bool MSSQLStatementConnection::DisableIdentityInsert(int timeout_ms) noexcept {
	if (!identity_insert_on_) {
		return true;
	}
	// The flag stays set until the server CONFIRMS the OFF. It used to be
	// cleared on the attempt, so a failed OFF in Commit handed Fail a
	// connection it believed clean; measured with the OFF sabotaged, Fail
	// still closed it — a refused SET leaves the connection non-Idle and the
	// mid-response branch below catches that — so the guarantee held by a side
	// effect. It now holds by construction: Fail retries, closes if the retry
	// does not confirm, and the destructor path gets one more attempt on a
	// pinned connection that is Idle again by then.
	if (!connection_ || connection_->GetState() != tds::ConnectionState::Idle) {
		return false;
	}
	bool confirmed = false;
	try {
		auto result = MSSQLSimpleQuery::Execute(
			*connection_, mssql::IdentityInsertSql(identity_schema_, identity_table_, false), timeout_ms);
		confirmed = result.success;
	} catch (...) {
		confirmed = false;
	}
	if (confirmed) {
		identity_insert_on_ = false;
	}
	return confirmed;
}

void MSSQLStatementConnection::Commit(ClientContext &context, MSSQLCatalog &catalog) {
	if (!connection_) {
		return;
	}
	// OFF before COMMIT: it is session state, not transaction state, and the
	// connection must not go back to the pool — or on to the next statement of
	// a pinned transaction — still accepting identity values.
	if (identity_insert_on_ && !DisableIdentityInsert(IDENTITY_INSERT_OFF_TIMEOUT_MS)) {
		// Read before Fail: it does not change it, but the message must say
		// what Fail actually did, and on a pinned connection it neither rolls
		// back (the DuckDB transaction owns that) nor closes.
		const bool pinned = transaction_pinned_;
		Fail(context, catalog);
		if (pinned) {
			throw IOException(
				"MSSQL: could not turn IDENTITY_INSERT off for %s.%s after the INSERT. The "
				"transaction's connection cannot be discarded, so the setting may still be on "
				"until the transaction ends: ROLLBACK it.",
				identity_schema_, identity_table_);
		}
		throw IOException(
			"MSSQL: could not turn IDENTITY_INSERT off for %s.%s after the INSERT; the statement was "
			"rolled back and the connection discarded",
			identity_schema_, identity_table_);
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
	// A leaked ON would make the next statement on this connection accept
	// identity values in silence. Not confirmed off → the connection is closed
	// below rather than pooled; a PINNED one cannot be closed (it is the
	// DuckDB transaction's), so there the attempt is all there is, and the
	// transaction's end resets the session under the default
	// mssql_reset_connection — with it off, the rest of that transaction runs
	// with IDENTITY_INSERT on, which is why it is logged.
	const bool identity_cleared = DisableIdentityInsert(IDENTITY_INSERT_OFF_TIMEOUT_MS);
	if (!identity_cleared && transaction_pinned_ && GetDmlDebugLevel() >= 1) {
		fprintf(stderr,
				"[MSSQL DML] IDENTITY_INSERT for %s.%s could not be turned off on the transaction's pinned "
				"connection; it stays on until the transaction ends\n",
				identity_schema_.c_str(), identity_table_.c_str());
	}
	try {
		if (!transaction_pinned_ &&
			(!identity_cleared || (connection_->GetState() != tds::ConnectionState::Idle &&
								   connection_->GetState() != tds::ConnectionState::Disconnected))) {
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
	// Same discipline as Fail, with no ClientContext: OFF, bounded; not
	// confirmed → Close, so ReleaseBcpConnectionOnError discards it instead of
	// pooling a session that still accepts identity values.
	if (!DisableIdentityInsert(IDENTITY_INSERT_OFF_TIMEOUT_MS) && connection_) {
		if (!transaction_pinned_) {
			try {
				connection_->Close();
			} catch (...) {
			}
		} else if (GetDmlDebugLevel() >= 1) {
			fprintf(stderr,
					"[MSSQL DML] IDENTITY_INSERT for %s.%s could not be turned off on the transaction's pinned "
					"connection (statement unwound); it stays on until the transaction ends\n",
					identity_schema_.c_str(), identity_table_.c_str());
		}
	}
	// Closes a non-Idle connection, leaves a pinned one to its transaction, and
	// returns a pool connection with the reset flag captured at Acquire.
	mssql::ReleaseBcpConnectionOnError(connection_, pool_handle_, transaction_pinned_, reset_on_release_);
}

}  // namespace duckdb
