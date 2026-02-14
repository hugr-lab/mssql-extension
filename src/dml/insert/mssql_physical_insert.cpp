#include "dml/insert/mssql_physical_insert.hpp"
#include "dml/insert/mssql_insert_executor.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLInsertGlobalSinkState
//===----------------------------------------------------------------------===//

MSSQLInsertGlobalSinkState::MSSQLInsertGlobalSinkState(ClientContext &context, const MSSQLInsertTarget &target,
													   const MSSQLInsertConfig &config, bool return_chunk_flag)
	: total_rows_inserted(0), returned(false), return_chunk(return_chunk_flag), result_chunk_index(0) {
	executor = make_uniq<MSSQLInsertExecutor>(context, target, config);

	// Build returning column indices from target
	if (return_chunk) {
		returning_column_ids = target.returning_column_indices;
	}
}

//===----------------------------------------------------------------------===//
// MSSQLPhysicalInsert Constructor
//===----------------------------------------------------------------------===//

MSSQLPhysicalInsert::MSSQLPhysicalInsert(PhysicalPlan &plan, vector<LogicalType> types, idx_t estimated_cardinality,
										 MSSQLInsertTarget target, MSSQLInsertConfig config, bool return_chunk)
	: PhysicalOperator(plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
	  target_(std::move(target)),
	  config_(std::move(config)),
	  return_chunk_(return_chunk) {}

//===----------------------------------------------------------------------===//
// State Management
//===----------------------------------------------------------------------===//

unique_ptr<GlobalSinkState> MSSQLPhysicalInsert::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<MSSQLInsertGlobalSinkState>(context, target_, config_, return_chunk_);
}

unique_ptr<LocalSinkState> MSSQLPhysicalInsert::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<MSSQLInsertLocalSinkState>();
}

//===----------------------------------------------------------------------===//
// Sink Implementation
//===----------------------------------------------------------------------===//

SinkResultType MSSQLPhysicalInsert::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<MSSQLInsertGlobalSinkState>();

	// Thread-safe execution
	std::lock_guard<std::mutex> lock(gstate.mutex);

	if (gstate.return_chunk) {
		// RETURNING mode - use ExecuteWithReturning
		auto result = gstate.executor->ExecuteWithReturning(chunk, gstate.returning_column_ids);
		if (result && result->size() > 0) {
			gstate.total_rows_inserted += result->size();
			gstate.result_chunks.push_back(std::move(result));
		}
	} else {
		// Normal mode - just count rows
		idx_t rows_inserted = gstate.executor->Execute(chunk);
		gstate.total_rows_inserted += rows_inserted;
	}

	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType MSSQLPhysicalInsert::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	// Nothing to combine - we use a global executor with mutex
	return SinkCombineResultType::FINISHED;
}

SinkFinalizeType MSSQLPhysicalInsert::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
											   OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<MSSQLInsertGlobalSinkState>();

	// Thread-safe finalization
	std::lock_guard<std::mutex> lock(gstate.mutex);

	if (gstate.return_chunk) {
		// RETURNING mode - finalize with output
		auto result = gstate.executor->FinalizeWithReturning();
		if (result && result->size() > 0) {
			gstate.total_rows_inserted += result->size();
			gstate.result_chunks.push_back(std::move(result));
		}
	} else {
		// Normal mode - just finalize
		gstate.executor->Finalize();
	}

	return SinkFinalizeType::READY;
}

//===----------------------------------------------------------------------===//
// Source Implementation (for returning results)
//===----------------------------------------------------------------------===//

SourceResultType MSSQLPhysicalInsert::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
													  OperatorSourceInput &input) const {
	auto &gstate = sink_state->Cast<MSSQLInsertGlobalSinkState>();

	// Thread-safe access
	std::lock_guard<std::mutex> lock(gstate.mutex);

	if (gstate.return_chunk) {
		// RETURNING mode - return accumulated result chunks
		if (gstate.result_chunk_index >= gstate.result_chunks.size()) {
			return SourceResultType::FINISHED;
		}

		// Get the next result chunk
		auto &result = gstate.result_chunks[gstate.result_chunk_index];
		gstate.result_chunk_index++;

		// Reference the data from result chunk
		// This avoids copying and uses the already-parsed data directly
		chunk.Reference(*result);

		return gstate.result_chunk_index >= gstate.result_chunks.size() ? SourceResultType::FINISHED
																		: SourceResultType::HAVE_MORE_OUTPUT;
	} else {
		// Normal mode - return row count
		if (gstate.returned) {
			return SourceResultType::FINISHED;
		}

		gstate.returned = true;

		// Return the count
		chunk.SetCardinality(1);
		chunk.SetValue(0, 0, Value::BIGINT(static_cast<int64_t>(gstate.total_rows_inserted)));

		return SourceResultType::FINISHED;
	}
}

}  // namespace duckdb
