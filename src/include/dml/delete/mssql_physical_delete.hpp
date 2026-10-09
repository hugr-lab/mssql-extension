//===----------------------------------------------------------------------===//
//                         DuckDB
//
// mssql_physical_delete.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "dml/delete/mssql_delete_target.hpp"
#include "dml/mssql_dml_config.hpp"
#include "dml/mssql_staged_dml.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

class MSSQLDeleteExecutor;

//! MSSQLPhysicalDelete is the physical operator for DELETE operations on MSSQL tables
//! Implements the Sink pattern to receive rows from DuckDB's DELETE planning
class MSSQLPhysicalDelete : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

	//! Constructor
	//! @param plan The physical plan
	//! @param types Return types (just row count)
	//! @param estimated_cardinality Estimated number of rows to delete
	//! @param target Target table metadata
	//! @param config DML configuration
	MSSQLPhysicalDelete(PhysicalPlan &plan, vector<LogicalType> types, idx_t estimated_cardinality,
						MSSQLDeleteTarget target, MSSQLDMLConfig config);

	//! Spec 080 W3: the entry whose row estimate an autocommit DELETE adjusts.
	void SetTableEntry(MSSQLTableEntry &entry) {
		table_entry_ = &entry;
	}

	//! Spec 080 D3: stage past mssql_dml_stage_threshold rows.
	void SetStagedTarget(MSSQLStagedDmlTarget target) {
		staged_target_ = make_uniq<MSSQLStagedDmlTarget>(std::move(target));
	}

	//! Get the name of this operator
	string GetName() const override {
		return "MSSQL_DELETE";
	}

	//! This is a sink operator
	bool IsSink() const override {
		return true;
	}

	//! Parallelism is not supported for DELETE
	bool ParallelSink() const override {
		return false;
	}

	//! Sink interface - receive rows to delete
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;

	//! Combine local states (no-op for non-parallel)
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;

	//! Finalize - flush remaining batches
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
							  OperatorSinkFinalizeInput &input) const override;

	//! Get global sink state
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;

	//! Get local sink state
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;

	//! This operator is also a source (returns row count)
	bool IsSource() const override {
		return true;
	}

	//! Source interface - return the row count
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
									 OperatorSourceInput &input) const override;

private:
	//! Target table metadata
	MSSQLDeleteTarget target_;
	//! Spec 080 D3, rungs 1-2: the #stage form of this statement, taken past
	//! mssql_dml_stage_threshold rows; null where the platform has no stage.
	unique_ptr<MSSQLStagedDmlTarget> staged_target_;
	MSSQLTableEntry *table_entry_ = nullptr;

	//! DML configuration
	MSSQLDMLConfig config_;
};

//! Global sink state for DELETE operations
class MSSQLDeleteGlobalSinkState : public GlobalSinkState {
public:
	explicit MSSQLDeleteGlobalSinkState(ClientContext &context, const MSSQLDeleteTarget &target,
										const MSSQLDMLConfig &config);

	//! Spec 080 D3: holds the rows until the threshold decides; null when the
	//! statement cannot stage (a MERGE action, a platform with no stage).
	unique_ptr<MSSQLStageSwitch> stage_switch;

	//! The executor that handles batch accumulation and execution
	unique_ptr<MSSQLDeleteExecutor> executor;

	//! Total rows deleted
	idx_t total_rows_deleted = 0;

	//! Number of batches executed
	idx_t batch_count = 0;

	//! Mutex for thread safety
	std::mutex mutex;

	//! Whether finalization has been done
	bool finalized = false;

	//! Whether result has been returned
	bool returned = false;
};

//! Local sink state (empty for non-parallel DELETE)
class MSSQLDeleteLocalSinkState : public LocalSinkState {
public:
	// No local state needed for non-parallel execution
};

}  // namespace duckdb
