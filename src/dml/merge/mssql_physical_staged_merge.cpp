#include "dml/merge/mssql_physical_staged_merge.hpp"

#include "connection/mssql_connection_provider.hpp"

namespace duckdb {

MSSQLPhysicalStagedMerge::MSSQLPhysicalStagedMerge(PhysicalPlan &plan, vector<LogicalType> types,
												   idx_t estimated_cardinality, MSSQLStagedMergeTarget target,
												   vector<idx_t> src_chunk_index)
	: PhysicalOperator(plan, TYPE, std::move(types), estimated_cardinality),
	  target_(std::move(target)),
	  src_chunk_index_(std::move(src_chunk_index)) {}

InsertionOrderPreservingMap<string> MSSQLPhysicalStagedMerge::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Table"] = target_.schema_name + "." + target_.table_name;
	result["Statement"] = target_.merge_sql;
	return result;
}

SinkResultType MSSQLPhysicalStagedMerge::Sink(ExecutionContext &context, DataChunk &chunk,
											  OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<MSSQLStagedMergeGlobalSinkState>();
	std::lock_guard<std::mutex> lock(gstate.mutex);
	vector<LogicalType> types;
	for (auto index : src_chunk_index_) {
		types.push_back(chunk.data[index].GetType());
	}
	gstate.fill.Destroy();
	gstate.fill.InitializeEmpty(types);
	for (idx_t i = 0; i < src_chunk_index_.size(); i++) {
		gstate.fill.data[i].Reference(chunk.data[src_chunk_index_[i]]);
	}
	gstate.fill.SetCardinalityUnsafe(chunk.size());
	gstate.staged.Execute(context.client, gstate.fill);
	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType MSSQLPhysicalStagedMerge::Combine(ExecutionContext &context,
														OperatorSinkCombineInput &input) const {
	return SinkCombineResultType::FINISHED;
}

SinkFinalizeType MSSQLPhysicalStagedMerge::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
													OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<MSSQLStagedMergeGlobalSinkState>();
	std::lock_guard<std::mutex> lock(gstate.mutex);
	gstate.rows = gstate.staged.Finalize(context);
	return SinkFinalizeType::READY;
}

unique_ptr<GlobalSinkState> MSSQLPhysicalStagedMerge::GetGlobalSinkState(ClientContext &context) const {
	// Issue #451: the statement runs on the catalog as a transaction of one --
	// the MERGE commits and rolls back with DuckDB's statement.
	ConnectionProvider::JoinStatementTransaction(context, target_.catalog_name);
	return make_uniq<MSSQLStagedMergeGlobalSinkState>(context, target_);
}

SourceResultType MSSQLPhysicalStagedMerge::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
														   OperatorSourceInput &input) const {
	auto &gstate = sink_state->Cast<MSSQLStagedMergeGlobalSinkState>();
	std::lock_guard<std::mutex> lock(gstate.mutex);
	if (gstate.returned_count) {
		return SourceResultType::FINISHED;
	}
	chunk.SetChildCardinality(1);
	chunk.data[0].SetValue(0, Value::BIGINT(NumericCast<int64_t>(gstate.rows)));
	gstate.returned_count = true;
	return SourceResultType::FINISHED;
}

}  // namespace duckdb
