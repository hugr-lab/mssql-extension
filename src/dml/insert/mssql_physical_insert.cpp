#include "dml/insert/mssql_physical_insert.hpp"

#include <cstdio>
#include <cstdlib>

#include "catalog/mssql_catalog.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "copy/load_policy.hpp"
#include "copy/target_resolver.hpp"
#include "dml/insert/mssql_insert_executor.hpp"
#include "dml/mssql_dml_outcome.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/database.hpp"

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetInsertSinkDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
	return level;
}

#define INSERT_SINK_LOG(lvl, fmt, ...)                                      \
	do {                                                                    \
		if (GetInsertSinkDebugLevel() >= lvl) {                             \
			fprintf(stderr, "[MSSQL INSERT BCP] " fmt "\n", ##__VA_ARGS__); \
		}                                                                   \
	} while (0)

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLInsertGlobalSinkState
//===----------------------------------------------------------------------===//

MSSQLInsertGlobalSinkState::MSSQLInsertGlobalSinkState(ClientContext &context, const MSSQLInsertTarget &target,
													   const MSSQLInsertConfig &config, bool return_chunk_flag,
													   const MSSQLInsertBulkPlan &bulk)
	: total_rows_inserted(0), returned(false), return_chunk(return_chunk_flag), result_chunk_index(0) {
	executor = make_uniq<MSSQLInsertExecutor>(context, target, config);

	// Build returning column indices from target
	if (return_chunk) {
		returning_column_ids = target.returning_column_indices;
	}

	// Bulk path: stage the first rows. The chunk that reaches the sink is
	// full-width in table order (the 2.0 binder resolves defaults into the
	// projection), so the staging buffer takes every table column's type.
	if (bulk.enabled) {
		vector<LogicalType> types;
		types.reserve(target.columns.size());
		for (const auto &col : target.columns) {
			types.push_back(col.duckdb_type);
		}
		staged = make_uniq<MSSQLStagedRows>(context, types);
	}
}

//===----------------------------------------------------------------------===//
// MSSQLPhysicalInsert Constructor
//===----------------------------------------------------------------------===//

MSSQLPhysicalInsert::MSSQLPhysicalInsert(PhysicalPlan &plan, vector<LogicalType> types, idx_t estimated_cardinality,
										 MSSQLInsertTarget target, MSSQLInsertConfig config, bool return_chunk,
										 MSSQLInsertBulkPlan bulk)
	: PhysicalOperator(plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
	  target_(std::move(target)),
	  config_(std::move(config)),
	  return_chunk_(return_chunk),
	  bulk_(std::move(bulk)) {}

//===----------------------------------------------------------------------===//
// State Management
//===----------------------------------------------------------------------===//

unique_ptr<GlobalSinkState> MSSQLPhysicalInsert::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<MSSQLInsertGlobalSinkState>(context, target_, config_, return_chunk_, bulk_);
}

unique_ptr<LocalSinkState> MSSQLPhysicalInsert::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<MSSQLInsertLocalSinkState>();
}

//===----------------------------------------------------------------------===//
// Bulk path helpers (spec 062 W2)
//===----------------------------------------------------------------------===//

