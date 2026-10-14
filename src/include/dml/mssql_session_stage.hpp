//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// dml/mssql_session_stage.hpp
//
// Spec 080: a session `#temp` table filled by INSERT BULK on the statement's
// own connection, then read by one statement on that connection -- D3's
// `#stage` (a staged UPDATE / DELETE, MSSQLStagedDml) and D4's `#src` (a
// staged MERGE, MSSQLStagedMerge).
//
// The table is created inside the statement's server transaction -- the
// LoadTransaction in autocommit, the DuckDB transaction's when pinned -- and a
// `#temp` created in a transaction is dropped by its ROLLBACK. So a failure
// needs no cleanup statement of its own: FailAndThrow rolls back (autocommit)
// or the transaction's end does (pinned), and a connection left mid-response
// is closed, which ends the session. On success the owner drops the table
// before Commit. A fill that fails mid-stream leaves the connection
// mid-response, and it is closed even when it is the transaction's pinned one
// -- as a failed COPY or INSERT BULK in a transaction does: DuckDB aborts the
// transaction anyway.
//===----------------------------------------------------------------------===//

#pragma once

#include "copy/bulk_load_session.hpp"
#include "copy/target_resolver.hpp"
#include "dml/mssql_statement_connection.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace duckdb {

class ClientContext;
class MSSQLCatalog;

class MSSQLSessionStage {
public:
	//! `label` opens every error message: "MSSQL UPDATE on 'dbo.t'".
	MSSQLSessionStage(MSSQLCatalog &catalog, string label);

	//! Take the statement's connection, run `create_sql` on it (which creates
	//! `stage_name`), and hand the connection to the fill. `columns` are the
	//! stage's, in the order Write's chunks lay them out. `operation` names the
	//! statement in RequireIdle's refusal; `timeout_ms` bounds `create_sql`
	//! (0 = none). Sends no INSERT BULK yet: the stream opens on the first
	//! Write, so a stage that gets no row costs only its CREATE.
	void Open(ClientContext &context, const char *operation, const string &create_sql, const string &stage_name,
			  vector<mssql::BCPColumnMetadata> columns, idx_t flush_rows, int timeout_ms);

	bool IsOpen() const {
		return connection_ != nullptr;
	}

	//! Add one chunk, its columns in stage order.
	void Write(ClientContext &context, DataChunk &fill);

	//! End the fill: DONE for what is unflushed, the connection back to the
	//! owner's statements. Call with LockPinned held.
	void CloseFill(ClientContext &context);

	//! The pinned connection's lock (MSSQLStatementConnection::LockPinned).
	MSSQLStatementConnection::PinnedLock LockPinned(ClientContext &context) const;

	//! The statement's connection, for the statements that read the stage.
	//! Valid between Open and Commit / FailAndThrow.
	tds::TdsConnection &GetConnection() {
		return *connection_;
	}
	const std::shared_ptr<tds::TdsConnection> &GetConnectionPtr() const {
		return connection_;
	}

	idx_t RowsStaged() const {
		return rows_staged_;
	}

	//! Inside an explicit DuckDB transaction (see MSSQLStatementConnection).
	bool InOpenTransaction() const {
		return stmt_conn_.InOpenTransaction();
	}

	//! The statement failed: abandon the fill and fail the statement's
	//! connection (ROLLBACK in autocommit). Never throws.
	void Fail(ClientContext &context) noexcept;

	//! Fail, then throw `message` with what became of the write.
	[[noreturn]] void FailAndThrow(ClientContext &context, const string &message);

	//! After the owner's last statement (the stage dropped): let go of the
	//! connection and commit, in autocommit.
	void Commit(ClientContext &context);

private:
	MSSQLCatalog &catalog_;
	string label_;
	MSSQLStatementConnection stmt_conn_;
	std::shared_ptr<tds::TdsConnection> connection_;

	mssql::BCPCopyTarget bcp_target_;
	vector<mssql::BCPColumnMetadata> bcp_columns_;
	string insert_bulk_sql_;
	mssql::BulkLoadSessionParams session_params_;
	mssql::BulkLoadSession session_;
	idx_t rows_staged_ = 0;
};

}  // namespace duckdb
