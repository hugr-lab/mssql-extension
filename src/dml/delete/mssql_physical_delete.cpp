#include "dml/delete/mssql_physical_delete.hpp"
#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_table_entry.hpp"
#include "connection/mssql_connection_provider.hpp"
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

	if (gstate.stage_switch) {
		gstate.stage_switch->Sink(context.client, chunk);
	} else {
		gstate.executor->Execute(chunk);
	}

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

	if (!gstate.finalized && gstate.stage_switch && gstate.stage_switch->IsStaged()) {
		gstate.total_rows_deleted = gstate.stage_switch->FinalizeStaged(context);
		gstate.finalized = true;
	}
	if (!gstate.finalized) {
		if (gstate.stage_switch) {
			gstate.stage_switch->Replay([&](DataChunk &held) { gstate.executor->Execute(held); });
		}
		auto result = gstate.executor->Finalize();
		if (!result.success) {
			throw IOException("%s", result.FormatError("DELETE"));
		}
		gstate.total_rows_deleted = gstate.executor->GetTotalRowsDeleted();
		gstate.batch_count = gstate.executor->GetBatchCount();
		gstate.finalized = true;
		// Spec 080 W3: the planner's estimate follows an autocommit DELETE (the
		// staged path does the same in MSSQLStagedDml::Finalize).
		if (table_entry_ && !ConnectionProvider::IsInTransaction(context, table_entry_->GetMSSQLCatalog())) {
			table_entry_->NoteRowsDeleted(gstate.total_rows_deleted);
		}
	}

	return SinkFinalizeType::READY;
}

unique_ptr<GlobalSinkState> MSSQLPhysicalDelete::GetGlobalSinkState(ClientContext &context) const {
	auto gstate = make_uniq<MSSQLDeleteGlobalSinkState>(context, target_, config_);
	if (staged_target_ && !config_.defer_to_finalize) {
		gstate->stage_switch = make_uniq<MSSQLStageSwitch>(*staged_target_, config_.stage_threshold);
	}
	return std::move(gstate);
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
	chunk.SetChildCardinality(1);
	chunk.data[0].SetValue(0, Value::BIGINT(gstate.total_rows_deleted));
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
