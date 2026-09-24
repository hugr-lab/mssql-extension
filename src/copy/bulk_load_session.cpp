#include "copy/bulk_load_session.hpp"
#include <cstdio>
#include <cstdlib>
#include "query/mssql_identifier.hpp"

#include <chrono>

#include "connection/mssql_connection_provider.hpp"
#include "copy/bcp_config.hpp"
#include "duckdb/common/exception.hpp"
#include "query/mssql_simple_query.hpp"

// House debug pattern: a static level read from MSSQL_DEBUG.
static int GetBulkLoadDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
	return level;
}

#define BULK_LOAD_LOG(fmt, ...)                                       \
	do {                                                              \
		if (GetBulkLoadDebugLevel() >= 1) {                           \
			fprintf(stderr, "[MSSQL BULK] " fmt "\n", ##__VA_ARGS__); \
		}                                                             \
	} while (0)

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
						  idx_t rows_per_batch, const InsertBulkHints &hints) {
	// A temp table is named by its bare name: `#t` lives in tempdb, and
	// `[dbo].[#t]` sends the server looking in the current database.
	string sql = "INSERT BULK ";
	sql += target.IsTempTable() ? target.GetBracketedTable() : target.GetFullyQualifiedName();
	sql += " (";
	for (idx_t i = 0; i < columns.size(); i++) {
		if (i > 0) {
			sql += ", ";
		}
		sql += mssql::QuoteIdentifier(columns[i].name) + " ";
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
	vector<string> with;
	if (tablock) {
		with.push_back("TABLOCK");
	}
	if (rows_per_batch > 0) {
		with.push_back("ROWS_PER_BATCH = " + std::to_string(rows_per_batch));
	}
	// The statement-semantics hints (see InsertBulkHints): an INSERT that
	// loads through INSERT BULK still checks constraints, fires triggers and
	// keeps its NULLs.
	if (hints.check_constraints) {
		with.push_back("CHECK_CONSTRAINTS");
	}
	if (hints.fire_triggers) {
		with.push_back("FIRE_TRIGGERS");
	}
	if (hints.keep_nulls) {
		with.push_back("KEEP_NULLS");
	}
	if (!with.empty()) {
		sql += " WITH (";
		for (idx_t i = 0; i < with.size(); i++) {
			if (i > 0) {
				sql += ", ";
			}
			sql += with[i];
		}
		sql += ")";
	}
	return sql;
}

BulkLoadSession::~BulkLoadSession() noexcept {
	// The writer goes first: it holds a reference to the connection, and the
	// release protocol closes the socket underneath it. An own transaction
	// still open here is rolled back: by the ROLLBACK if the connection is
	// Idle, by the close otherwise.
	SnapshotWriterCounters();
	writer_.reset();
	transaction_.Rollback();
	ReleaseBcpConnectionOnError(connection_, pool_handle_, transaction_pinned_, reset_on_release_);
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
		// Never wait (review of #382): an extra writer is optional, so it takes
		// an idle connection, or a new one while the pool is below its limit,
		// or none -- and the thread shares the global writer meanwhile. Waiting
		// out mssql_acquire_timeout for a connection the statement itself holds
		// (the pinned one, or the source scan's) stalled a CTAS 30 s.
		std::string why;
		bool creation_failed = false;
		conn = params.pool->TryAcquire(&why, &creation_failed);
		if (!conn) {
			slots_used.fetch_sub(1);
			if (creation_failed) {
				// A login the server refuses does not clear on the next chunk:
				// stop asking, as before TryAcquire (review of #382) -- else the
				// thread re-dials on every backoff window for the whole load.
				BULK_LOAD_LOG("extra writer not started, the pool could not create a connection: %s", why.c_str());
				return Claim::Unavailable;
			}
			// Nothing free right now: let the caller ask again on a later chunk.
			return Claim::Busy;
		}
		if (conn->GetState() != tds::ConnectionState::Idle) {
			throw IOException("no idle connection available for a parallel writer");
		}
		Adopt(conn, params, /*transaction_pinned=*/false);
		// A parallel writer opens its stream at once: it exists to carry rows and
		// has a chunk in hand, and a failure to open is what makes the claim fall
		// back rather than surface later as a write error.
		OpenStream();
		return Claim::Started;
	} catch (std::exception &) {
		// Falling back is the whole contract: put the connection back and let the
		// thread share the global writer. This is TERMINAL for the load — the
		// server refused a bulk load, or handed a connection that was not Idle,
		// and neither clears on a later chunk — so the caller stops asking. A pool
		// with nothing free is not this case: that returns Busy above.
		//
		// Adopted already (the throw came from OpenStream): the session owns the
		// connection, and an own transaction may be open on it -- Abandon rolls
		// that back and clears the descriptor before the connection goes back,
		// so the pool never holds a connection mid-transaction that a later
		// destructor would then send ROLLBACK to (spec 062 self-review). Not yet
		// adopted (Acquire failed or returned a non-Idle connection): the local
		// handle is all there is.
		if (connection_) {
			Abandon();
		} else if (conn) {
			try {
				params.pool->Release(conn);
			} catch (...) {
				// Nothing left to do — the handle is dropped either way.
			}
		}
		writer_.reset();
		stream_open_ = false;
		connection_.reset();
		slots_used.fetch_sub(1);
		return Claim::Unavailable;
	}
}

