//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// dml/merge/mssql_physical_staged_merge.hpp
//
// Spec 080 D4 (PR 4b): the sink of a staged MERGE (dml/merge/
// mssql_staged_merge.hpp). Its child computes the #src columns; it stages them
// and, at Finalize, runs the one MERGE statement. Reports the MERGE's count.
//===----------------------------------------------------------------------===//

#pragma once

#include <mutex>

#include "dml/merge/mssql_staged_merge.hpp"
#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

class MSSQLPhysicalStagedMerge : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

	//! `src_chunk_index`: where each #src column sits in the child's chunk.
	MSSQLPhysicalStagedMerge(PhysicalPlan &plan, vector<LogicalType> types, idx_t estimated_cardinality,
							 MSSQLStagedMergeTarget target, vector<idx_t> src_chunk_index);

	string GetName() const override {
		return "MSSQL_STAGED_MERGE";
	}
	InsertionOrderPreservingMap<string> ParamsToString() const override;

	bool IsSink() const override {
		return true;
	}
	bool IsSource() const override {
		return true;
	}
	OrderPreservationType SourceOrder() const override {
		return OrderPreservationType::NO_ORDER;
	}

	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
							  OperatorSinkFinalizeInput &input) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;

	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
									 OperatorSourceInput &input) const override;

private:
	MSSQLStagedMergeTarget target_;
	vector<idx_t> src_chunk_index_;
};

class MSSQLStagedMergeGlobalSinkState : public GlobalSinkState {
public:
	MSSQLStagedMergeGlobalSinkState(ClientContext &context, const MSSQLStagedMergeTarget &target)
		: staged(context, target) {}

	MSSQLStagedMerge staged;
	//! The #src columns of the chunk being staged, as references.
	DataChunk fill;
	idx_t rows = 0;
	bool returned_count = false;
	std::mutex mutex;
};

}  // namespace duckdb
