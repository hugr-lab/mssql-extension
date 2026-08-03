#pragma once

#include <atomic>
#include <memory>
#include <mutex>
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

	//! Last-resort release. A throw from Sink skips Combine, so nothing else
	//! would return this thread's connection: it would sit in Executing with a
	//! bulk-load transaction open, holding locks on the table CTAS just created,
	//! until the pool was torn down. Touches NO ClientContext — the pool handle
	//! is captured on the thread that acquired the connection (issue #178).
	~MSSQLCTASLocalSinkState();

	//! This thread's own bulk-load connection, or null when it shares the global
	//! writer (in a transaction, at the limit, or acquisition failed).
	std::shared_ptr<tds::TdsConnection> connection;
	unique_ptr<mssql::BCPWriter> writer;
	weak_ptr<tds::ConnectionPool> pool_handle;

	//! Rows this writer has sent since its last flush.
	idx_t rows_in_batch = 0;

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
