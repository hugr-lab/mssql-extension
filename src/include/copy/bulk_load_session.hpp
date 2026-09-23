//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// copy/bulk_load_session.hpp
//
// One thread's own bulk-load session: its connection, its INSERT BULK, its
// writer (spec 063 D2).
//
// COPY and CTAS grew this in the same week and in parallel, and the two copies
// had already drifted nine ways. Two more consumers are coming — INSERT via BCP
// (spec 062) and the `#temp` staging table UPDATE/DELETE will fill — so the
// sequence lives here once:
//
//     claim a slot -> acquire -> INSERT BULK -> COLMETADATA
//                  -> write, flush at the threshold, re-open
//                  -> DONE -> Finalize -> release
//
// What is deliberately NOT here, and why. Who may open a session at all, and how
// many, is `MSSQLResolveLoadPolicy` in copy/load_policy.hpp — a property of the
// TARGET and the transaction, not of the writer. This type is handed the answer.
// Getting that boundary wrong is how CTAS ended up loading through the
// transaction's pinned connection, which cannot stream a result set and receive
// a bulk load at once (spec 057).
//
// Accounting stays with the CALLER. COPY folds into global atomics as it goes;
// CTAS accumulates per thread and folds in Combine. That difference is drift
// worth resolving, but resolving it inside a commit that MOVES this code would
// leave a reviewer unable to tell what moved from what changed — so Write() and
// Finish() report what happened and the operator records it as it does today
// (spec 063 D5 unifies it next).
//===----------------------------------------------------------------------===//

#pragma once

#include <atomic>

#include "copy/bcp_writer.hpp"
#include "copy/load_transaction.hpp"
#include "copy/target_resolver.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "tds/tds_connection.hpp"
#include "tds/tds_connection_pool.hpp"
#include "tds/tds_types.hpp"

