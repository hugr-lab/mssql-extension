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
};

}  // namespace duckdb
