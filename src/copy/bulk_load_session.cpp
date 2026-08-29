#include "copy/bulk_load_session.hpp"

#include <chrono>

#include "connection/mssql_connection_provider.hpp"
#include "copy/bcp_config.hpp"
#include "duckdb/common/exception.hpp"
#include "query/mssql_simple_query.hpp"

namespace duckdb {
namespace mssql {

namespace {

using Clock = std::chrono::high_resolution_clock;
using TimePoint = std::chrono::time_point<Clock>;

uint64_t ElapsedNs(TimePoint start) {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

}  // namespace

string BuildInsertBulkSql(const BCPCopyTarget &target, const vector<BCPColumnMetadata> &columns, bool tablock,
						  idx_t rows_per_batch) {
	// A temp table is named by its bare name: `#t` lives in tempdb, and
	// `[dbo].[#t]` sends the server looking in the current database.
	string sql = "INSERT BULK ";
	sql += target.IsTempTable() ? target.GetBracketedTable() : target.GetFullyQualifiedName();
	sql += " (";
	for (idx_t i = 0; i < columns.size(); i++) {
		if (i > 0) {
			sql += ", ";
		}
		sql += "[" + columns[i].name + "] ";
		// The column's own declaration: exact TDS type info for an existing
		// table, generated types for one this statement is creating.
		sql += columns[i].GetSQLServerTypeDeclaration();
	}
	sql += ")";

	// TABLOCK: a table-level lock instead of row locks, which also enables
	// minimal logging. ROWS_PER_BATCH: tells the server the batch size up front
	// so it can size the load rather than discover it.
	//
	// An MSSQL_BCP_EXTRA_HINTS env var used to splice arbitrary text into this
	// WITH clause, to measure hints the extension does not send yet. It is gone:
	// an environment variable reaching a T-SQL statement verbatim is an injection
	// surface for anything that can set one. The hint it existed to investigate —
	// ORDER for a clustered target — needs a real option and a guarantee that the
	// rows really are sorted, which is its own spec.
	if (tablock && rows_per_batch > 0) {
		sql += " WITH (TABLOCK, ROWS_PER_BATCH = " + std::to_string(rows_per_batch) + ")";
	} else if (tablock) {
		sql += " WITH (TABLOCK)";
	} else if (rows_per_batch > 0) {
		sql += " WITH (ROWS_PER_BATCH = " + std::to_string(rows_per_batch) + ")";
	}
	return sql;
}

BulkLoadSession::~BulkLoadSession() noexcept {
	// The writer goes first: it holds a reference to the connection, and the
	// release protocol closes the socket underneath it.
	writer_.reset();
	ReleaseBcpConnectionOnError(connection_, pool_handle_, /*transaction_pinned=*/false, reset_on_release_);
}

BulkLoadSession::Claim BulkLoadSession::TryStart(const BulkLoadSessionParams &params, std::atomic<idx_t> &slots_used,
												 idx_t max_writers, const std::atomic<idx_t> &rows_sunk) {
	if (max_writers <= 1) {
		return Claim::Unavailable;
	}
	// Warm-up gate (spec 070 W2): hold every extra writer until the load has
	// produced ONE COMPRESSIBLE batch on the shared writer, then let the full
	// writer count open. That first batch is what keeps a SMALL columnstore load
	// compressible — it lands a full rowgroup before any second writer dilutes it
	// — while a load big enough to want parallelism pays only that one batch
	// (~102k rows, ~0.1-0.2 s) before it fans out. A gate of active*flush_rows
	// was tried first and measured 1.3-1.5x slower at threads=4: it kept
	// ~active*flush_rows rows serialized on the shared writer, far past the point
	// of diminishing compression return.
	//
	// The threshold is the SERVER's (MSSQL_COLUMNSTORE_ROWGROUP_ROWS), not the
	// user's `flush_rows` (PR #270 review). `mssql_copy_flush_rows` is settable
	// to anything non-negative and only DEFAULTS to the same number, so keying
	// the gate on it re-entered the rejected shape through the setting: at
	// `flush_rows = 1000000` the first million rows serialized onto one writer.
	// Below the server threshold no batch can compress at all, so there is
	// nothing for the gate to protect and it stays open. `flush_rows = 0` is NOT
	// that case — it means one UNBOUNDED batch per writer, which can compress, so
	// the gate applies there (job 1116). The rule is MSSQLWarmupGateRows, which is
	// pure and unit-tested at its boundaries.
	//
	// The read is racy by design — a ramp heuristic, not a correctness bound; the
	// slot cap below still holds.
	const idx_t warmup_rows =
		MSSQLWarmupGateRows(params.warmup_gate, params.flush_rows, MSSQL_COLUMNSTORE_ROWGROUP_ROWS);
	if (warmup_rows > 0 && rows_sunk.load(std::memory_order_relaxed) < warmup_rows) {
		// Transient — the gate opens once the shared writer crosses the rowgroup
		// threshold. The caller must ask again on a later chunk.
		return Claim::GateClosed;
	}
	// Claim a slot before doing any work, so N threads racing here cannot
	// collectively exceed the limit.
	const idx_t slot = slots_used.fetch_add(1);
	if (slot >= max_writers) {
		slots_used.fetch_sub(1);
		return Claim::Unavailable;
	}

	std::shared_ptr<tds::TdsConnection> conn;
	try {
		conn = params.pool->Acquire();
		if (!conn || conn->GetState() != tds::ConnectionState::Idle) {
			throw IOException("no idle connection available for a parallel writer");
		}
		auto result = MSSQLSimpleQuery::Execute(*conn, *params.insert_bulk_sql);
		if (!result.success) {
			throw IOException("INSERT BULK failed on the parallel connection: %s", result.error_message);
		}
		if (!conn->TransitionState(tds::ConnectionState::Idle, tds::ConnectionState::Executing)) {
			throw IOException("could not transition the parallel connection to Executing");
		}

		pool_handle_ = params.pool_handle;
		insert_bulk_sql_ = params.insert_bulk_sql;
		flush_rows_ = params.flush_rows;
		collect_timings_ = params.collect_timings;
		reset_on_release_ = params.reset_on_release;
		connection_ = conn;
		writer_ = make_uniq<BCPWriter>(*connection_, *params.target, *params.columns,
									   params.column_mapping ? *params.column_mapping : vector<int32_t>());
		// The stream opens with COLMETADATA; without it the server has no schema
		// for the ROW tokens that follow.
		writer_->WriteColmetadata();
		return Claim::Started;
	} catch (std::exception &) {
		// Falling back is the whole contract: put the connection back and let the
		// thread share the global writer. This is TERMINAL for the load — the pool
		// is exhausted or the server refused a bulk load, and neither clears on a
		// later chunk — so the caller stops asking rather than re-blocking a 30 s
		// Acquire() every chunk (spec 070 W2 review, finding 1).
		if (conn) {
			try {
				params.pool->Release(conn);
			} catch (...) {
				// Nothing left to do — the handle is dropped either way.
			}
		}
		connection_.reset();
		writer_.reset();
		slots_used.fetch_sub(1);
		return Claim::Unavailable;
	}
}

void BulkLoadSession::ReopenBatch() {
	auto result = MSSQLSimpleQuery::Execute(*connection_, *insert_bulk_sql_);
	if (!result.success) {
		throw IOException("failed to re-execute INSERT BULK on a parallel connection: %s", result.error_message);
	}
	if (!connection_->TransitionState(tds::ConnectionState::Idle, tds::ConnectionState::Executing)) {
		throw IOException("failed to transition a parallel connection to Executing");
	}
	writer_->ResetForNextBatch();
	writer_->WriteColmetadata();
}

BulkLoadWriteResult BulkLoadSession::Write(DataChunk &chunk) {
	BulkLoadWriteResult out;

	auto encode_start = collect_timings_ ? Clock::now() : TimePoint{};
	out.rows_written = writer_->WriteRows(chunk);
	out.encode_ns = collect_timings_ ? ElapsedNs(encode_start) : 0;
	rows_in_batch_ += out.rows_written;

	if (flush_rows_ > 0 && rows_in_batch_ >= flush_rows_) {
		auto flush_start = collect_timings_ ? Clock::now() : TimePoint{};
		out.rows_confirmed = writer_->FlushBatch(rows_in_batch_);
		out.flushed = true;
		rows_in_batch_ = 0;
		++batches_flushed_;
		// FlushBatch closed the stream; the next batch needs its own INSERT BULK.
		ReopenBatch();
		out.flush_ns = collect_timings_ ? ElapsedNs(flush_start) : 0;
	}
	return out;
}

idx_t BulkLoadSession::Finish() {
	if (!writer_) {
		return 0;
	}
	// Always send DONE, even for zero rows: INSERT BULK left the connection in
	// Executing, and only DONE closes the stream so it can be pooled again.
	writer_->WriteDone(rows_in_batch_);
	const idx_t confirmed = writer_->Finalize();
	if (rows_in_batch_ > 0) {
		++batches_flushed_;
	}
	rows_in_batch_ = 0;
	writer_.reset();

	if (connection_) {
		connection_->TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
		if (auto pool = pool_handle_.lock()) {
			pool->Release(connection_);
		}
		connection_.reset();
	}
	return confirmed;
}

void BulkLoadSession::Abandon() noexcept {
	// Order matters and is the point of this function existing: the connection is
	// sitting mid-bulk-load on the very table a cleanup DROP has to take a schema
	// lock on. Leaving it open until the destructor runs would have that DROP
	// block on this same thread's work.
	writer_.reset();
	ReleaseBcpConnectionOnError(connection_, pool_handle_, /*transaction_pinned=*/false, reset_on_release_);
	rows_in_batch_ = 0;
}

}  // namespace mssql
}  // namespace duckdb
