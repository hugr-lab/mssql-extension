#include "dml/mssql_physical_staged_dml.hpp"

namespace duckdb {

MSSQLPhysicalStagedDml::MSSQLPhysicalStagedDml(PhysicalPlan &plan, vector<LogicalType> types,
											   idx_t estimated_cardinality, MSSQLStagedDmlTarget target)
	: PhysicalOperator(plan, TYPE, std::move(types), estimated_cardinality), target_(std::move(target)) {}

SinkResultType MSSQLPhysicalStagedDml::Sink(ExecutionContext &context, DataChunk &chunk,
											OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<MSSQLStagedDmlGlobalSinkState>();
	std::lock_guard<std::mutex> lock(gstate.mutex);
	if (target_.hold_until_finalize) {
		if (!gstate.held) {
			gstate.held = make_uniq<ColumnDataCollection>(context.client, chunk.GetTypes());
		}
		gstate.held->Append(chunk);
		return SinkResultType::NEED_MORE_INPUT;
	}
	gstate.staged.Execute(context.client, chunk);
	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType MSSQLPhysicalStagedDml::Combine(ExecutionContext &context,
													  OperatorSinkCombineInput &input) const {
	return SinkCombineResultType::FINISHED;
}

SinkFinalizeType MSSQLPhysicalStagedDml::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
												  OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<MSSQLStagedDmlGlobalSinkState>();
	std::lock_guard<std::mutex> lock(gstate.mutex);
	if (gstate.held) {
		for (auto &held_chunk : gstate.held->Chunks()) {
			gstate.staged.Execute(context, held_chunk);
		}
		gstate.held.reset();
	}
	gstate.rows = gstate.staged.Finalize(context);
	return SinkFinalizeType::READY;
}

unique_ptr<GlobalSinkState> MSSQLPhysicalStagedDml::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<MSSQLStagedDmlGlobalSinkState>(context, target_);
}

SourceResultType MSSQLPhysicalStagedDml::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
														 OperatorSourceInput &input) const {
	auto &gstate = sink_state->Cast<MSSQLStagedDmlGlobalSinkState>();
	std::lock_guard<std::mutex> lock(gstate.mutex);
	if (gstate.returned) {
		return SourceResultType::FINISHED;
	}
	chunk.SetChildCardinality(1);
	chunk.data[0].SetValue(0, Value::BIGINT(NumericCast<int64_t>(gstate.rows)));
	gstate.returned = true;
	return SourceResultType::FINISHED;
}

}  // namespace duckdb
