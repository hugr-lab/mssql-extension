#include "dml/ctas/mssql_ctas_planner.hpp"
#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_ddl_translator.hpp"
#include "dml/ctas/mssql_physical_ctas.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"

namespace duckdb {
namespace mssql {

//===----------------------------------------------------------------------===//
// CTASPlanner::Plan
//===----------------------------------------------------------------------===//

PhysicalOperator &CTASPlanner::Plan(ClientContext &context, PhysicalPlanGenerator &planner, MSSQLCatalog &catalog,
									LogicalCreateTable &op, PhysicalOperator &child_plan) {
	// Load CTAS configuration from settings
	CTASConfig config = CTASConfig::Load(context);

	// Extract target table information
	CTASTarget target = ExtractTarget(op, catalog);

	// Map columns from child plan to SQL Server types
	vector<CTASColumnDef> columns = MapColumns(op, child_plan, config);

	// Validate at least one column
	if (columns.empty()) {
		throw InvalidInputException("CTAS requires at least one column from the source query.");
	}

	// Result types: CTAS returns BIGINT row count
	vector<LogicalType> result_types;
	result_types.push_back(LogicalType::BIGINT);

	// Create the physical operator
	auto &physical_ctas =
		planner.Make<MSSQLPhysicalCreateTableAs>(std::move(result_types), op.estimated_cardinality, catalog,
												 std::move(target), std::move(columns), std::move(config));

	// Add child operator (the SELECT query)
	physical_ctas.children.push_back(child_plan);

	return physical_ctas;
}

//===----------------------------------------------------------------------===//
// CTASPlanner::ExtractTarget
//===----------------------------------------------------------------------===//

CTASTarget CTASPlanner::ExtractTarget(const LogicalCreateTable &op, MSSQLCatalog &catalog) {
	CTASTarget target;

	// Get catalog name from the MSSQL catalog
	target.catalog_name = catalog.GetName();

	// Extract schema and table from CreateTableInfo via BoundCreateTableInfo
	// The schema is in op.schema (SchemaCatalogEntry&)
	target.schema_name = op.schema.name;

	// Get table name and on_conflict from the base CreateTableInfo
	auto &base_info = op.info->Base();
	target.table_name = base_info.table;

	// Check for OR REPLACE (from on_conflict behavior)
	target.on_conflict = base_info.on_conflict;
	target.or_replace = (base_info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT);

	// Check for IF NOT EXISTS (Issue #44)
	target.if_not_exists = (base_info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT);

	return target;
}

//===----------------------------------------------------------------------===//
// CTASPlanner::MapColumns
//===----------------------------------------------------------------------===//

vector<CTASColumnDef> CTASPlanner::MapColumns(const LogicalCreateTable &op, PhysicalOperator &child_plan,
											  const CTASConfig &config) {
	vector<CTASColumnDef> columns;

	// Get the output types from the child plan
	const auto &child_types = child_plan.types;

	// Get base CreateTableInfo for column information
	auto &base_info = op.info->Base();

	// The columns in CreateTableInfo should match child_types count
	if (base_info.columns.LogicalColumnCount() != child_types.size()) {
		throw InternalException("CTAS column count mismatch: expected %llu, got %llu",
								(unsigned long long)child_types.size(),
								(unsigned long long)base_info.columns.LogicalColumnCount());
	}

	idx_t col_idx = 0;
	for (auto &col : base_info.columns.Logical()) {
		CTASColumnDef col_def;

		// Column name
		col_def.name = col.GetName();

		// DuckDB type from child plan
		col_def.duckdb_type = child_types[col_idx];

		// Map to SQL Server type using CTAS-specific mapper (FR-012, FR-013)
		try {
			col_def.mssql_type = MSSQLDDLTranslator::MapLogicalTypeToCTAS(child_types[col_idx], config);
		} catch (NotImplementedException &e) {
			// Enhance error message with column name (FR-012)
			throw NotImplementedException("CTAS failed for column '%s': %s", col_def.name, e.what());
		}

		// Nullability - assume nullable unless source is NOT NULL
		// For CTAS, we typically allow NULL for all columns since we can't
		// determine NOT NULL from the source query plan
		col_def.nullable = true;

		columns.push_back(std::move(col_def));
		col_idx++;
	}

	return columns;
}

}  // namespace mssql
}  // namespace duckdb
