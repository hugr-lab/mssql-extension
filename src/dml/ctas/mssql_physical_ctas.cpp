#include "dml/ctas/mssql_physical_ctas.hpp"
#include "catalog/mssql_catalog.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLCTASGlobalSinkState
//===----------------------------------------------------------------------===//

MSSQLCTASGlobalSinkState::MSSQLCTASGlobalSinkState(ClientContext &context, MSSQLCatalog &catalog,
												   const mssql::CTASTarget &target,
												   const vector<mssql::CTASColumnDef> &columns,
												   const mssql::CTASConfig &config) {
	state.Initialize(catalog, target, columns, config);
}

//===----------------------------------------------------------------------===//
// MSSQLPhysicalCreateTableAs Constructor
//===----------------------------------------------------------------------===//

MSSQLPhysicalCreateTableAs::MSSQLPhysicalCreateTableAs(PhysicalPlan &plan, vector<LogicalType> types,
													   idx_t estimated_cardinality, MSSQLCatalog &catalog,
													   mssql::CTASTarget target, vector<mssql::CTASColumnDef> columns,
													   mssql::CTASConfig config)
	: PhysicalOperator(plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
	  catalog_(catalog),
	  target_(std::move(target)),
	  columns_(std::move(columns)),
	  config_(std::move(config)) {}

//===----------------------------------------------------------------------===//
// State Management
//===----------------------------------------------------------------------===//

unique_ptr<GlobalSinkState> MSSQLPhysicalCreateTableAs::GetGlobalSinkState(ClientContext &context) const {
	auto gstate = make_uniq<MSSQLCTASGlobalSinkState>(context, catalog_, target_, columns_, config_);

	// Execute DDL phase immediately (CREATE TABLE or DROP + CREATE for OR REPLACE)
	// This is done in GetGlobalSinkState to fail fast before any data is processed
	try {
		// Check if table exists and determine if this is a new table (Issue #45 - auto-TABLOCK)
		bool table_existed = gstate->state.TableExists(context);

		// Handle OR REPLACE: check if table exists and drop if needed
		if (gstate->state.target.or_replace) {
			if (table_existed) {
				gstate->state.ExecuteDrop(context);
				// After drop, this is effectively a new table
				gstate->state.config.is_new_table = true;
			} else {
				// Table didn't exist - definitely new
				gstate->state.config.is_new_table = true;
			}
		} else if (gstate->state.target.if_not_exists) {
			// Handle IF NOT EXISTS: skip if table exists (Issue #44)
			if (table_existed) {
				// Table exists - mark as skipped and return early
				gstate->state.phase = mssql::CTASPhase::SKIPPED;
				gstate->state.LogMetrics();
				return std::move(gstate);
			}
			// Table didn't exist - new table
			gstate->state.config.is_new_table = true;
		} else {
			// Non-OR REPLACE, non-IF NOT EXISTS: fail if table exists (FR-014)
			if (table_existed) {
				throw InvalidInputException(
					"CTAS failed: table '%s' already exists. "
					"Use CREATE OR REPLACE TABLE to overwrite.",
					gstate->state.target.GetQualifiedName());
			}
			// Table didn't exist - new table
			gstate->state.config.is_new_table = true;
		}

		// Validate schema exists (FR-009)
		if (!gstate->state.SchemaExists(context)) {
			throw InvalidInputException("CTAS failed: schema '%s' does not exist in SQL Server.",
										gstate->state.target.schema_name);
		}

		// Execute CREATE TABLE DDL
		gstate->state.ExecuteDDL(context);

	} catch (...) {
		// DDL phase failed - log and rethrow
		gstate->state.phase = mssql::CTASPhase::FAILED;
		gstate->state.LogMetrics();
		throw;
	}

	return std::move(gstate);
}

unique_ptr<LocalSinkState> MSSQLPhysicalCreateTableAs::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<MSSQLCTASLocalSinkState>();
}

