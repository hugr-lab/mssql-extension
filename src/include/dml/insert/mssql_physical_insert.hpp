#pragma once

#include <memory>
#include <mutex>
#include "dml/insert/mssql_insert_config.hpp"
#include "dml/insert/mssql_insert_error.hpp"
#include "dml/insert/mssql_insert_target.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

// Forward declaration
class MSSQLInsertExecutor;

//===----------------------------------------------------------------------===//
// MSSQLPhysicalInsert - Physical operator for INSERT into SQL Server
//
// This operator receives rows from its child operator and inserts them
// into the target SQL Server table using batched SQL statements.
//
// Modes:
// - Without RETURNING: Returns count of inserted rows
// - With RETURNING: Returns the inserted rows with OUTPUT INSERTED values
//===----------------------------------------------------------------------===//

class MSSQLPhysicalInsert : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

	// Constructor - must take PhysicalPlan& as first argument for planner.Make<T>()
	// @param plan Physical plan reference (passed by planner.Make)
	// @param types Result types (count or returning columns)
	// @param estimated_cardinality Expected row count
	// @param target Insert target metadata
	// @param config Insert configuration
	// @param return_chunk Whether to return inserted data (RETURNING mode)
	MSSQLPhysicalInsert(PhysicalPlan &plan, vector<LogicalType> types, idx_t estimated_cardinality,
						MSSQLInsertTarget target, MSSQLInsertConfig config, bool return_chunk);

	//===----------------------------------------------------------------------===//
	// Target Information
	//===----------------------------------------------------------------------===//

	// Get insert target
	const MSSQLInsertTarget &GetTarget() const {
		return target_;
	}

	// Get insert configuration
	const MSSQLInsertConfig &GetConfig() const {
		return config_;
	}

	// Check if in RETURNING mode
	bool ReturnsChunk() const {
		return return_chunk_;
	}

public:
	//===----------------------------------------------------------------------===//
	// PhysicalOperator Interface
	//===----------------------------------------------------------------------===//

	// Get operator string for EXPLAIN
	string GetName() const override {
		return "MSSQL_INSERT";
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
	MSSQLInsertTarget target_;
	MSSQLInsertConfig config_;
	bool return_chunk_;
};

//===----------------------------------------------------------------------===//
// MSSQLInsertGlobalSinkState - Global state for INSERT operator
//===----------------------------------------------------------------------===//

class MSSQLInsertGlobalSinkState : public GlobalSinkState {
public:
	explicit MSSQLInsertGlobalSinkState(ClientContext &context, const MSSQLInsertTarget &target,
										const MSSQLInsertConfig &config, bool return_chunk);

	// The insert executor
	unique_ptr<MSSQLInsertExecutor> executor;

	// Total rows inserted
	idx_t total_rows_inserted;

	// Has data been returned (for source)
	bool returned;

	// RETURNING mode flag
	bool return_chunk;

	// Returning column indices
	vector<idx_t> returning_column_ids;

	// Accumulated result chunks for RETURNING mode
	vector<unique_ptr<DataChunk>> result_chunks;

	// Current index when returning chunks
	idx_t result_chunk_index;

	// Mutex for thread-safe access
	mutable std::mutex mutex;
};

//===----------------------------------------------------------------------===//
// MSSQLInsertLocalSinkState - Per-thread state for INSERT operator
//===----------------------------------------------------------------------===//

class MSSQLInsertLocalSinkState : public LocalSinkState {
public:
	MSSQLInsertLocalSinkState() = default;
};

}  // namespace duckdb
