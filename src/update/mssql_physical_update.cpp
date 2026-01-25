#include "update/mssql_physical_update.hpp"
#include "update/mssql_update_executor.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLPhysicalUpdate Implementation
//===----------------------------------------------------------------------===//

MSSQLPhysicalUpdate::MSSQLPhysicalUpdate(PhysicalPlan &plan, vector<LogicalType> types, idx_t estimated_cardinality,
                                         MSSQLUpdateTarget target, MSSQLDMLConfig config)
    : PhysicalOperator(plan, TYPE, std::move(types), estimated_cardinality), target_(std::move(target)),
      config_(std::move(config)) {
}

//===----------------------------------------------------------------------===//
// Sink Interface
//===----------------------------------------------------------------------===//

SinkResultType MSSQLPhysicalUpdate::Sink(ExecutionContext &context, DataChunk &chunk,
                                         OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<MSSQLUpdateGlobalSinkState>();
	lock_guard<mutex> lock(gstate.mutex);

	// Process the chunk through the executor
	gstate.executor->Execute(chunk);

	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType MSSQLPhysicalUpdate::Combine(ExecutionContext &context,
                                                   OperatorSinkCombineInput &input) const {
	// No local state to combine
	return SinkCombineResultType::FINISHED;
}

SinkFinalizeType MSSQLPhysicalUpdate::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                               OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<MSSQLUpdateGlobalSinkState>();
	lock_guard<mutex> lock(gstate.mutex);

	if (!gstate.finalized) {
		auto result = gstate.executor->Finalize();
		if (!result.success) {
			throw IOException("%s", result.FormatError("UPDATE"));
		}
		gstate.total_rows_updated = gstate.executor->GetTotalRowsUpdated();
		gstate.batch_count = gstate.executor->GetBatchCount();
		gstate.finalized = true;
	}

	return SinkFinalizeType::READY;
}

unique_ptr<GlobalSinkState> MSSQLPhysicalUpdate::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<MSSQLUpdateGlobalSinkState>(context, target_, config_);
}

unique_ptr<LocalSinkState> MSSQLPhysicalUpdate::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<MSSQLUpdateLocalSinkState>();
}

//===----------------------------------------------------------------------===//
// Source Interface
//===----------------------------------------------------------------------===//

SourceResultType MSSQLPhysicalUpdate::MSSQL_GETDATA_METHOD(ExecutionContext &context, DataChunk &chunk,
                                                           OperatorSourceInput &input) const {
	auto &gstate = sink_state->Cast<MSSQLUpdateGlobalSinkState>();
	lock_guard<mutex> lock(gstate.mutex);

	if (gstate.returned) {
		return SourceResultType::FINISHED;
	}

	// Return the count of updated rows
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(gstate.total_rows_updated));
	gstate.returned = true;

	return SourceResultType::FINISHED;
}

//===----------------------------------------------------------------------===//
// MSSQLUpdateGlobalSinkState Implementation
//===----------------------------------------------------------------------===//

MSSQLUpdateGlobalSinkState::MSSQLUpdateGlobalSinkState(ClientContext &context, const MSSQLUpdateTarget &target,
                                                       const MSSQLDMLConfig &config) {
	executor = make_uniq<MSSQLUpdateExecutor>(context, target, config);
}

}  // namespace duckdb
