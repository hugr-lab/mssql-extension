#include "dml/mssql_session_stage.hpp"

#include "catalog/mssql_catalog.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "query/mssql_simple_query.hpp"

namespace duckdb {

MSSQLSessionStage::MSSQLSessionStage(MSSQLCatalog &catalog, string label)
	: catalog_(catalog), label_(std::move(label)) {}

void MSSQLSessionStage::Fail(ClientContext &context) noexcept {
	session_.Abandon();
	stmt_conn_.Fail(context, catalog_);
	connection_.reset();
}

void MSSQLSessionStage::FailAndThrow(ClientContext &context, const string &message) {
	// One statement over the whole stage: the server ran it whole or not at
	// all. In autocommit its transaction is rolled back -- by DuckDB, which
	// rolls back the statement's own (issue #451); inside an explicit DuckDB
	// transaction the stage and the statement sit in the open one.
	const bool pinned = stmt_conn_.InOpenTransaction();
	Fail(context);
	throw IOException("%s %s", message,
					  pinned ? "(the open transaction must be rolled back)"
							 : "(nothing was written: the statement's server transaction was rolled back)");
}

void MSSQLSessionStage::Open(ClientContext &context, const char *operation, const string &create_sql,
							 const string &stage_name, vector<mssql::BCPColumnMetadata> columns, idx_t flush_rows,
							 int timeout_ms) {
	connection_ = stmt_conn_.Acquire(context, catalog_);
	{
		auto pinned_lock = stmt_conn_.LockPinned(context, catalog_);
		MSSQLStatementConnection::RequireIdle(*connection_, operation);
		auto result = MSSQLSimpleQuery::Execute(*connection_, create_sql, timeout_ms);
		if (!result.success) {
			FailAndThrow(context, label_ + ": creating the stage failed: " + result.error_message);
		}
	}

	bcp_columns_ = std::move(columns);
	bcp_target_ = mssql::BCPCopyTarget(catalog_.GetName().GetIdentifierName(), string(), stage_name);
	// KEEP_NULLS: a NULL is a value of the stage, not a request for a default.
	insert_bulk_sql_ = mssql::BuildInsertBulkSql(bcp_target_, bcp_columns_, false, flush_rows,
												 mssql::InsertBulkHints::StatementSemantics());

	session_params_.pool = &catalog_.GetConnectionPool();
	session_params_.pool_handle = catalog_.GetConnectionPoolHandle();
	session_params_.insert_bulk_sql = &insert_bulk_sql_;
	session_params_.target = &bcp_target_;
	session_params_.columns = &bcp_columns_;
	session_params_.column_mapping = nullptr;  // positional: the fill chunk is in stage order
	session_params_.flush_rows = flush_rows;
	session_params_.reset_on_release = ConnectionProvider::ShouldResetOnRelease(context);
	// The statement's server transaction already brackets the fill.
	session_params_.own_transaction = false;
	// The connection is the statement's (MSSQLStatementConnection), not the
	// session's: adopted as "pinned", the session never releases it -- Finish /
	// the error paths drop the reference, closing it on an error -- and the
	// statement's Commit / Fail decide what happens to it.
	session_.Adopt(connection_, session_params_, true);
}

void MSSQLSessionStage::Write(ClientContext &context, DataChunk &fill) {
	auto pinned_lock = stmt_conn_.LockPinned(context, catalog_);
	try {
		session_.Write(fill);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		FailAndThrow(context, StringUtil::Format("%s: filling the stage failed after %llu rows: %s", label_,
												 (unsigned long long)rows_staged_, error.RawMessage()));
	}
	rows_staged_ += fill.size();
}

void MSSQLSessionStage::CloseFill(ClientContext &context) {
	try {
		session_.CloseStream();
	} catch (std::exception &ex) {
		ErrorData error(ex);
		FailAndThrow(context, label_ + ": filling the stage failed: " + error.RawMessage());
	}
	session_.Release();
}

MSSQLStatementConnection::PinnedLock MSSQLSessionStage::LockPinned(ClientContext &context) const {
	return stmt_conn_.LockPinned(context, catalog_);
}

void MSSQLSessionStage::Commit(ClientContext &context) {
	connection_.reset();
	stmt_conn_.Commit(context, catalog_);
}

}  // namespace duckdb