namespace {

// First failure wins: with N writers, one broken load fails them all, and the
// first message is the one that explains it.
void RecordBulkError(MSSQLInsertGlobalSinkState &gstate, const string &message) {
	{
		std::lock_guard<std::mutex> lock(gstate.error_mutex);
		if (gstate.error_message.empty()) {
			gstate.error_message = message;
		}
	}
	gstate.has_error.store(true, std::memory_order_release);
}

// What the text path names, said for a bulk load (spec 062 W5): which batch of
// which writer, the server's own text, and what happened to the rows before
// it -- rolled back with the load's transaction in autocommit, or in the open
// DuckDB transaction inside one.
string BulkFailureMessage(const MSSQLInsertGlobalSinkState &gstate, idx_t batch_no, idx_t rows_in_batch,
						  const string &what) {
	return StringUtil::Format("INSERT via BCP failed at batch %llu of a writer (%llu row(s) in it): %s; %s",
							  (unsigned long long)batch_no, (unsigned long long)rows_in_batch, what,
							  MSSQLRowsBeforeOutcome(gstate.transaction_pinned, gstate.rows_confirmed.load()));
}

// The batch a session is on, read BEFORE the call that may fail: CloseStream
// and Commit abandon the session on failure, and Abandon zeroes the counters.
string BulkFailureMessage(const MSSQLInsertGlobalSinkState &gstate, const mssql::BulkLoadSession &session,
						  const string &what) {
	return BulkFailureMessage(gstate, session.BatchesFlushed() + 1, session.RowsInBatch(), what);
}

void AccountWrite(MSSQLInsertGlobalSinkState &gstate, const mssql::BulkLoadWriteResult &written) {
	gstate.rows_sent.fetch_add(written.rows_written, std::memory_order_relaxed);
	if (written.flushed) {
		gstate.rows_confirmed.fetch_add(written.rows_confirmed, std::memory_order_relaxed);
		gstate.batches_flushed.fetch_add(1, std::memory_order_relaxed);
	}
}

mssql::BulkLoadSessionParams SessionParams(const MSSQLInsertGlobalSinkState &gstate, const MSSQLInsertBulkPlan &bulk) {
	mssql::BulkLoadSessionParams params;
	params.pool = gstate.pool;
	params.pool_handle = gstate.pool_handle;
	params.insert_bulk_sql = &bulk.insert_bulk_sql;
	params.target = &bulk.target;
	params.columns = &bulk.columns;
	params.column_mapping = &bulk.column_mapping;
	params.flush_rows = bulk.flush_rows;
	params.collect_timings = false;
	params.reset_on_release = gstate.reset_on_release;
	// An INSERT is atomic: every writer's load sits in a server transaction of
	// its own until Finalize commits them all (a no-op on the pinned
	// connection, which the DuckDB transaction owns).
	params.own_transaction = true;
	// Spec 070 W2: hold extra writers until the shared one has landed a full
	// rowgroup on a clustered columnstore target; a heap fans out at once.
	params.warmup_gate = bulk.shape == MSSQLIndexKind::CLUSTERED_COLUMNSTORE;
	return params;
}

// The threshold is crossed: take the load's connection, open the shared
// session on it, resolve how many writers this INSERT may run, and stream the
// staged rows. Under write_mutex.
void OpenSharedStream(ClientContext &context, MSSQLInsertGlobalSinkState &gstate, const MSSQLInsertTarget &target,
					  const MSSQLInsertBulkPlan &bulk) {
	auto &catalog = Catalog::GetCatalog(context, Identifier(target.catalog_name)).Cast<MSSQLCatalog>();
	gstate.transaction_pinned = ConnectionProvider::IsInTransaction(context, catalog);
	gstate.pool = &catalog.GetConnectionPool();
	gstate.pool_handle = catalog.GetConnectionPoolHandle();
	gstate.reset_on_release = ConnectionProvider::ShouldResetOnRelease(context);

	// Spec 075 W3: inside a transaction the source scans of this catalog were
	// materialised under the catalog's MaterializeMutex before a row reached
	// this sink; taking it here is the guarantee that nothing is still
	// draining on the pinned connection the stream is about to use.
	std::unique_lock<std::mutex> materialize_lock;
	if (gstate.transaction_pinned) {
		materialize_lock = std::unique_lock<std::mutex>(catalog.MaterializeMutex());
	}

	auto connection = ConnectionProvider::GetConnection(context, catalog);
	if (!connection) {
		throw IOException("INSERT via BCP: failed to acquire a connection");
	}
	if (connection->GetState() != tds::ConnectionState::Idle) {
		if (!gstate.transaction_pinned) {
			// mssql_reset_connection is honoured by every release path in the
			// extension (issue #189); a raw pool Release() here would hand the
			// connection back carrying whatever reset bit it happened to have.
			connection->SetNeedsReset(gstate.reset_on_release);
			catalog.GetConnectionPool().Release(connection);
		}
		throw InvalidInputException(
			"INSERT via BCP: the transaction's connection is busy (state: %s) -- a result set is still being "
			"read on it. Materialise the source first, or run the INSERT outside the transaction.",
			tds::ConnectionStateToString(connection->GetState()));
	}

	// The target's shape as the server has it NOW, on the connection just
	// taken (Idle, one round trip): the writer rule below must not trust the
	// catalog's cached index_kind -- a table cached as a heap and given a
	// clustered index since (mssql_exec does not invalidate by default) would
	// fan out into the client-side deadlock the rule exists to prevent
	// (spec 062 self-review). The TABLOCK hint in the INSERT BULK text stays
	// the plan's; with one writer it serialises nothing.
	mssql::TableLoadShape live;
	try {
		live = mssql::TargetResolver::QueryTableShape(*connection, bulk.target);
	} catch (...) {
		if (!gstate.transaction_pinned) {
			connection->SetNeedsReset(gstate.reset_on_release);
			catalog.GetConnectionPool().Release(connection);
		}
		throw;
	}

	gstate.shared.Adopt(std::move(connection), SessionParams(gstate, bulk), gstate.transaction_pinned);

	// COPY's policy, and COPY's answer: JoinsTransaction. Inside a transaction
	// the pinned connection is the one writer; outside, up to
	// mssql_copy_parallel_writers, each in its own server transaction.
	const auto policy = MSSQLResolveLoadPolicy(bulk.target.is_temp_table, gstate.transaction_pinned,
											   MSSQLLoadTransactionRole::JoinsTransaction, bulk.configured_writers,
											   static_cast<uint64_t>(context.db->NumberOfThreads()));
	gstate.parallel_writer_limit = static_cast<idx_t>(policy.max_writers);

	// Then a rule COPY does not need, because COPY's writers commit every batch
	// and an INSERT's hold their locks until Finalize commits them together:
	// two writers whose locks conflict deadlock CLIENT-SIDE. Writer B waits on
	// a lock A's uncommitted rows hold, so the server stops reading B's stream,
	// so B's thread blocks in send(); A's commit is in Finalize, which waits
	// for B's Combine. The server sees no deadlock -- one side is a client --
	// and nothing times out (measured: a hang, 10 minutes and counting, on a
	// heap without TABLOCK; a 30 s BCP timeout on a clustered rowstore). Only
	// two shapes let concurrent transactional bulk loads coexist: a BARE heap
	// under TABLOCK (BU locks are mutually compatible, and nothing escalates)
	// and a clustered columnstore without it (each session fills its own
	// rowgroups; measured 1M rows in 0.49 s and 0.80 s at four writers).
	// Everything else -- a clustered rowstore index, a heap on row locks, a
	// columnstore under a table lock -- gets one writer.
	//
	// "Bare" is the whole of it: SQL Server's rule is "if the table has no
	// indexes and TABLOCK is specified, the table can be loaded concurrently
	// by multiple clients". A heap carrying a nonclustered index takes an
	// exclusive table lock instead, which is the deadlock above -- and
	// live.kind cannot see that, because every query behind it filters
	// index_id <= 1. has_nonclustered is the column that can.
	// "Bare" governs BOTH arms, measured on each (spec 062 § 6.5). The heap
	// half is the reviewed one: a nonclustered index turns the compatible BU
	// lock into Sch-M and the load stalls 30 s per extra writer. The
	// columnstore half was left open by that review and is WORSE -- 400k rows
	// into a clustered columnstore carrying one nonclustered index, four
	// writers: a writer times out reading its BCP response after 30 s, which
	// is an error rather than a failed claim, so the whole INSERT rolls back
	// and lands nothing. Both arms therefore want a target with no
	// nonclustered index on it.
	const bool bare = !live.has_nonclustered;
	const bool locks_compatible = bare && ((live.kind == MSSQLIndexKind::HEAP && bulk.tablock) ||
										   (live.kind == MSSQLIndexKind::CLUSTERED_COLUMNSTORE && !bulk.tablock));
	if (!locks_compatible) {
		gstate.parallel_writer_limit = 1;
	}
	if (live.has_nonclustered && live.kind != MSSQLIndexKind::CLUSTERED) {
		INSERT_SINK_LOG(1,
						"target carries a nonclustered index: one writer (concurrent transactional bulk loads "
						"need a target with no index on it -- measured, a heap takes Sch-M instead of BU and "
						"stalls, a columnstore fails its load outright)");
	}
	if (live.kind != bulk.shape) {
		INSERT_SINK_LOG(1, "target shape changed since the catalog cached it (%d -> %d): writer_limit=%llu",
						(int)bulk.shape, (int)live.kind, (unsigned long long)gstate.parallel_writer_limit);
	}

	INSERT_SINK_LOG(1, "stream opened after %llu staged row(s): pinned=%d, writer_limit=%llu, tablock in: %s",
					(unsigned long long)gstate.staged->Count(), gstate.transaction_pinned ? 1 : 0,
					(unsigned long long)gstate.parallel_writer_limit, bulk.insert_bulk_sql.c_str());

	// The staged rows go first, in arrival order.
	gstate.staged->Drain([&](DataChunk &chunk) { AccountWrite(gstate, gstate.shared.Write(chunk)); });
	gstate.staged.reset();
	gstate.streaming = true;
}

// Every session, abandoned: the shared one, the finished ones, and (through
// their own destructors) the ones still in local states.
void AbandonAll(MSSQLInsertGlobalSinkState &gstate) {
	gstate.shared.Abandon();
	std::lock_guard<std::mutex> lock(gstate.finished_mutex);
	for (auto &session : gstate.finished_sessions) {
		session->Abandon();
	}
	gstate.finished_sessions.clear();
}

}  // namespace

