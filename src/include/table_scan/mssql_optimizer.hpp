//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// table_scan/mssql_optimizer.hpp
//
// ORDER BY pushdown optimizer extension (Spec 039)
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/optimizer_extension.hpp"

namespace duckdb {

class MSSQLOptimizer {
public:
	//! Optimizer callback: detect ORDER BY / TOP N patterns above MSSQL scans
	//! and push them down to SQL Server
	static void Optimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);

	//! Registered entry point. Runs Optimize over the tree, then the whole-plan
	//! pass that materializes catalog scans sharing a pinned connection (#239).
	static void OptimizeRoot(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);

	//! Registered as the pre-optimize hook (spec 080 D4, PR 4b): every MERGE
	//! into a catalog table becomes a staged MERGE (mssql::RewriteMerge) or is
	//! refused by name, before DuckDB's optimizers take the binder's join apart.
	static void PreOptimizeRoot(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);
};

}  // namespace duckdb
