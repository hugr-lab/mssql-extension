#pragma once

#include <memory>
#include <mutex>
#include "dml/ctas/mssql_ctas_config.hpp"
#include "dml/ctas/mssql_ctas_executor.hpp"
#include "dml/ctas/mssql_ctas_types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

// Forward declarations
class MSSQLCatalog;

//===----------------------------------------------------------------------===//
// MSSQLPhysicalCreateTableAs - Physical operator for CTAS into SQL Server
//
// This operator executes CTAS in two phases:
// 1. DDL Phase: CREATE TABLE (or DROP + CREATE for OR REPLACE)
// 2. DML Phase: Batched INSERT of query results
//
// Uses sink pattern to receive rows from child query and stream-insert
// them into the newly created SQL Server table.
//===----------------------------------------------------------------------===//

class MSSQLPhysicalCreateTableAs : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

	// Constructor
	// @param plan Physical plan reference (passed by planner.Make)
	// @param types Result types (BIGINT count)
	// @param estimated_cardinality Expected row count
	// @param catalog Reference to MSSQL catalog
	// @param target Target table metadata
	// @param columns Column definitions with mapped types
	// @param config CTAS configuration
	MSSQLPhysicalCreateTableAs(PhysicalPlan &plan, vector<LogicalType> types, idx_t estimated_cardinality,
							   MSSQLCatalog &catalog, mssql::CTASTarget target, vector<mssql::CTASColumnDef> columns,
							   mssql::CTASConfig config);

	//===----------------------------------------------------------------------===//
	// Target Information
	//===----------------------------------------------------------------------===//

	const mssql::CTASTarget &GetTarget() const {
		return target_;
	}

	const vector<mssql::CTASColumnDef> &GetColumns() const {
		return columns_;
	}

	const mssql::CTASConfig &GetConfig() const {
		return config_;
	}

	MSSQLCatalog &GetCatalog() const {
		return catalog_;
	}

public:
	//===----------------------------------------------------------------------===//
	// PhysicalOperator Interface
	//===----------------------------------------------------------------------===//

	string GetName() const override {
		return "MSSQL_CREATE_TABLE_AS";
	}

	bool IsSink() const override {
		return true;
	}

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
	// Source Interface (for returning row count)
	//===----------------------------------------------------------------------===//

	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
										  OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}

private:
	MSSQLCatalog &catalog_;
	mssql::CTASTarget target_;
	vector<mssql::CTASColumnDef> columns_;
	mssql::CTASConfig config_;
};

//===----------------------------------------------------------------------===//
// MSSQLCTASGlobalSinkState - Global state for CTAS operator
//===----------------------------------------------------------------------===//

class MSSQLCTASGlobalSinkState : public GlobalSinkState {
public:
	explicit MSSQLCTASGlobalSinkState(ClientContext &context, MSSQLCatalog &catalog, const mssql::CTASTarget &target,
									  const vector<mssql::CTASColumnDef> &columns, const mssql::CTASConfig &config);

	// The CTAS execution state
	mssql::CTASExecutionState state;

	// Has row count been returned
	bool returned = false;

	// Mutex for thread-safe access
	mutable std::mutex mutex;
};

//===----------------------------------------------------------------------===//
// MSSQLCTASLocalSinkState - Per-thread state (empty for CTAS)
//===----------------------------------------------------------------------===//

class MSSQLCTASLocalSinkState : public LocalSinkState {
public:
	MSSQLCTASLocalSinkState() = default;
};

}  // namespace duckdb