//===----------------------------------------------------------------------===//
// Sink Implementation
//===----------------------------------------------------------------------===//

SinkResultType MSSQLPhysicalInsert::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<MSSQLInsertGlobalSinkState>();

	if (!bulk_.enabled) {
		// Statement path. Thread-safe execution: one executor under one lock.
		std::lock_guard<std::mutex> lock(gstate.mutex);

		if (gstate.return_chunk) {
			// RETURNING mode - use ExecuteWithReturning; every statement's rows
			auto results = gstate.executor->ExecuteWithReturning(chunk, gstate.returning_column_ids);
			for (auto &result : results) {
				gstate.total_rows_inserted += result->size();
				gstate.result_chunks.push_back(std::move(result));
			}
		} else {
			// Normal mode - just count rows
			idx_t rows_inserted = gstate.executor->Execute(chunk);
			gstate.total_rows_inserted += rows_inserted;
		}

		return SinkResultType::NEED_MORE_INPUT;
	}

	//===------------------------------------------------------------------===//
	// Bulk path (spec 062 W2)
	//===------------------------------------------------------------------===//
	if (chunk.size() == 0) {
		return SinkResultType::NEED_MORE_INPUT;
	}
	if (context.client.IsInterrupted()) {
		throw InterruptException();
	}
	if (gstate.has_error.load(std::memory_order_acquire)) {
		std::lock_guard<std::mutex> error_lock(gstate.error_mutex);
		throw IOException("INSERT via BCP: a parallel writer failed: %s", gstate.error_message);
	}

	auto &lstate = input.local_state.Cast<MSSQLInsertLocalSinkState>();

	// Staging phase, under the write lock: the rows are held until the
	// threshold says which path they take. The chunk that crosses it opens the
	// stream and goes with the drained buffer.
	//
	// The error check is repeated UNDER the lock, here and at the shared write
	// below: a thread that passed the check above and then waited on the lock
	// while the holder abandoned the shared session must not open a second
	// stream on the closed connection, or write to a session with no writer.
	{
		std::unique_lock<std::mutex> lock(gstate.write_mutex);
		if (gstate.has_error.load(std::memory_order_acquire)) {
			std::lock_guard<std::mutex> error_lock(gstate.error_mutex);
			throw IOException("INSERT via BCP: a parallel writer failed: %s", gstate.error_message);
		}
		if (!gstate.streaming) {
			gstate.staged->Append(chunk);
			if (!gstate.staged->Exceeds(bulk_.threshold)) {
				return SinkResultType::NEED_MORE_INPUT;
			}
			try {
				OpenSharedStream(context.client, gstate, target_, bulk_);
			} catch (std::exception &e) {
				const string msg = BulkFailureMessage(gstate, gstate.shared, MSSQLRawMessage(e));
				gstate.shared.Abandon();
				RecordBulkError(gstate, msg);
				throw IOException("%s", msg);
			}
			return SinkResultType::NEED_MORE_INPUT;
		}
	}

	// Streaming. A thread may claim a writer of its own (spec 070 W2: asked on
	// every chunk while the warm-up gate may still open), else it appends to
	// the shared session under the lock -- COPY's loop.
	if (!lstate.init_attempted) {
		lstate.init_attempted = true;
		lstate.may_claim = gstate.parallel_writer_limit > 1;
	}
	if (lstate.may_claim && !lstate.session) {
		auto session = make_uniq<mssql::BulkLoadSession>();
		switch (session->TryStart(SessionParams(gstate, bulk_), gstate.parallel_writers_used,
								  gstate.parallel_writer_limit, gstate.rows_sent)) {
		case mssql::BulkLoadSession::Claim::Started:
			lstate.session = std::move(session);
			INSERT_SINK_LOG(1, "parallel writer started (used=%llu/%llu)",
							(unsigned long long)gstate.parallel_writers_used.load(),
							(unsigned long long)gstate.parallel_writer_limit);
			break;
		case mssql::BulkLoadSession::Claim::GateClosed:
			break;
		case mssql::BulkLoadSession::Claim::Unavailable:
			lstate.may_claim = false;
			break;
		}
	}

	if (lstate.session) {
		try {
			AccountWrite(gstate, lstate.session->Write(chunk));
		} catch (std::exception &e) {
			const string msg = BulkFailureMessage(gstate, *lstate.session, MSSQLRawMessage(e));
			lstate.session->Abandon();
			RecordBulkError(gstate, msg);
			throw IOException("%s", msg);
		}
		return SinkResultType::NEED_MORE_INPUT;
	}

	std::lock_guard<std::mutex> lock(gstate.write_mutex);
	if (gstate.has_error.load(std::memory_order_acquire)) {
		std::lock_guard<std::mutex> error_lock(gstate.error_mutex);
		throw IOException("INSERT via BCP: a parallel writer failed: %s", gstate.error_message);
	}
	try {
		AccountWrite(gstate, gstate.shared.Write(chunk));
	} catch (std::exception &e) {
		const string msg = BulkFailureMessage(gstate, gstate.shared, MSSQLRawMessage(e));
		gstate.shared.Abandon();
		RecordBulkError(gstate, msg);
		throw IOException("%s", msg);
	}
	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType MSSQLPhysicalInsert::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	if (!bulk_.enabled) {
		// Nothing to combine - we use a global executor with mutex
		return SinkCombineResultType::FINISHED;
	}
	auto &gstate = input.global_state.Cast<MSSQLInsertGlobalSinkState>();
	auto &lstate = input.local_state.Cast<MSSQLInsertLocalSinkState>();
	if (!lstate.session) {
		return SinkCombineResultType::FINISHED;
	}
	// This thread's stream closes here -- DONE, the server's confirmation --
	// but its connection and its open transaction move to the global state:
	// the commit waits until every writer has closed (Finalize), and a local
	// state does not live that long.
	const idx_t batch_no = lstate.session->BatchesFlushed() + 1;
	const idx_t rows_in_batch = lstate.session->RowsInBatch();
	try {
		const idx_t confirmed = lstate.session->CloseStream();
		gstate.rows_confirmed.fetch_add(confirmed, std::memory_order_relaxed);
		if (confirmed > 0) {
			gstate.batches_flushed.fetch_add(1, std::memory_order_relaxed);
		}
	} catch (std::exception &e) {
		// CloseStream abandoned the session before rethrowing.
		const string msg = BulkFailureMessage(gstate, batch_no, rows_in_batch, MSSQLRawMessage(e));
		RecordBulkError(gstate, msg);
		throw IOException("%s", msg);
	}
	std::lock_guard<std::mutex> lock(gstate.finished_mutex);
	gstate.finished_sessions.push_back(std::move(lstate.session));
	return SinkCombineResultType::FINISHED;
}

