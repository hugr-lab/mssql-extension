// Filter Encoder
// Feature: 013-table-scan-filter-refactor
//
// NAMING CONVENTION:
// - Namespace: duckdb::mssql (MSSQL-specific module)
// - Types in duckdb::mssql do NOT use MSSQL prefix

#pragma once

#include <string>
#include <vector>
#include "duckdb.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"

namespace duckdb {
namespace mssql {

//------------------------------------------------------------------------------
// Result Structures
//------------------------------------------------------------------------------

/**
 * Result of encoding a single expression or filter.
 */
struct ExpressionEncodeResult {
	std::string sql;    // T-SQL fragment (empty if not supported)
	bool supported;     // True if expression was fully encoded
};

/**
 * Result of encoding an entire filter set.
 */
struct FilterEncoderResult {
	std::string where_clause;      // Complete WHERE clause (without "WHERE" keyword)
	bool needs_duckdb_filter;      // True if DuckDB must re-apply all filters
};

//------------------------------------------------------------------------------
// Encoding Context
//------------------------------------------------------------------------------

/**
 * Context for expression encoding, passed through recursive calls.
 */
struct ExpressionEncodeContext {
	const std::vector<column_t> &column_ids;          // Projection mapping
	const std::vector<std::string> &column_names;     // All table column names
	const std::vector<LogicalType> &column_types;     // All table column types
	int depth;                                         // Current recursion depth
	static constexpr int MAX_DEPTH = 100;             // Maximum nesting depth

	ExpressionEncodeContext(
		const std::vector<column_t> &col_ids,
		const std::vector<std::string> &col_names,
		const std::vector<LogicalType> &col_types
	) : column_ids(col_ids), column_names(col_names), column_types(col_types), depth(0) {}

	// Create child context with incremented depth
	ExpressionEncodeContext child() const {
		ExpressionEncodeContext ctx(column_ids, column_names, column_types);
		ctx.depth = depth + 1;
		return ctx;
	}

	bool at_max_depth() const { return depth >= MAX_DEPTH; }
};

//------------------------------------------------------------------------------
// Filter Encoder Class
//------------------------------------------------------------------------------

/**
 * Main filter encoder class.
 * Converts DuckDB filter expressions to T-SQL WHERE clauses.
 */
class FilterEncoder {
public:
	/**
	 * Encode a TableFilterSet to a T-SQL WHERE clause.
	 *
	 * @param filters The DuckDB filter set (may be nullptr)
	 * @param column_ids Projection mapping: projected index → table column index
	 * @param column_names All table column names
	 * @param column_types All table column types
	 * @return FilterEncoderResult with WHERE clause and re-filter flag
	 *
	 * Contract:
	 * - If filters is nullptr or empty, returns empty where_clause
	 * - If any filter cannot be pushed, needs_duckdb_filter is true
	 * - All column references are bracket-escaped
	 * - All string literals use N'' prefix
	 * - Result is valid T-SQL syntax
	 */
	static FilterEncoderResult Encode(
		const TableFilterSet *filters,
		const std::vector<column_t> &column_ids,
		const std::vector<std::string> &column_names,
		const std::vector<LogicalType> &column_types
	);

	//--------------------------------------------------------------------------
	// Utility Functions (public for testing)
	//--------------------------------------------------------------------------

	/**
	 * Convert DuckDB Value to T-SQL literal.
	 */
	static std::string ValueToSQLLiteral(const Value &value, const LogicalType &type);

	/**
	 * Escape string for T-SQL string literal (doubles single quotes).
	 */
	static std::string EscapeStringLiteral(const std::string &str);

	/**
	 * Escape identifier for T-SQL bracket notation (doubles right brackets).
	 */
	static std::string EscapeBracketIdentifier(const std::string &identifier);

	/**
	 * Get T-SQL comparison operator for DuckDB ExpressionType.
	 * @param type The ExpressionType
	 * @param out_operator Output: the SQL operator string
	 * @return true if supported, false otherwise
	 */
	static bool GetComparisonOperator(ExpressionType type, std::string &out_operator);

	/**
	 * Get T-SQL arithmetic operator for DuckDB ExpressionType.
	 * @param type The ExpressionType
	 * @param out_operator Output: the SQL operator string
	 * @return true if supported, false otherwise
	 */
	static bool GetArithmeticOperator(ExpressionType type, std::string &out_operator);

	/**
	 * Escape SQL Server LIKE special characters in pattern.
	 * Escapes: % → [%], _ → [_], [ → [[]
	 */
	static std::string EscapeLikePattern(const std::string &pattern);

