//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// dml/merge/mssql_logical_staged_merge.hpp
//
// Spec 080 D4 (PR 4b): what a LogicalMergeInto into a catalog table becomes
// before DuckDB's optimizers run (MSSQLOptimizer::PreOptimizeRoot, through
// mssql::RewriteMerge). Its child is a projection over the MERGE's source
// that computes the #src columns; its expressions are references to them, one
// per #src column, so they are resolved to chunk positions and nothing prunes
// them. The match, the conditions and the writes are the T-SQL statement in
// its target. One BIGINT column, the count.
//===----------------------------------------------------------------------===//

#pragma once

#include "dml/merge/mssql_staged_merge.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"

namespace duckdb {

class TableCatalogEntry;

class MSSQLLogicalStagedMerge : public LogicalExtensionOperator {
public:
	MSSQLLogicalStagedMerge(TableCatalogEntry &table, TableIndex table_index, MSSQLStagedMergeTarget target,
							vector<unique_ptr<Expression>> src_columns);

	//! The table merged into.
	TableCatalogEntry &table;
	//! The count's binding (the LogicalMergeInto's own).
	TableIndex table_index;
	MSSQLStagedMergeTarget target;

	string GetName() const override {
		return "MSSQL_STAGED_MERGE";
	}
	string GetExtensionName() const override {
		return "mssql_staged_merge";
	}
	InsertionOrderPreservingMap<string> ParamsToString() const override;

	vector<ColumnBinding> GetColumnBindings() override;
	vector<TableIndex> GetTableIndex() const override;

	//! The plan is never serialised: a write is planned where it runs.
	void Serialize(Serializer &serializer) const override;

	PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) override;

protected:
	void ResolveTypes() override;
};

}  // namespace duckdb