SinkFinalizeType MSSQLPhysicalInsert::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
											   OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<MSSQLInsertGlobalSinkState>();

	if (!bulk_.enabled) {
		// Thread-safe finalization
		std::lock_guard<std::mutex> lock(gstate.mutex);

		if (gstate.return_chunk) {
			// RETURNING mode - finalize with output
			auto result = gstate.executor->FinalizeWithReturning();
			if (result && result->size() > 0) {
				gstate.total_rows_inserted += result->size();
				gstate.result_chunks.push_back(std::move(result));
			}
		} else {
			// Normal mode - just finalize
			gstate.executor->Finalize();
		}

		return SinkFinalizeType::READY;
	}

	//===------------------------------------------------------------------===//
	// Bulk path (spec 062 W2)
	//===------------------------------------------------------------------===//
	std::lock_guard<std::mutex> lock(gstate.write_mutex);

	if (gstate.has_error.load(std::memory_order_acquire)) {
		AbandonAll(gstate);
		std::lock_guard<std::mutex> error_lock(gstate.error_mutex);
		throw IOException("%s", gstate.error_message);
	}

	if (!gstate.streaming) {
		// The whole INSERT fitted under the threshold: the staged rows go as
		// statements, through the same executor and the same one-transaction
		// bracket the statement path has (W1c). Byte for byte what the INSERT
		// sent before the bulk path existed.
		INSERT_SINK_LOG(1, "%llu row(s) under the threshold of %llu: statement path",
						(unsigned long long)gstate.staged->Count(), (unsigned long long)bulk_.threshold);
		gstate.staged->Drain([&](DataChunk &chunk) { gstate.total_rows_inserted += gstate.executor->Execute(chunk); });
		gstate.executor->Finalize();
		gstate.total_rows_inserted = gstate.executor->GetTotalRowsInserted();
		gstate.staged.reset();
		return SinkFinalizeType::READY;
	}

	// Close the shared stream, then commit every writer and return every
	// connection. Between the first and the last commit of a multi-writer
	// load a failed commit leaves the earlier writers' rows in place -- the
	// two-phase window of any multi-connection load; one writer (every load
	// inside a transaction) has none.
	{
		const idx_t batch_no = gstate.shared.BatchesFlushed() + 1;
		const idx_t rows_in_batch = gstate.shared.RowsInBatch();
		try {
			const idx_t confirmed = gstate.shared.CloseStream();
			gstate.rows_confirmed.fetch_add(confirmed, std::memory_order_relaxed);
			if (confirmed > 0) {
				gstate.batches_flushed.fetch_add(1, std::memory_order_relaxed);
			}
		} catch (std::exception &e) {
			const string msg = BulkFailureMessage(gstate, batch_no, rows_in_batch, MSSQLRawMessage(e));
			AbandonAll(gstate);
			throw IOException("%s", msg);
		}
	}
	// Counted, not inferred: the catch below has to say how much of the load
	// survived, and only the number of COMMITs that returned knows that.
	idx_t committed_writers = 0;
	try {
		gstate.shared.Commit();
		committed_writers++;
		{
			std::lock_guard<std::mutex> finished_lock(gstate.finished_mutex);
			for (auto &session : gstate.finished_sessions) {
				session->Commit();
				committed_writers++;
			}
		}
	} catch (std::exception &e) {
		// Each writer's COMMIT is its own round trip, so a failure part-way
		// through leaves every writer that already committed in place. Only the
		// one-writer case (every load inside a transaction, and any load the
		// rule above demoted) can honestly claim nothing landed -- claiming it
		// for a multi-writer load invites a retry onto a half-loaded table.
		const idx_t committed = gstate.rows_confirmed.load();
		string outcome;
		if (committed_writers == 0) {
			outcome = MSSQLRowsBeforeOutcome(gstate.transaction_pinned, 0);
		} else {
			outcome = StringUtil::Format(
				"%llu writer(s) had already committed, so up to %llu row(s) are in the table and were NOT rolled "
				"back -- check the table before retrying",
				(unsigned long long)committed_writers, (unsigned long long)committed);
		}
		const string msg = StringUtil::Format("INSERT via BCP failed to commit: %s; %s", MSSQLRawMessage(e), outcome);
		AbandonAll(gstate);
		throw IOException("%s", msg);
	}
	gstate.shared.Release();
	{
		std::lock_guard<std::mutex> finished_lock(gstate.finished_mutex);
		for (auto &session : gstate.finished_sessions) {
			session->Release();
		}
		gstate.finished_sessions.clear();
	}

	gstate.total_rows_inserted = gstate.rows_confirmed.load();
	INSERT_SINK_LOG(
		1, "done: sent=%llu confirmed=%llu batches=%llu writers=%llu/%llu", (unsigned long long)gstate.rows_sent.load(),
		(unsigned long long)gstate.rows_confirmed.load(), (unsigned long long)gstate.batches_flushed.load(),
		(unsigned long long)gstate.parallel_writers_used.load(), (unsigned long long)gstate.parallel_writer_limit);
	return SinkFinalizeType::READY;
}