	/**
	 * Encode a DuckDB Expression to T-SQL.
	 * Used by pushdown_complex_filter callback for expressions that cannot
	 * be represented as simple TableFilter objects (e.g., year(col) = 2024).
	 */
	static ExpressionEncodeResult EncodeExpression(
		const Expression &expr,
		const ExpressionEncodeContext &ctx
	);

private:
	//--------------------------------------------------------------------------
	// TableFilter Encoding
	//--------------------------------------------------------------------------

	/**
	 * Encode a single TableFilter to T-SQL.
	 */
	static ExpressionEncodeResult EncodeFilter(
		const TableFilter &filter,
		const std::string &column_name,
		const LogicalType &column_type,
		const ExpressionEncodeContext &ctx
	);

	/**
	 * Encode CONSTANT_COMPARISON filter (col OP value).
	 */
	static ExpressionEncodeResult EncodeConstantComparison(
		const ConstantFilter &filter,
		const std::string &column_name,
		const LogicalType &column_type
	);

	/**
	 * Encode IS_NULL filter.
	 */
	static ExpressionEncodeResult EncodeIsNull(const std::string &column_name);

	/**
	 * Encode IS_NOT_NULL filter.
	 */
	static ExpressionEncodeResult EncodeIsNotNull(const std::string &column_name);

	/**
	 * Encode IN_FILTER (col IN (values)).
	 */
	static ExpressionEncodeResult EncodeInFilter(
		const InFilter &filter,
		const std::string &column_name,
		const LogicalType &column_type
	);

	/**
	 * Encode CONJUNCTION_AND filter.
	 * Partial pushdown allowed: unsupported children are skipped.
	 */
	static ExpressionEncodeResult EncodeConjunctionAnd(
		const ConjunctionAndFilter &filter,
		const std::string &column_name,
		const LogicalType &column_type,
		const ExpressionEncodeContext &ctx
	);

	/**
	 * Encode CONJUNCTION_OR filter.
	 * All-or-nothing: if any child unsupported, entire OR is skipped.
	 */
	static ExpressionEncodeResult EncodeConjunctionOr(
		const ConjunctionOrFilter &filter,
		const std::string &column_name,
		const LogicalType &column_type,
		const ExpressionEncodeContext &ctx
	);

	/**
	 * Encode EXPRESSION_FILTER (arbitrary expression).
	 */
	static ExpressionEncodeResult EncodeExpressionFilter(
		const ExpressionFilter &filter,
		const ExpressionEncodeContext &ctx
	);

	//--------------------------------------------------------------------------
	// Expression Encoding Helpers
	//--------------------------------------------------------------------------

	/**
	 * Encode a function call expression.
	 */
	static ExpressionEncodeResult EncodeFunctionExpression(
		const BoundFunctionExpression &expr,
		const ExpressionEncodeContext &ctx
	);

	/**
	 * Encode a comparison expression (left OP right).
	 */
	static ExpressionEncodeResult EncodeComparisonExpression(
		const BoundComparisonExpression &expr,
		const ExpressionEncodeContext &ctx
	);

	/**
	 * Encode an operator expression (+, -, *, /, etc.).
	 */
	static ExpressionEncodeResult EncodeOperatorExpression(
		const BoundOperatorExpression &expr,
		const ExpressionEncodeContext &ctx
	);

	/**
	 * Encode a CASE expression.
	 */
	static ExpressionEncodeResult EncodeCaseExpression(
		const BoundCaseExpression &expr,
		const ExpressionEncodeContext &ctx
	);

	/**
	 * Encode a column reference.
	 */
	static ExpressionEncodeResult EncodeColumnRef(
		const BoundColumnRefExpression &expr,
		const ExpressionEncodeContext &ctx
	);

	/**
	 * Encode a constant value.
	 */
	static ExpressionEncodeResult EncodeConstant(
		const BoundConstantExpression &expr
	);

	/**
	 * Encode a conjunction (AND/OR) expression.
	 */
	static ExpressionEncodeResult EncodeConjunctionExpression(
		const BoundConjunctionExpression &expr,
		const ExpressionEncodeContext &ctx
	);

	//--------------------------------------------------------------------------
	// LIKE Pattern Helpers
	//--------------------------------------------------------------------------

	/**
	 * Encode prefix/suffix/contains pattern function.
	 * @param function_name One of: prefix, suffix, contains, iprefix, isuffix, icontains
	 * @param column_expr The column expression
	 * @param pattern_expr The pattern expression
	 * @param ctx Encoding context
	 */
	static ExpressionEncodeResult EncodeLikePattern(
		const std::string &function_name,
		const Expression &column_expr,
		const Expression &pattern_expr,
		const ExpressionEncodeContext &ctx
	);
};

} // namespace mssql
} // namespace duckdb
