#include "dml/delete/mssql_physical_delete.hpp"
#include "dml/delete/mssql_delete_executor.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLPhysicalDelete Implementation
//===----------------------------------------------------------------------===//

MSSQLPhysicalDelete::MSSQLPhysicalDelete(PhysicalPlan &plan, vector<LogicalType> types, idx_t estimated_cardinality,
										 MSSQLDeleteTarget target, MSSQLDMLConfig config)
	: PhysicalOperator(plan, TYPE, std::move(types), estimated_cardinality),
	  target_(std::move(target)),
	  config_(std::move(config)) {}

//===----------------------------------------------------------------------===//
// Sink Interface
//===----------------------------------------------------------------------===//

SinkResultType MSSQLPhysicalDelete::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<MSSQLDeleteGlobalSinkState>();
	lock_guard<mutex> lock(gstate.mutex);

	// Process the chunk through the executor
	gstate.executor->Execute(chunk);

	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType MSSQLPhysicalDelete::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	// No local state to combine
	return SinkCombineResultType::FINISHED;
}

SinkFinalizeType MSSQLPhysicalDelete::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
											   OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<MSSQLDeleteGlobalSinkState>();
	lock_guard<mutex> lock(gstate.mutex);

	if (!gstate.finalized) {
		auto result = gstate.executor->Finalize();
		if (!result.success) {
			throw IOException("%s", result.FormatError("DELETE"));
		}
		gstate.total_rows_deleted = gstate.executor->GetTotalRowsDeleted();
		gstate.batch_count = gstate.executor->GetBatchCount();
		gstate.finalized = true;
	}

	return SinkFinalizeType::READY;
}

unique_ptr<GlobalSinkState> MSSQLPhysicalDelete::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<MSSQLDeleteGlobalSinkState>(context, target_, config_);
}

unique_ptr<LocalSinkState> MSSQLPhysicalDelete::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<MSSQLDeleteLocalSinkState>();
}

//===----------------------------------------------------------------------===//
// Source Interface
//===----------------------------------------------------------------------===//

SourceResultType MSSQLPhysicalDelete::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
													  OperatorSourceInput &input) const {
	auto &gstate = sink_state->Cast<MSSQLDeleteGlobalSinkState>();
	lock_guard<mutex> lock(gstate.mutex);

	if (gstate.returned) {
		return SourceResultType::FINISHED;
	}

	// Return the count of deleted rows
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(gstate.total_rows_deleted));
	gstate.returned = true;

	return SourceResultType::FINISHED;
}

//===----------------------------------------------------------------------===//
// MSSQLDeleteGlobalSinkState Implementation
//===----------------------------------------------------------------------===//

MSSQLDeleteGlobalSinkState::MSSQLDeleteGlobalSinkState(ClientContext &context, const MSSQLDeleteTarget &target,
													   const MSSQLDMLConfig &config) {
	executor = make_uniq<MSSQLDeleteExecutor>(context, target, config);
}

}  // namespace duckdb