//===----------------------------------------------------------------------===//
// Source Implementation (for returning results)
//===----------------------------------------------------------------------===//

SourceResultType MSSQLPhysicalInsert::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
													  OperatorSourceInput &input) const {
	auto &gstate = sink_state->Cast<MSSQLInsertGlobalSinkState>();

	// Thread-safe access
	std::lock_guard<std::mutex> lock(gstate.mutex);

	if (gstate.return_chunk) {
		// RETURNING mode - return accumulated result chunks
		if (gstate.result_chunk_index >= gstate.result_chunks.size()) {
			return SourceResultType::FINISHED;
		}

		// Get the next result chunk
		auto &result = gstate.result_chunks[gstate.result_chunk_index];
		gstate.result_chunk_index++;

		// Reference the data from result chunk
		// This avoids copying and uses the already-parsed data directly
		chunk.Reference(*result);

		return gstate.result_chunk_index >= gstate.result_chunks.size() ? SourceResultType::FINISHED
																		: SourceResultType::HAVE_MORE_OUTPUT;
	} else {
		// Normal mode - return row count
		if (gstate.returned) {
			return SourceResultType::FINISHED;
		}

		gstate.returned = true;

		// Return the count
		chunk.SetChildCardinality(1);
		chunk.SetValue(0, 0, Value::BIGINT(static_cast<int64_t>(gstate.total_rows_inserted)));

		return SourceResultType::FINISHED;
	}
}

}  // namespace duckdb
