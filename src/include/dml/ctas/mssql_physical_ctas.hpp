#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include "copy/bulk_load_session.hpp"
#include "dml/ctas/mssql_ctas_config.hpp"
#include "dml/ctas/mssql_ctas_executor.hpp"
#include "dml/ctas/mssql_ctas_types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

// Forward declarations
class MSSQLCatalog;

//===----------------------------------------------------------------------===//
// MSSQLPhysicalCreateTableAs - Physical operator for CTAS into SQL Server
//
// This operator executes CTAS in two phases:
// 1. DDL Phase: CREATE TABLE (or DROP + CREATE for OR REPLACE)
// 2. DML Phase: Batched INSERT of query results
//
// Uses sink pattern to receive rows from child query and stream-insert
// them into the newly created SQL Server table.
//===----------------------------------------------------------------------===//

class MSSQLPhysicalCreateTableAs : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

	// Constructor
	// @param plan Physical plan reference (passed by planner.Make)
	// @param types Result types (BIGINT count)
	// @param estimated_cardinality Expected row count
	// @param catalog Reference to MSSQL catalog
	// @param target Target table metadata
	// @param columns Column definitions with mapped types
	// @param config CTAS configuration
	MSSQLPhysicalCreateTableAs(PhysicalPlan &plan, vector<LogicalType> types, idx_t estimated_cardinality,
							   MSSQLCatalog &catalog, mssql::CTASTarget target, vector<mssql::CTASColumnDef> columns,
							   mssql::CTASConfig config);

	//===----------------------------------------------------------------------===//
	// Target Information
	//===----------------------------------------------------------------------===//

	const mssql::CTASTarget &GetTarget() const {
		return target_;
	}

	const vector<mssql::CTASColumnDef> &GetColumns() const {
		return columns_;
	}

	const mssql::CTASConfig &GetConfig() const {
		return config_;
	}

	MSSQLCatalog &GetCatalog() const {
		return catalog_;
	}

public:
	//===----------------------------------------------------------------------===//
	// PhysicalOperator Interface
	//===----------------------------------------------------------------------===//

	string GetName() const override {
		return "MSSQL_CREATE_TABLE_AS";
	}

	bool IsSink() const override {
		return true;
	}

	OrderPreservationType SourceOrder() const override {
		return OrderPreservationType::NO_ORDER;
	}

	//===----------------------------------------------------------------------===//
	// Sink Interface
	//===----------------------------------------------------------------------===//

	//! Spec 057 step 7: CTAS loads through the same INSERT BULK path COPY does,
	//! and the bound there is SQL Server's ingest rate, which it parallelises
	//! across SESSIONS. This was false — the default — so DuckDB drove the sink
	//! from one thread and the mutex below was never contended.
	bool ParallelSink() const override {
		return true;
	}

	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;

	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;

	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
							  OperatorSinkFinalizeInput &input) const override;

	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;

	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;

	//===----------------------------------------------------------------------===//
	// Source Interface (for returning row count)
	//===----------------------------------------------------------------------===//

	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
									 OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}

private:
	MSSQLCatalog &catalog_;
	mssql::CTASTarget target_;
	vector<mssql::CTASColumnDef> columns_;
	mssql::CTASConfig config_;
};

//===----------------------------------------------------------------------===//
// MSSQLCTASGlobalSinkState - Global state for CTAS operator
//===----------------------------------------------------------------------===//

class MSSQLCTASGlobalSinkState : public GlobalSinkState {
public:
	explicit MSSQLCTASGlobalSinkState(ClientContext &context, MSSQLCatalog &catalog, const mssql::CTASTarget &target,
									  const vector<mssql::CTASColumnDef> &columns, const mssql::CTASConfig &config);

	// The CTAS execution state
	mssql::CTASExecutionState state;

	// Has row count been returned
	bool returned = false;

	// Mutex for thread-safe access
	mutable std::mutex mutex;

	//===------------------------------------------------------------------===//
	// Parallel writers (spec 057 step 7, CTAS)
	//===------------------------------------------------------------------===//

	//! Upper bound on bulk-load sessions, the one opened by ExecuteBCPInsert
	//! included. 1 disables the feature. Resolved once in GetGlobalSinkState.
	idx_t parallel_writer_limit = 1;

	//! Sessions handed out so far, that first one included.
	std::atomic<idx_t> parallel_writers_used{1};

	//! IF NOT EXISTS found the table already there, so no row is to be sent.
	//!
	//! Decided in GetGlobalSinkState, before any Sink call, and never changed
	//! after — which is the whole point of it being here rather than read off
	//! `state.phase`. Sink checks this on every chunk on every thread, and phase
	//! is written by a FAILING thread under `mutex`: reading it there unlocked
	//! was a data race from the moment the sink became parallel.
	bool skipped = false;

	//! Set by the first thread whose load fails, so exactly one of them runs the
	//! DROP that mssql_ctas_drop_on_failure asks for. Separate from `phase`
	//! because it is the one piece of failure state read outside `mutex`.
	std::atomic<bool> load_failed{false};

	//===------------------------------------------------------------------===//
	// MSSQL_COUNTERS (spec 063 D5)
	//
	// CTAS reported NOTHING: `counter_*` appeared zero times in this operator
	// against twenty in the COPY sink, so `MSSQL_COUNTERS=1` measured one of the
	// two write paths and a CTAS could not be compared with a COPY at all.
	//
	// Totals cover both routes — a thread with its own session reports through
	// BulkLoadWriteResult, one sharing the global writer through
	// CTASExecutionState's own phase counters.
	//===------------------------------------------------------------------===//

	std::atomic<idx_t> counter_sink_calls{0};
	std::atomic<uint64_t> counter_sink_ns{0};
	std::atomic<uint64_t> counter_encode_ns{0};
	std::atomic<uint64_t> counter_flush_ns{0};
};

//===----------------------------------------------------------------------===//
// MSSQLCTASLocalSinkState - Per-thread state
//
// A thread either owns a bulk-load session of its own — its own connection, its
// own INSERT BULK, its own writer, no lock — or it has none and appends to the
// global writer under the global mutex, which is what every thread did before.
//===----------------------------------------------------------------------===//

class MSSQLCTASLocalSinkState : public LocalSinkState {
public:
	MSSQLCTASLocalSinkState() = default;

	//! This thread's own bulk-load session, or unopened when it shares the global
	//! writer (at the limit, or acquisition failed). Owns the connection, the
	//! writer, the batch bookkeeping and the last-resort release — see
	//! copy/bulk_load_session.hpp; COPY holds the same type.
	mssql::BulkLoadSession session;

	//! Rows this writer has encoded in total, folded into the global
	//! rows_produced in Combine so the metrics line still adds up.
	idx_t rows_written = 0;

	//! Rows the server has confirmed to this writer, folded into the global
	//! count in Combine.
	idx_t rows_confirmed = 0;

	//! Acquisition is tried ONCE, on the first chunk. Failure is not an error:
	//! the thread falls back to the shared writer, which is always correct.
	bool init_attempted = false;
};

}  // namespace duckdb