void BulkLoadSession::Adopt(std::shared_ptr<tds::TdsConnection> connection, const BulkLoadSessionParams &params,
							bool transaction_pinned) {
	pool_handle_ = params.pool_handle;
	insert_bulk_sql_ = *params.insert_bulk_sql;
	flush_rows_ = params.flush_rows;
	collect_timings_ = params.collect_timings;
	reset_on_release_ = params.reset_on_release;
	transaction_pinned_ = transaction_pinned;
	connection_ = std::move(connection);
	// Before the writer and before any INSERT BULK: the transaction has to be
	// open on the connection before the first request it should cover. A
	// pinned connection is already inside the DuckDB transaction; Begin is a
	// no-op for it.
	if (params.own_transaction) {
		transaction_.Begin(connection_, transaction_pinned_);
	}
	writer_ = make_uniq<BCPWriter>(*connection_, *params.target, *params.columns,
								   params.column_mapping ? *params.column_mapping : vector<int32_t>());
	stream_open_ = false;
	rows_in_batch_ = 0;
	batches_flushed_ = 0;
}

void BulkLoadSession::OpenStream() {
	auto result = MSSQLSimpleQuery::Execute(*connection_, insert_bulk_sql_);
	if (!result.success) {
		throw IOException("INSERT BULK failed: %s", result.error_message);
	}
	if (!connection_->TransitionState(tds::ConnectionState::Idle, tds::ConnectionState::Executing,
									  "INSERT BULK stream")) {
		throw IOException("could not transition the bulk-load connection to Executing");
	}
	// The stream opens with COLMETADATA; without it the server has no schema
	// for the ROW tokens that follow.
	writer_->WriteColmetadata();
	stream_open_ = true;
}

void BulkLoadSession::ReopenBatch() {
	writer_->ResetForNextBatch();
	OpenStream();
}

BulkLoadWriteResult BulkLoadSession::Write(DataChunk &chunk) {
	BulkLoadWriteResult out;
	if (!writer_) {
		// Abandoned (a failure on another thread) or never started: there is no
		// connection to open a stream on. The caller's error protocol should
		// have stopped it before this; this is what stops it anyway.
		throw IOException("bulk-load session is closed");
	}
	if (!stream_open_) {
		OpenStream();
	}
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

void BulkLoadSession::SnapshotWriterCounters() {
	if (!writer_) {
		return;
	}
	counter_build_send_ns_ = writer_->GetBuildSendNs();
	counter_server_wait_ns_ = writer_->GetServerWaitNs();
	counter_send_calls_ = writer_->GetSendCalls();
}

void BulkLoadSession::ReleaseConnection() {
	if (!connection_) {
		return;
	}
	connection_->TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
	if (transaction_pinned_) {
		// The MSSQLTransaction owns the pin; just drop our reference.
		connection_.reset();
		return;
	}
	if (auto pool = pool_handle_.lock()) {
		connection_->SetNeedsReset(reset_on_release_);
		pool->Release(connection_);
	}
	connection_.reset();
}

idx_t BulkLoadSession::CloseStream() {
	if (!writer_) {
		return 0;
	}
	idx_t confirmed = 0;
	if (stream_open_) {
		try {
			// Always send DONE, even for zero rows: INSERT BULK left the
			// connection in Executing, and only DONE closes the stream so it can
			// be pooled again.
			writer_->WriteDone(rows_in_batch_);
			confirmed = writer_->Finalize();
		} catch (...) {
			Abandon();
			throw;
		}
		if (rows_in_batch_ > 0) {
			++batches_flushed_;
		}
		stream_open_ = false;
	}
	rows_in_batch_ = 0;
	SnapshotWriterCounters();
	writer_.reset();
	return confirmed;
}

void BulkLoadSession::Commit() {
	if (!transaction_.IsOpen()) {
		return;
	}
	try {
		transaction_.Commit();
	} catch (...) {
		Abandon();
		throw;
	}
}

void BulkLoadSession::Release() {
	ReleaseConnection();
}

void BulkLoadSession::AdoptDeferred() {
	if (!deferred_params_ || writer_) {
		return;
	}
	auto pool = deferred_params_->pool_handle.lock();
	if (!pool) {
		throw IOException("bulk load: the catalog's connection pool is gone");
	}
	std::string why;
	auto connection = pool->Acquire(-1, &why);
	if (!connection) {
		throw IOException("bulk load: could not acquire a connection: %s", why);
	}
	Adopt(std::move(connection), *deferred_params_, /*transaction_pinned=*/false);
}

idx_t BulkLoadSession::Finish() {
	const idx_t confirmed = CloseStream();
	Commit();
	Release();
	return confirmed;
}

void BulkLoadSession::Abandon() noexcept {
	// Order matters and is the point of this function existing: the connection is
	// sitting mid-bulk-load on the very table a cleanup DROP has to take a schema
	// lock on. Leaving it open until the destructor runs would have that DROP
	// block on this same thread's work.
	SnapshotWriterCounters();
	writer_.reset();
	transaction_.Rollback();
	ReleaseBcpConnectionOnError(connection_, pool_handle_, transaction_pinned_, reset_on_release_);
	stream_open_ = false;
	rows_in_batch_ = 0;
}

}  // namespace mssql
}  // namespace duckdb
