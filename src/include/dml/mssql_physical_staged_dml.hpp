//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// dml/mssql_physical_staged_dml.hpp
//
// Spec 080 D3: the sink of an UPDATE / DELETE delivered through `#stage`
// (dml/mssql_staged_dml.hpp). One operator for both verbs; it reports the
// statement's count like MSSQL_UPDATE / MSSQL_DELETE.
//===----------------------------------------------------------------------===//

#pragma once

#include <mutex>

#include "dml/mssql_staged_dml.hpp"
#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

class MSSQLPhysicalStagedDml : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

	MSSQLPhysicalStagedDml(PhysicalPlan &plan, vector<LogicalType> types, idx_t estimated_cardinality,
						   MSSQLStagedDmlTarget target);

	string GetName() const override {
		return target_.kind == MSSQLStagedDmlKind::UPDATE ? "MSSQL_STAGED_UPDATE" : "MSSQL_STAGED_DELETE";
	}

	bool IsSink() const override {
		return true;
	}
	bool IsSource() const override {
		return true;
	}
	OrderPreservationType SourceOrder() const override {
		return OrderPreservationType::NO_ORDER;
	}

	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
							  OperatorSinkFinalizeInput &input) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;

	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
									 OperatorSourceInput &input) const override;

private:
	MSSQLStagedDmlTarget target_;
};

class MSSQLStagedDmlGlobalSinkState : public GlobalSinkState {
public:
	MSSQLStagedDmlGlobalSinkState(ClientContext &context, const MSSQLStagedDmlTarget &target)
		: staged(context, target) {}

	MSSQLStagedDml staged;
	//! MSSQLStagedDmlTarget::hold_until_finalize: the rows, until Finalize.
	unique_ptr<ColumnDataCollection> held;
	idx_t rows = 0;
	bool returned = false;
	std::mutex mutex;
};

}  // namespace duckdb
