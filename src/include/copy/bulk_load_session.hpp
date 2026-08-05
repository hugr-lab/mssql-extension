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
string BuildInsertBulkSql(const BCPCopyTarget &target, const vector<BCPColumnMetadata> &columns, bool tablock,
						  idx_t rows_per_batch);

//! A bulk-load session owned by ONE thread. Not thread-safe and not meant to be:
//! a thread either owns one of these or shares the operator's global writer.
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
	//! @return true when this thread now owns a session.
	bool TryStart(const BulkLoadSessionParams &params, std::atomic<idx_t> &slots_used, idx_t max_writers);

	//! Does this thread own a session? False means "use the shared writer".
	bool IsOwned() const {
		return writer_ != nullptr;
	}

	//! Write one chunk, flushing and re-opening the batch at the threshold.
	//! Throws on a protocol or server error; the caller owns the failure policy.
	BulkLoadWriteResult Write(DataChunk &chunk);

	//! DONE for whatever is unflushed, the server's confirmation, then the
	//! connection goes back to the pool.
	//!
	//! DONE is sent even for zero rows: INSERT BULK left the connection in
	//! Executing, and only DONE closes the stream so it can be pooled again.
	//!
	//! @return rows the server confirmed for the final batch.
	idx_t Finish();

	//! Close this thread's stream and return its connection WITHOUT completing
	//! the load. For the failure path, where the connection is sitting
	//! mid-bulk-load on the very table a cleanup DROP has to lock.
	void Abandon() noexcept;

	//! Rows sent since the last flush — what Finish() reports in DONE.
	idx_t RowsInBatch() const {
		return rows_in_batch_;
	}

	//! Batches this session closed at the threshold, the final one excluded.
	idx_t BatchesFlushed() const {
		return batches_flushed_;
	}

private:
	//! Re-open the batch after FlushBatch closed it: INSERT BULK again, back to
	//! Executing, fresh COLMETADATA.
	void ReopenBatch();

	std::shared_ptr<tds::TdsConnection> connection_;
	unique_ptr<BCPWriter> writer_;
	weak_ptr<tds::ConnectionPool> pool_handle_;
	tds::ConnectionPool *pool_ = nullptr;
	const string *insert_bulk_sql_ = nullptr;
	idx_t flush_rows_ = 0;
	bool collect_timings_ = false;
	bool reset_on_release_ = tds::DEFAULT_RESET_CONNECTION;

	idx_t rows_in_batch_ = 0;
	idx_t batches_flushed_ = 0;
};

}  // namespace mssql
}  // namespace duckdb