//===----------------------------------------------------------------------===//
// Sink Implementation
//===----------------------------------------------------------------------===//

SinkResultType MSSQLPhysicalCreateTableAs::Sink(ExecutionContext &context, DataChunk &chunk,
												OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<MSSQLCTASGlobalSinkState>();

	// Thread-safe execution
	std::lock_guard<std::mutex> lock(gstate.mutex);

	// Skip if IF NOT EXISTS triggered skip (Issue #44)
	if (gstate.state.phase == mssql::CTASPhase::SKIPPED) {
		return SinkResultType::NEED_MORE_INPUT;
	}

	// Skip empty chunks
	if (chunk.size() == 0) {
		return SinkResultType::NEED_MORE_INPUT;
	}

	// Execute data transfer using BCP or INSERT mode (Spec 027)
	try {
		if (gstate.state.config.use_bcp && gstate.state.bcp_writer) {
			// BCP mode: delegate to AddChunkBCP
			gstate.state.AddChunkBCP(context.client, chunk);
		} else if (gstate.state.insert_executor) {
			// Legacy INSERT mode
			gstate.state.rows_produced += chunk.size();
			idx_t rows_inserted = gstate.state.insert_executor->Execute(chunk);
			gstate.state.rows_inserted += rows_inserted;
		}
	} catch (...) {
		// Data transfer phase failed - attempt cleanup if configured
		gstate.state.phase = mssql::CTASPhase::FAILED;
		gstate.state.error_message = gstate.state.config.use_bcp ? "BCP phase failed" : "Insert phase failed";

		if (gstate.state.config.drop_on_failure) {
			gstate.state.AttemptCleanup(context.client);
		}

		gstate.state.LogMetrics();
		throw;
	}

	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType MSSQLPhysicalCreateTableAs::Combine(ExecutionContext &context,
														  OperatorSinkCombineInput &input) const {
	// Nothing to combine - we use a global executor with mutex
	return SinkCombineResultType::FINISHED;
}

SinkFinalizeType MSSQLPhysicalCreateTableAs::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
													  OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<MSSQLCTASGlobalSinkState>();

	// Thread-safe finalization
	std::lock_guard<std::mutex> lock(gstate.mutex);

	// If skipped (IF NOT EXISTS and table existed), just return success (Issue #44)
	if (gstate.state.phase == mssql::CTASPhase::SKIPPED) {
		return SinkFinalizeType::READY;
	}

	try {
		// Flush any remaining INSERT batches
		gstate.state.FlushInserts(context);

		// Mark complete
		gstate.state.phase = mssql::CTASPhase::COMPLETE;

		// Invalidate catalog cache so the new table is visible
		gstate.state.InvalidateCache();

		// Log success metrics
		gstate.state.LogMetrics();

	} catch (...) {
		// Finalize failed - attempt cleanup if configured
		gstate.state.phase = mssql::CTASPhase::FAILED;
		gstate.state.error_message = "Finalize phase failed";

		if (gstate.state.config.drop_on_failure) {
			gstate.state.AttemptCleanup(context);
		}

		gstate.state.LogMetrics();
		throw;
	}

	return SinkFinalizeType::READY;
}

//===----------------------------------------------------------------------===//
// Source Implementation (for returning row count)
//===----------------------------------------------------------------------===//

SourceResultType MSSQLPhysicalCreateTableAs::MSSQL_GETDATA_METHOD(ExecutionContext &context, DataChunk &chunk,
																  OperatorSourceInput &input) const {
	auto &gstate = sink_state->Cast<MSSQLCTASGlobalSinkState>();

	// Thread-safe access
	std::lock_guard<std::mutex> lock(gstate.mutex);

	if (gstate.returned) {
		return SourceResultType::FINISHED;
	}

	gstate.returned = true;

	// Return the count of inserted rows
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(static_cast<int64_t>(gstate.state.rows_inserted)));

	return SourceResultType::FINISHED;
}

}  // namespace duckdb