namespace duckdb {
namespace mssql {

//! What one Write() did. Returned rather than accumulated so each operator keeps
//! the accounting shape it has today.
struct BulkLoadWriteResult {
	//! Rows handed to the server by this call.
	idx_t rows_written = 0;
	//! Rows the server confirmed, non-zero only when this call crossed the batch
	//! threshold and flushed.
	idx_t rows_confirmed = 0;
	//! Did this call close a batch and open the next one?
	bool flushed = false;
	//! Encode + send, and the flush, in nanoseconds. Zero unless the caller asked
	//! for timings — the phases MSSQL_COUNTERS reports are measured HERE because
	//! the work is here, and a caller timing around this call would attribute the
	//! flush to the encode.
	uint64_t encode_ns = 0;
	uint64_t flush_ns = 0;
};

//! Everything a session needs that does not change between chunks. Passed once
//! to TryStart so the per-chunk path carries no lookups.
struct BulkLoadSessionParams {
	//! The pool to acquire from. NEVER a ConnectionProvider: inside a transaction
	//! the provider returns the PINNED connection, and two writers on one
	//! connection interleave their ROW tokens into a single bulk load.
	tds::ConnectionPool *pool = nullptr;
	//! Captured for the destructor, which may run on a worker thread and must not
	//! reach a catalog or a ClientContext (issue #178).
	weak_ptr<tds::ConnectionPool> pool_handle;
	//! Re-executed at every batch boundary, so it is held rather than rebuilt.
	const string *insert_bulk_sql = nullptr;
	const BCPCopyTarget *target = nullptr;
	const vector<BCPColumnMetadata> *columns = nullptr;
	//! COPY's name-based source-to-target mapping; null for a positional load.
	const vector<int32_t> *column_mapping = nullptr;
	//! Rows per batch — the boundary the SERVER sees between DONE tokens.
	//! 0 disables intermediate flushes.
	idx_t flush_rows = 0;
	//! Fill BulkLoadWriteResult's timings.
	bool collect_timings = false;
	//! `mssql_reset_connection`, resolved on the client thread. Carried because
	//! the release happens in a destructor that may run on a worker thread, where
	//! there is no ClientContext to ask (issue #178).
	bool reset_on_release = tds::DEFAULT_RESET_CONNECTION;
	//! Spec 062 W2: bracket this session's load in a server transaction of its
	//! own -- BEGIN TRANSACTION before the first INSERT BULK, COMMIT from
	//! Commit(), ROLLBACK on Abandon or destruction. For a connection the
	//! session acquires itself (TryStart) this is the only place the bracket
	//! can go, since the acquisition and the first INSERT BULK are one step;
	//! an adopted connection gets it on the same flag, so the operator's
	//! shared session and its parallel ones are treated alike. Never on a
	//! pinned connection: the DuckDB transaction owns that one, and Adopt
	//! ignores the flag for it. What an INSERT is -- atomic -- and what a COPY
	//! is not (each batch commits, as bcp's does): COPY and CTAS leave it off.
	bool own_transaction = false;
	//! Spec 070 W2: apply the warm-up gate (hold extra writers until one
	//! flush_rows batch has landed). True only for a COLUMNSTORE target, where a
	//! second writer splitting a sub-threshold load costs compression. A heap or
	//! rowstore target has no such cost, so it fans out immediately (the pre-W2
	//! behaviour) and the warm-up serialization — measured 1.2-1.3x on a large
	//! parallel load — is not paid where it buys nothing.
	bool warmup_gate = false;
};

//! Build the `INSERT BULK` statement that opens a bulk load.
//!
//! One builder for every consumer, which is what closes the drift it was written
//! to close: CTAS emitted only `WITH (TABLOCK)` and never `ROWS_PER_BATCH`, so
//! the server was told the batch size for a COPY and left to guess it for a
//! CTAS. Two builders is how that happens; one cannot.
//!
//! @param target        resolved target — a temp table is named WITHOUT its
//!                      schema, because `#name` lives in tempdb and qualifying
//!                      it makes the server look in the wrong place
//! @param columns       column list, bracketed, with each column's own SQL
//!                      Server type declaration (exact for an existing table,
//!                      generated for one being created)
//! @param tablock       resolved TABLOCK decision — the POLICY belongs to the
//!                      caller (MSSQLResolveTablock reads the target's shape);
//!                      this only renders it
//! @param rows_per_batch `flush_rows`, told to the server up front so it can
//!                      size the load. 0 omits the hint.
//! The INSERT BULK hints that make a bulk load behave like an INSERT statement
//! (spec 062 W2). By default a bulk load IGNORES check constraints, does NOT
//! fire triggers, and puts a column's DEFAULT where the stream says NULL --
//! bcp's contract, and COPY's. An INSERT statement does the opposite on all
//! three, and an INSERT that went through INSERT BULK must too: measured, a
//! 1000-row INSERT with a CHECK violation in row 950 loaded all 1000 rows.
struct InsertBulkHints {
	bool check_constraints = false;
	bool fire_triggers = false;
	bool keep_nulls = false;

	//! Every hint on: what an INSERT statement does.
	static InsertBulkHints StatementSemantics() {
		InsertBulkHints h;
		h.check_constraints = true;
		h.fire_triggers = true;
		h.keep_nulls = true;
		return h;
	}
};

string BuildInsertBulkSql(const BCPCopyTarget &target, const vector<BCPColumnMetadata> &columns, bool tablock,
						  idx_t rows_per_batch, const InsertBulkHints &hints = InsertBulkHints());

//! A bulk-load session owned by ONE thread. Not thread-safe and not meant to be:
//! a thread either owns one of these or shares the operator's global session
//! under the operator's lock.
//!
//! Two ways in, one object (spec 062 W0). `TryStart` claims a slot and a POOL
//! connection for a parallel writer and may decline; `Adopt` takes a connection
//! the operator already holds — the transaction's pinned one, or the pool
//! connection the operator's init acquired — and never declines. From there the
//! two are the same session: the stream opens on the first Write (spec 075 W3:
//! never at init, where inside a transaction the source may still be draining
//! on the very same pinned connection), batches close and reopen at
//! `flush_rows`, Finish sends the last DONE and returns the connection to
//! whoever owns it. COPY, CTAS and INSERT all run their shared writer through
//! this; before W0 each of the first two carried its own copy of the sequence,
//! and INSERT would have been the third.
class BulkLoadSession {
public:
	BulkLoadSession() = default;

