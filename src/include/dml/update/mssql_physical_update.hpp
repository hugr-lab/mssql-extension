#pragma once

#include <memory>
#include <mutex>
#include "dml/mssql_dml_config.hpp"
#include "dml/update/mssql_update_target.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

// Forward declaration
class MSSQLUpdateExecutor;

//===----------------------------------------------------------------------===//
// MSSQLPhysicalUpdate - Physical operator for UPDATE on SQL Server
//
// This operator receives rows from its child operator (containing rowid and
// new values) and updates them in the target SQL Server table using batched
// SQL statements.
//
// Input chunk format: [rowid, update_col1, update_col2, ...]
// Output: Row count (BIGINT)
//===----------------------------------------------------------------------===//

class MSSQLPhysicalUpdate : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

	// Constructor - must take PhysicalPlan& as first argument for planner.Make<T>()
	// @param plan Physical plan reference (passed by planner.Make)
	// @param types Result types (count column)
	// @param estimated_cardinality Expected row count
	// @param target Update target metadata
	// @param config DML configuration
	MSSQLPhysicalUpdate(PhysicalPlan &plan, vector<LogicalType> types, idx_t estimated_cardinality,
						MSSQLUpdateTarget target, MSSQLDMLConfig config);

	//===----------------------------------------------------------------------===//
	// Target Information
	//===----------------------------------------------------------------------===//

	// Get update target
	const MSSQLUpdateTarget &GetTarget() const {
		return target_;
	}

	// Get DML configuration
	const MSSQLDMLConfig &GetConfig() const {
		return config_;
	}

public:
	//===----------------------------------------------------------------------===//
	// PhysicalOperator Interface
	//===----------------------------------------------------------------------===//

	// Get operator string for EXPLAIN
	string GetName() const override {
		return "MSSQL_UPDATE";
	}

	// Check if this is a sink operator
	bool IsSink() const override {
		return true;
	}

	// Order preservation type
	OrderPreservationType SourceOrder() const override {
		return OrderPreservationType::NO_ORDER;
	}

	//===----------------------------------------------------------------------===//
	// Sink Interface
	//===----------------------------------------------------------------------===//

	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;

	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;

	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
							  OperatorSinkFinalizeInput &input) const override;

	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;

	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;

	//===----------------------------------------------------------------------===//
	// Source Interface (for returning results)
	//===----------------------------------------------------------------------===//

	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
										  OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}

private:
	MSSQLUpdateTarget target_;
	MSSQLDMLConfig config_;
};

//===----------------------------------------------------------------------===//
// MSSQLUpdateGlobalSinkState - Global state for UPDATE operator
//===----------------------------------------------------------------------===//

class MSSQLUpdateGlobalSinkState : public GlobalSinkState {
public:
	explicit MSSQLUpdateGlobalSinkState(ClientContext &context, const MSSQLUpdateTarget &target,
										const MSSQLDMLConfig &config);

	// The update executor
	unique_ptr<MSSQLUpdateExecutor> executor;

	// Total rows updated
	idx_t total_rows_updated = 0;

	// Number of batches executed
	idx_t batch_count = 0;

	// Has Finalize() been called?
	bool finalized = false;

	// Has GetData() returned result?
	bool returned = false;

	// Mutex for thread-safe access
	mutable std::mutex mutex;
};

//===----------------------------------------------------------------------===//
// MSSQLUpdateLocalSinkState - Per-thread state for UPDATE operator
//===----------------------------------------------------------------------===//

class MSSQLUpdateLocalSinkState : public LocalSinkState {
public:
	MSSQLUpdateLocalSinkState() = default;
};

}  // namespace duckdb
