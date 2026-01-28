#pragma once

#include "dml/ctas/mssql_ctas_config.hpp"
#include "dml/ctas/mssql_ctas_types.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"

namespace duckdb {

// Forward declarations
class MSSQLCatalog;
class PhysicalPlanGenerator;
class PhysicalOperator;

//===----------------------------------------------------------------------===//
// CTASPlanner - Plans CTAS operations for SQL Server
//
// Extracts target information from LogicalCreateTable, maps DuckDB types
// to SQL Server types, and creates the physical CTAS operator.
//===----------------------------------------------------------------------===//

namespace mssql {

class CTASPlanner {
public:
	//! Plan a CTAS operation
	//! @param context Client context for settings
	//! @param planner Physical plan generator
	//! @param catalog MSSQL catalog reference
	//! @param op Logical create table operator
	//! @param child_plan Child operator providing data
	//! @return Physical CTAS operator
	static PhysicalOperator &Plan(ClientContext &context, PhysicalPlanGenerator &planner, MSSQLCatalog &catalog,
								  LogicalCreateTable &op, PhysicalOperator &child_plan);

private:
	//! Extract target table information from LogicalCreateTable
	static CTASTarget ExtractTarget(const LogicalCreateTable &op, MSSQLCatalog &catalog);

	//! Map columns from child plan types to CTAS column definitions
	static vector<CTASColumnDef> MapColumns(const LogicalCreateTable &op, PhysicalOperator &child_plan,
											const CTASConfig &config);
};

}  // namespace mssql
}  // namespace duckdb
