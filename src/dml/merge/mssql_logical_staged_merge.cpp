#include "dml/merge/mssql_logical_staged_merge.hpp"

#include "catalog/mssql_catalog.hpp"
#include "dml/merge/mssql_physical_staged_merge.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb {

MSSQLLogicalStagedMerge::MSSQLLogicalStagedMerge(TableCatalogEntry &table_p, TableIndex table_index_p,
												 MSSQLStagedMergeTarget target_p,
												 vector<unique_ptr<Expression>> src_columns)
	: LogicalExtensionOperator(std::move(src_columns)),
	  table(table_p),
	  table_index(table_index_p),
	  target(std::move(target_p)) {}

InsertionOrderPreservingMap<string> MSSQLLogicalStagedMerge::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Table"] = target.schema_name + "." + target.table_name;
	result["Statement"] = target.merge_sql;
	return result;
}

vector<ColumnBinding> MSSQLLogicalStagedMerge::GetColumnBindings() {
	return {ColumnBinding(table_index, ProjectionIndex(0))};
}

vector<TableIndex> MSSQLLogicalStagedMerge::GetTableIndex() const {
	return {table_index};
}

void MSSQLLogicalStagedMerge::ResolveTypes() {
	types = {LogicalType::BIGINT};
}

void MSSQLLogicalStagedMerge::Serialize(Serializer &serializer) const {
	throw NotImplementedException("MSSQL: a staged MERGE plan cannot be serialized");
}

PhysicalOperator &MSSQLLogicalStagedMerge::CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) {
	auto &catalog = table.ParentCatalog().Cast<MSSQLCatalog>();
	// As for every write: a read-only ATTACH refuses here, at planning, with
	// the shipped path's message.
	catalog.CheckWriteAccess("MERGE");
	estimated_cardinality = children[0]->EstimateCardinality(context);
	auto &child = planner.CreatePlan(*children[0]);
	planner.dependencies.AddDependency(table);
	// ColumnBindingResolver has turned the references to the #src columns into
	// positions in the child's chunk.
	vector<idx_t> src_chunk_index;
	for (auto &expression : expressions) {
		if (expression->GetExpressionClass() != ExpressionClass::BOUND_REF) {
			throw InternalException("MSSQL staged MERGE: a #src column was not resolved to a chunk position");
		}
		src_chunk_index.push_back(expression->Cast<BoundReferenceExpression>().Index());
	}
	auto &op = planner.Make<MSSQLPhysicalStagedMerge>(types, estimated_cardinality, target, std::move(src_chunk_index));
	op.children.push_back(child);
	return op;
}

}  // namespace duckdb