	//! Last-resort release. A throw from the sink skips Combine, so nothing else
	//! would return this connection: it would sit in Executing with a bulk-load
	//! transaction open, holding locks on the target, until the pool was torn
	//! down (issue #191). Touches no ClientContext and no catalog — every release
	//! target was captured on the thread that acquired the connection (#178).
	~BulkLoadSession() noexcept;

	BulkLoadSession(const BulkLoadSession &) = delete;
	BulkLoadSession &operator=(const BulkLoadSession &) = delete;

	//! Claim a slot and open a session, or leave this thread sharing the global
	//! writer.
	//!
	//! `slots_used` counts sessions handed out INCLUDING the operator's first
	//! one, and the slot is claimed before any work so N threads racing here
	//! cannot collectively exceed `max_writers`.
	//!
	//! Failure is NOT an error and never throws: a pool at its limit, a server
	//! refusing another bulk load, anything — the thread falls back to the shared
	//! writer, which is the pre-parallel behaviour and always correct. A load must
	//! not fail because it could not go faster.
	//!
	//! `rows_sunk` is the load's running total across all writers. The warm-up
	//! gate (columnstore targets only) holds every extra writer until that total
	//! reaches one `flush_rows` batch, so the shared writer lands the first
	//! rowgroup compressed before any second writer dilutes it; a large load pays
	//! only that one batch before fanning out to the full writer count.
	//!
	//! The THREE outcomes matter to the caller, which is why this is not a bool
	//! (spec 070 W2 review): `GateClosed` is cheap and transient — no slot
	//! claimed, no connection touched — so the caller must keep asking on later
	//! chunks. `Busy` is transient too: a slot was free but no connection was,
	//! and the connection is asked for with TryAcquire, which never waits, so
	//! asking again on a later chunk is cheap -- one lost momentary race must not
	//! cost the thread its writer for the whole load (review of #382).
	//! `Unavailable` is terminal for this load (limit is one, the slot cap is
	//! reached, or the server refused the bulk load on the connection it got):
	//! the caller must STOP asking. Only `Started` means this thread now owns a
	//! session.
	enum class Claim { Started, GateClosed, Busy, Unavailable };
	Claim TryStart(const BulkLoadSessionParams &params, std::atomic<idx_t> &slots_used, idx_t max_writers,
				   const std::atomic<idx_t> &rows_sunk);

	//! Take over a connection the operator holds. No wire traffic: the stream
	//! opens on the first Write. `transaction_pinned` says whose the connection
	//! is — a pinned one is handed back to its transaction untouched on every
	//! path (Finish drops the reference, the error paths close it and drop the
	//! reference, as ReleaseBcpConnectionOnError always did); a pool connection
	//! is released with `params.reset_on_release`, which is `mssql_reset_connection`
	//! honoured on this release path like on every other (issue #189) — COPY's
	//! own success path used to skip it.
	void Adopt(std::shared_ptr<tds::TdsConnection> connection, const BulkLoadSessionParams &params,
			   bool transaction_pinned);

	//! Does this session hold a connection and a writer? For a per-thread
	//! session: false means "use the shared writer".
	bool IsOwned() const {
		return writer_ != nullptr;
	}

	//! Has INSERT BULK + COLMETADATA gone down the connection? False until the
	//! first Write, and after Finish.
	bool IsOpen() const {
		return stream_open_;
	}

	//! Write one chunk, opening the stream if this is the first, flushing and
	//! re-opening the batch at the threshold. Throws on a protocol or server
	//! error; the caller owns the failure policy.
	BulkLoadWriteResult Write(DataChunk &chunk);

