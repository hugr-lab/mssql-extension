// Table Scan Module - Public Interface
// Feature: 013-table-scan-filter-refactor
//
// NAMING CONVENTION:
// - Namespace: duckdb::mssql (MSSQL-specific module)
// - Types in duckdb::mssql do NOT use MSSQL prefix (e.g., TableScanBindData)
// - Types directly in duckdb namespace MUST use MSSQL prefix (e.g., MSSQLCatalog)

#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace mssql {

/**
 * Get the table function for catalog-based MSSQL table scans.
 *
 * This function is called by MSSQLTableEntry::GetScanFunction().
 *
 * The returned function has:
 * - func.projection_pushdown = true
 * - func.filter_pushdown = true
 * - func.filter_prune = true
 * - MaxThreads() = 1 (single-threaded)
 */
TableFunction GetCatalogScanFunction();

} // namespace mssql
} // namespace duckdb
