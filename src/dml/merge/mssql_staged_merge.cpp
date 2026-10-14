#include "dml/merge/mssql_staged_merge.hpp"

#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_statistics.hpp"
#include "dml/mssql_dml_execute.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "mssql_counters.hpp"
#include "query/mssql_identifier.hpp"
#include "query/mssql_simple_query.hpp"

namespace duckdb {

//! mssql_query_timeout in ms for MSSQLSimpleQuery (0 = none), clamped to int.
static int QueryTimeoutMs(const MSSQLStagedMergeTarget &target) {
	const int64_t ms = static_cast<int64_t>(target.query_timeout_seconds) * 1000;
	return ms > NumericLimits<int32_t>::Maximum() ? NumericLimits<int32_t>::Maximum() : static_cast<int>(ms);
}

MSSQLStagedMerge::MSSQLStagedMerge(ClientContext &context, const MSSQLStagedMergeTarget &target)
	: target_(target),
	  catalog_(Catalog::GetCatalog(context, Identifier(target.catalog_name)).Cast<MSSQLCatalog>()),
	  stage_(catalog_, StringUtil::Format("MSSQL MERGE INTO '%s.%s'", target.schema_name, target.table_name)) {}

void MSSQLStagedMerge::Start(ClientContext &context) {
	stage_.Open(context, "MERGE", target_.create_sql, target_.src_name, target_.src_columns, target_.flush_rows,
				QueryTimeoutMs(target_));
}

void MSSQLStagedMerge::Execute(ClientContext &context, DataChunk &fill) {
	if (finalized_) {
		throw InternalException("MSSQLStagedMerge::Execute called after Finalize");
	}
	if (fill.size() == 0 || target_.no_action) {
		return;
	}
	if (!stage_.IsOpen()) {
		Start(context);
	}
	stage_.Write(context, fill);
}

idx_t MSSQLStagedMerge::Finalize(ClientContext &context) {
	if (finalized_) {
		return 0;
	}
	finalized_ = true;
	if (!stage_.IsOpen()) {
		if (!target_.runs_on_empty_source) {
			// No source row and no WHEN NOT MATCHED BY SOURCE: no clause can
			// fire, so nothing is sent and no connection is taken.
			return 0;
		}
		// Every target row is unmatched by source: the MERGE runs against an
		// empty #src.
		Start(context);
	}
	idx_t rows = 0;
	{
		auto pinned_lock = stage_.LockPinned(context);
		stage_.CloseFill(context);
		// No deadline, as for a pushed DML: the MERGE answers nothing until its
		// DONE and takes as long as it takes; Ctrl-C cancels it (ATTENTION, the
		// server rolls it back), a dead peer ends the wait through TCP keepalive.
		mssql::DmlBatchOptions options;
		options.label = "staged MERGE";
		options.no_deadline = true;
		const auto response = mssql::ExecuteDmlBatch(context, stage_.GetConnection(), target_.merge_request, options);
		if (response.status == mssql::DmlBatchResponse::Status::INTERRUPTED) {
			stage_.Fail(context);
			throw InterruptException();
		}
		if (!response.Succeeded()) {
			// A desync is the client's: the server ran the statement, and the
			// failure is in reading its answer (#323).
			const char *executed = response.status == mssql::DmlBatchResponse::Status::PARSE_ERROR
									   ? "; the server executed the statement"
									   : "";
			stage_.FailAndThrow(context,
								StringUtil::Format("MSSQL MERGE INTO '%s.%s' failed: %s%s", target_.schema_name,
												   target_.table_name, response.DescribeError(), executed));
		}
		rows = response.rows_affected;
		auto dropped = MSSQLSimpleQuery::Execute(
			stage_.GetConnection(), "DROP TABLE " + mssql::QuoteIdentifier(target_.src_name), QueryTimeoutMs(target_));
		if (!dropped.success) {
			stage_.FailAndThrow(context,
								StringUtil::Format("MSSQL MERGE INTO '%s.%s': dropping the stage failed: %s",
												   target_.schema_name, target_.table_name, dropped.error_message));
		}
	}
	stage_.Commit(context);
	mssql::CountDml(mssql::DmlPath::STAGED);
	if (context.transaction.IsAutoCommit() && rows > 0) {
		// A MERGE's count mixes its inserted, updated and deleted rows, so the
		// cached row count cannot be adjusted by it: it is dropped, and the
		// planner's next lookup reads a fresh one (as for a pushed MERGE).
		catalog_.GetStatisticsProvider().InvalidateTable(target_.schema_name, target_.table_name);
	}
	return rows;
}

}  // namespace duckdb
