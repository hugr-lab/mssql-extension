// Function Mapping - DuckDB to SQL Server
// Feature: 013-table-scan-filter-refactor
//
// NAMING CONVENTION:
// - Namespace: duckdb::mssql (MSSQL-specific module)
// - Types in duckdb::mssql do NOT use MSSQL prefix

#pragma once

#include <string>

namespace duckdb {
namespace mssql {

/**
 * Mapping from DuckDB function name to SQL Server equivalent.
 */
struct FunctionMapping {
	const char *duckdb_name;       // DuckDB function name (lowercase)
	const char *sql_template;      // T-SQL template with {0}, {1}, ... placeholders
	int expected_args;             // Expected argument count (-1 for variadic)
};

/**
 * Get the SQL Server equivalent for a DuckDB function.
 * @param function_name DuckDB function name (will be lowercased internally)
 * @return Pointer to FunctionMapping if supported, nullptr if not
 */
const FunctionMapping *GetFunctionMapping(const std::string &function_name);

/**
 * Check if a function is supported for pushdown.
 */
bool IsFunctionSupported(const std::string &function_name);

/**
 * Check if a function is a LIKE pattern function (prefix, suffix, contains, etc.)
 */
bool IsLikePatternFunction(const std::string &function_name);

/**
 * Check if a function is a case-insensitive LIKE pattern function (iprefix, isuffix, icontains)
 */
bool IsCaseInsensitiveLikeFunction(const std::string &function_name);

/**
 * Get the SQL Server date part identifier for a DuckDB date part string.
 * @param duckdb_part DuckDB date part (e.g., "year", "month", "day")
 * @param out_result Output: SQL Server date part identifier
 * @return true if mapping found, false if not supported
 */
bool GetDatePartMapping(const std::string &duckdb_part, std::string &out_result);

} // namespace mssql
} // namespace duckdb
