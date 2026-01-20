// Table Scan Bind Data
// Feature: 013-table-scan-filter-refactor
//
// NAMING CONVENTION:
// - Namespace: duckdb::mssql (MSSQL-specific module)
// - Types in duckdb::mssql do NOT use MSSQL prefix

#pragma once

#include <string>
#include <vector>
#include "duckdb.hpp"

namespace duckdb {
namespace mssql {

/**
 * Bind-time data for MSSQL catalog table scans.
 * Created in TableScanBind, used throughout scan lifecycle.
 */
struct TableScanBindData : public TableFunctionData {
	// Connection context
	std::string context_name;

	// Table identification
	std::string schema_name;
	std::string table_name;

	// Full table schema (all columns)
	std::vector<LogicalType> all_types;
	std::vector<std::string> all_column_names;

	// Projected schema (requested columns only)
	std::vector<LogicalType> return_types;
	std::vector<std::string> column_names;

	// Pre-executed result stream ID (for schema inference)
	uint64_t result_stream_id = 0;

	/**
	 * Get the full table name as [schema].[table].
	 */
	std::string GetFullTableName() const;

	/**
	 * Check if a column index is valid for projection.
	 */
	bool IsValidColumnIndex(idx_t idx) const;

	// FunctionData interface
	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;
};

}  // namespace mssql
}  // namespace duckdb
