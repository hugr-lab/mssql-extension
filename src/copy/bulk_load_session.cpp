#include "copy/bulk_load_session.hpp"

#include <chrono>

#include "connection/mssql_connection_provider.hpp"
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

BulkLoadSession::~BulkLoadSession() noexcept {
	// The writer goes first: it holds a reference to the connection, and the
	// release protocol closes the socket underneath it.
	writer_.reset();
	ReleaseBcpConnectionOnError(connection_, pool_handle_, /*transaction_pinned=*/false);
}

bool BulkLoadSession::TryStart(const BulkLoadSessionParams &params, std::atomic<idx_t> &slots_used, idx_t max_writers) {
	if (max_writers <= 1) {
		return false;
	}
	// Claim a slot before doing any work, so N threads racing here cannot
	// collectively exceed the limit.
	const idx_t slot = slots_used.fetch_add(1);
	if (slot >= max_writers) {
		slots_used.fetch_sub(1);
		return false;
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
		pool_ = params.pool;
		insert_bulk_sql_ = params.insert_bulk_sql;
		flush_rows_ = params.flush_rows;
		collect_timings_ = params.collect_timings;
		connection_ = conn;
		writer_ = make_uniq<BCPWriter>(*connection_, *params.target, *params.columns,
									   params.column_mapping ? *params.column_mapping : vector<int32_t>());
		// The stream opens with COLMETADATA; without it the server has no schema
		// for the ROW tokens that follow.
		writer_->WriteColmetadata();
		return true;
	} catch (std::exception &) {
		// Falling back is the whole contract: put the connection back and let the
		// thread share the global writer.
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
		return false;
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
		if (pool_) {
			pool_->Release(connection_);
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
	ReleaseBcpConnectionOnError(connection_, pool_handle_, /*transaction_pinned=*/false);
	rows_in_batch_ = 0;
}

}  // namespace mssql
}  // namespace duckdb