	//! DONE for whatever is unflushed and the server's confirmation; the
	//! connection stays with the session, its transaction (if own) still open.
	//! The first of the three steps Finish() runs, on its own for a caller
	//! that has more to do on the connection before it goes back -- the INSERT
	//! sink commits every writer's transaction only once all of them have
	//! closed their streams (spec 062 W2), and spec 066 runs its UPDATE on the
	//! connection that filled the `#temp`.
	//!
	//! DONE is sent whenever the stream is open, even for zero rows: INSERT BULK
	//! left the connection in Executing, and only DONE closes the stream so it
	//! can be pooled again. A session whose stream never opened — an empty
	//! source, or every chunk went to other writers — sends nothing.
	//!
	//! A failure here (the server rejecting the final batch) releases the
	//! connection the error way — closed, so the server rolls the load back and
	//! drops its locks — and rethrows.
	//!
	//! @return rows the server confirmed for the final batch.
	idx_t CloseStream();

	//! COMMIT the session's own transaction (a no-op without one). Throws when
	//! the server refuses; the connection is then released the error way.
	void Commit();

	//! The connection goes back to whoever owns it (see Adopt). A no-op once
	//! released.
	void Release();

	//! CloseStream, Commit, Release: the whole ending, for a caller with
	//! nothing else to do on the connection (COPY, CTAS).
	//!
	//! @return rows the server confirmed for the final batch.
	idx_t Finish();

	//! Close this thread's stream and return its connection WITHOUT completing
	//! the load; an own transaction is rolled back with it. For the failure
	//! path, where the connection is sitting mid-bulk-load on the very table a
	//! cleanup DROP has to lock.
	void Abandon() noexcept;

	//! Is the connection the DuckDB transaction's pinned one?
	bool IsPinned() const {
		return transaction_pinned_;
	}

	//! Rows sent since the last flush — what Finish() reports in DONE.
	idx_t RowsInBatch() const {
		return rows_in_batch_;
	}

	//! Batches this session closed at the threshold, the final one excluded.
	idx_t BatchesFlushed() const {
		return batches_flushed_;
	}

	//! The writer's wire counters (spec 057 step 0b), snapshotted when the
	//! writer is torn down — Finish and Abandon both — because the summary
	//! prints after that, and reading a destroyed writer there gave zeroes.
	uint64_t BuildSendNs() const {
		return counter_build_send_ns_;
	}
	uint64_t ServerWaitNs() const {
		return counter_server_wait_ns_;
	}
	idx_t SendCalls() const {
		return counter_send_calls_;
	}

private:
	//! INSERT BULK, Idle -> Executing, COLMETADATA: the three steps that open a
	//! stream, run for the first chunk and again after every FlushBatch.
	void OpenStream();
	//! Re-open the batch after FlushBatch closed it.
	void ReopenBatch();
	//! Read the writer's counters before it goes.
	void SnapshotWriterCounters();
	//! The normal release: pinned stays pinned, pooled goes back with the reset flag.
	void ReleaseConnection();

	std::shared_ptr<tds::TdsConnection> connection_;
	unique_ptr<BCPWriter> writer_;
	//! The ONE release mechanism. There was briefly a raw `tds::ConnectionPool *`
	//! beside this, used by Finish() while the destructor used the handle — two
	//! ways to return a connection in one class, which is how they come to
	//! disagree. Finish() runs inside the statement so a raw pointer was safe
	//! there; the point is that nothing now has to know that.
	weak_ptr<tds::ConnectionPool> pool_handle_;
	//! Own copy: re-executed at every batch boundary, and the operator's string
	//! may not outlive a session moved out of its local state (spec 062 W2).
	string insert_bulk_sql_;
	idx_t flush_rows_ = 0;
	bool collect_timings_ = false;
	bool reset_on_release_ = tds::DEFAULT_RESET_CONNECTION;
	//! Adopt only: the connection is the DuckDB transaction's, never released here.
	bool transaction_pinned_ = false;
	bool stream_open_ = false;
	//! The session's own server transaction (spec 062 W2), begun when the
	//! params asked for one and the connection is not pinned.
	LoadTransaction transaction_;

	idx_t rows_in_batch_ = 0;
	idx_t batches_flushed_ = 0;

	uint64_t counter_build_send_ns_ = 0;
	uint64_t counter_server_wait_ns_ = 0;
	idx_t counter_send_calls_ = 0;
};

}  // namespace mssql
}  // namespace duckdb
