#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include "copy/bulk_load_session.hpp"
#include "dml/insert/mssql_insert_bulk_plan.hpp"
#include "dml/insert/mssql_insert_config.hpp"
#include "dml/insert/mssql_insert_error.hpp"
#include "dml/insert/mssql_insert_target.hpp"
#include "dml/mssql_staged_rows.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "tds/tds_connection_pool.hpp"

namespace duckdb {

// Forward declaration
class MSSQLInsertExecutor;

//===----------------------------------------------------------------------===//
// MSSQLPhysicalInsert - Physical operator for INSERT into SQL Server
//
// This operator receives rows from its child operator and inserts them
// into the target SQL Server table.
//
// Two paths (spec 062):
// - Statement path: batched INSERT ... VALUES statements through
//   MSSQLInsertExecutor, one server transaction per statement (W1c). Always
//   for RETURNING (OUTPUT INSERTED), for an explicit identity column, under
//   mssql_insert_use_bcp = false -- and for any INSERT of at most
//   mssql_insert_bcp_threshold rows.
// - Bulk path: the rows are staged until the threshold, then streamed through
//   INSERT BULK on the operator's own BulkLoadSession (the transaction's pinned
//   connection, or a pool connection inside a server transaction of its own),
//   with parallel writers by the same policy COPY uses (W2).
//
// Modes:
// - Without RETURNING: Returns count of inserted rows
// - With RETURNING: Returns the inserted rows with OUTPUT INSERTED values
//===----------------------------------------------------------------------===//

class MSSQLPhysicalInsert : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

	// Constructor - must take PhysicalPlan& as first argument for planner.Make<T>()
	// @param plan Physical plan reference (passed by planner.Make)
	// @param types Result types (count or returning columns)
	// @param estimated_cardinality Expected row count
	// @param target Insert target metadata
	// @param config Insert configuration
	// @param return_chunk Whether to return inserted data (RETURNING mode)
	// @param bulk The bulk path's plan; `enabled` false keeps the statement path
	MSSQLPhysicalInsert(PhysicalPlan &plan, vector<LogicalType> types, idx_t estimated_cardinality,
						MSSQLInsertTarget target, MSSQLInsertConfig config, bool return_chunk,
						MSSQLInsertBulkPlan bulk);

	//===----------------------------------------------------------------------===//
	// Target Information
	//===----------------------------------------------------------------------===//

	// Get insert target
	const MSSQLInsertTarget &GetTarget() const {
		return target_;
	}

	// Get insert configuration
	const MSSQLInsertConfig &GetConfig() const {
		return config_;
	}

	// Check if in RETURNING mode
	bool ReturnsChunk() const {
		return return_chunk_;
	}

	const MSSQLInsertBulkPlan &GetBulkPlan() const {
		return bulk_;
	}

public:
	//===----------------------------------------------------------------------===//
	// PhysicalOperator Interface
	//===----------------------------------------------------------------------===//

	// Get operator string for EXPLAIN
	string GetName() const override {
		return bulk_.enabled ? "MSSQL_INSERT_BCP" : "MSSQL_INSERT";
	}

	// Check if this is a sink operator
	bool IsSink() const override {
		return true;
	}

	//! The bulk path may be fed from several threads: each either claims a
	//! writer of its own or appends to the shared session under its lock. The
	//! statement path keeps one executor under one mutex, so one thread is
	//! all it can use.
	bool ParallelSink() const override {
		return bulk_.enabled;
	}

	// Order preservation type
	OrderPreservationType SourceOrder() const override {
		return OrderPreservationType::NO_ORDER;
	}

	//===----------------------------------------------------------------------===//
	// Sink Interface
	//===----------------------------------------------------------------------===//

	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;

	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;

	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
							  OperatorSinkFinalizeInput &input) const override;

	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;

	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;

	//===----------------------------------------------------------------------===//
	// Source Interface (for returning results)
	//===----------------------------------------------------------------------===//

	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
									 OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}

private:
	MSSQLInsertTarget target_;
	MSSQLInsertConfig config_;
	bool return_chunk_;
	MSSQLInsertBulkPlan bulk_;
};

//===----------------------------------------------------------------------===//
// MSSQLInsertGlobalSinkState - Global state for INSERT operator
//===----------------------------------------------------------------------===//

class MSSQLInsertGlobalSinkState : public GlobalSinkState {
public:
	explicit MSSQLInsertGlobalSinkState(ClientContext &context, const MSSQLInsertTarget &target,
										const MSSQLInsertConfig &config, bool return_chunk,
										const MSSQLInsertBulkPlan &bulk);

	// The insert executor (statement path, and the staged rows of a bulk-mode
	// INSERT that never crossed the threshold)
	unique_ptr<MSSQLInsertExecutor> executor;

	// Total rows inserted
	idx_t total_rows_inserted;

	// Has data been returned (for source)
	bool returned;

	// RETURNING mode flag
	bool return_chunk;

	// Returning column indices
	vector<idx_t> returning_column_ids;

	// Accumulated result chunks for RETURNING mode
	vector<unique_ptr<DataChunk>> result_chunks;

	// Current index when returning chunks
	idx_t result_chunk_index;

	// Mutex for thread-safe access (the statement path, and the source)
	mutable std::mutex mutex;

	//===------------------------------------------------------------------===//
	// Bulk path (spec 062 W2)
	//===------------------------------------------------------------------===//

	//! Rows held until the threshold decides the path (W2a). Null once the
	//! stream is open and they have been drained into it.
	unique_ptr<MSSQLStagedRows> staged;
	//! The threshold was crossed: the shared session's stream is open and
	//! every later chunk streams.
	bool streaming = false;

	//! The operator's own writer -- the transaction's pinned connection, or a
	//! pool connection inside its own server transaction. Appended to under
	//! write_mutex, which also covers the staging phase.
	mssql::BulkLoadSession shared;
	std::mutex write_mutex;

	//! Release targets and the pool, captured on the client thread when the
	//! stream opens (issue #178).
	tds::ConnectionPool *pool = nullptr;
	weak_ptr<tds::ConnectionPool> pool_handle;
	bool transaction_pinned = false;
	bool reset_on_release = tds::DEFAULT_RESET_CONNECTION;

	//! Writers this INSERT may open, the shared one included; resolved by
	//! MSSQLResolveLoadPolicy when the stream opens (1 inside a transaction).
	idx_t parallel_writer_limit = 1;
	std::atomic<idx_t> parallel_writers_used{1};

	std::atomic<idx_t> rows_sent{0};
	std::atomic<idx_t> rows_confirmed{0};
	std::atomic<idx_t> batches_flushed{0};

	//! Sessions the worker threads closed in Combine, kept -- connection and
	//! open transaction -- until Finalize commits all of them together. Local
	//! sink states are destroyed before Finalize, so they cannot hold them.
	std::mutex finished_mutex;
	vector<unique_ptr<mssql::BulkLoadSession>> finished_sessions;

	//! First failure wins: with N writers, one broken load fails them all, and
	//! the first message is the one that explains it.
	std::mutex error_mutex;
	string error_message;
	std::atomic<bool> has_error{false};
};

//===----------------------------------------------------------------------===//
// MSSQLInsertLocalSinkState - Per-thread state for INSERT operator
//===----------------------------------------------------------------------===//

class MSSQLInsertLocalSinkState : public LocalSinkState {
public:
	MSSQLInsertLocalSinkState() = default;

	//! This thread's own bulk-load session, if it won one; null while it
	//! shares the global session. A pointer so Combine can hand it to the
	//! global state whole.
	unique_ptr<mssql::BulkLoadSession> session;
	//! Whether to keep asking for an own writer on later chunks (spec 070 W2:
	//! the columnstore warm-up gate may say "not yet" before it says "no").
	bool may_claim = false;
	bool init_attempted = false;
};

}  // namespace duckdb
