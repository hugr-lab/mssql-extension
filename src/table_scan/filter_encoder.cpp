// Filter Encoder Implementation
// Feature: 013-table-scan-filter-refactor
//
// This is the initial minimal implementation for backward compatibility.
// Enhanced expression support (LIKE patterns, functions, CASE, arithmetic)
// will be added in subsequent phases.

#include "table_scan/filter_encoder.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/null_filter.hpp"
#include "table_scan/function_mapping.hpp"

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define MSSQL_FILTER_DEBUG_LOG(level, fmt, ...)                         \
	do {                                                                \
		if (GetDebugLevel() >= level) {                                 \
			fprintf(stderr, "[MSSQL FILTER] " fmt "\n", ##__VA_ARGS__); \
		}                                                               \
	} while (0)

namespace duckdb {
namespace mssql {

//------------------------------------------------------------------------------
// Utility Functions
//------------------------------------------------------------------------------

std::string FilterEncoder::EscapeStringLiteral(const std::string &str) {
	std::string result;
	result.reserve(str.size() + 10);
	for (char c : str) {
		result += c;
		if (c == '\'') {
			result += '\'';	 // Double the ' character
		}
	}
	return result;
}

std::string FilterEncoder::EscapeBracketIdentifier(const std::string &identifier) {
	std::string result;
	result.reserve(identifier.size() + 2);
	for (char c : identifier) {
		result += c;
		if (c == ']') {
			result += ']';	// Double the ] character
		}
	}
	return result;
}

std::string FilterEncoder::EscapeLikePattern(const std::string &pattern) {
	std::string result;
	result.reserve(pattern.size() + 10);
	for (char c : pattern) {
		switch (c) {
		case '%':
			result += "[%]";
			break;
		case '_':
			result += "[_]";
			break;
		case '[':
			result += "[[]";
			break;
		default:
			result += c;
			break;
		}
	}
	return result;
}

bool FilterEncoder::GetComparisonOperator(ExpressionType type, std::string &out_operator) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		out_operator = " = ";
		return true;
	case ExpressionType::COMPARE_NOTEQUAL:
		out_operator = " <> ";
		return true;
	case ExpressionType::COMPARE_LESSTHAN:
		out_operator = " < ";
		return true;
	case ExpressionType::COMPARE_GREATERTHAN:
		out_operator = " > ";
		return true;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		out_operator = " <= ";
		return true;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		out_operator = " >= ";
		return true;
	default:
		return false;
	}
}

bool FilterEncoder::GetArithmeticOperator(ExpressionType type, std::string &out_operator) {
	// In DuckDB stable, arithmetic operations are handled as BoundFunctionExpression
	// with function names like "+", "-", "*", "/", not as ExpressionType operators.
	// This function is kept for potential future use but returns false for now.
	(void)type;
	(void)out_operator;
	return false;
}

std::string FilterEncoder::ValueToSQLLiteral(const Value &value, const LogicalType &type) {
	if (value.IsNull()) {
		return "NULL";
	}

	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return value.GetValue<bool>() ? "1" : "0";

	case LogicalTypeId::TINYINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UBIGINT:
		return value.ToString();

	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		return value.ToString();

	case LogicalTypeId::DECIMAL:
		return value.ToString();

	case LogicalTypeId::VARCHAR:
		// Use N'' for NVARCHAR compatibility and escape single quotes
		return "N'" + EscapeStringLiteral(value.ToString()) + "'";

	case LogicalTypeId::DATE: {
		auto date_val = value.GetValue<date_t>();
		return "'" + Date::ToString(date_val) + "'";
	}

	case LogicalTypeId::TIME:
		// TIME is stored as microseconds since midnight
		return "'" + value.ToString() + "'";

	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_TZ: {
		auto ts_val = value.GetValue<timestamp_t>();
		return "'" + Timestamp::ToString(ts_val) + "'";
	}

	case LogicalTypeId::UUID: {
		return "'" + value.ToString() + "'";
	}

	case LogicalTypeId::BLOB: {
		// Convert blob to hex string for SQL Server
		auto blob_val = value.GetValueUnsafe<string_t>();
		std::string hex = "0x";
		for (idx_t i = 0; i < blob_val.GetSize(); i++) {
			char buf[3];
			snprintf(buf, sizeof(buf), "%02X", (unsigned char)blob_val.GetData()[i]);
			hex += buf;
		}
		return hex;
	}

	default:
		// For other types, try ToString and quote as string
		return "N'" + EscapeStringLiteral(value.ToString()) + "'";
	}
}

//------------------------------------------------------------------------------
// Main Encode Function
//------------------------------------------------------------------------------

FilterEncoderResult FilterEncoder::Encode(const TableFilterSet *filters, const std::vector<column_t> &column_ids,
										  const std::vector<std::string> &column_names,
										  const std::vector<LogicalType> &column_types) {
	FilterEncoderResult result;
	result.needs_duckdb_filter = false;

	if (!filters || filters->filters.empty()) {
		MSSQL_FILTER_DEBUG_LOG(1, "Encode: no filters to encode");
		return result;
	}

	MSSQL_FILTER_DEBUG_LOG(1, "Encode: encoding %zu filter(s)", filters->filters.size());

	ExpressionEncodeContext ctx(column_ids, column_names, column_types);
	std::vector<std::string> where_conditions;

	// Virtual/special column identifiers start at 2^63
	constexpr column_t VIRTUAL_COL_START = UINT64_C(9223372036854775808);

	for (const auto &filter_entry : filters->filters) {
		idx_t projected_col_idx = filter_entry.first;

		// Map from projected column index to actual table column index
		idx_t table_col_idx;
		if (column_ids.empty()) {
			// No projection - use filter index directly as table column index
			table_col_idx = projected_col_idx;
		} else if (projected_col_idx >= column_ids.size()) {
			MSSQL_FILTER_DEBUG_LOG(1, "  filter column index %llu out of projected range (%zu), skipping",
								   (unsigned long long)projected_col_idx, column_ids.size());
			result.needs_duckdb_filter = true;
			continue;
		} else {
			// Map through column_ids to get actual table column index
			table_col_idx = column_ids[projected_col_idx];
		}

		// Skip virtual/special columns
		if (table_col_idx >= VIRTUAL_COL_START) {
			MSSQL_FILTER_DEBUG_LOG(2, "  skipping virtual column_id=%llu", (unsigned long long)table_col_idx);
			result.needs_duckdb_filter = true;
			continue;
		}

		if (table_col_idx >= column_names.size()) {
			MSSQL_FILTER_DEBUG_LOG(1, "  table column index %llu out of range (%zu), skipping",
								   (unsigned long long)table_col_idx, column_names.size());
			result.needs_duckdb_filter = true;
			continue;
		}

		const std::string &col_name = column_names[table_col_idx];
		const LogicalType &col_type = column_types[table_col_idx];
		std::string escaped_col = "[" + EscapeBracketIdentifier(col_name) + "]";

		MSSQL_FILTER_DEBUG_LOG(2, "  encoding filter for column: projected_idx=%llu -> table_idx=%llu -> %s",
							   (unsigned long long)projected_col_idx, (unsigned long long)table_col_idx,
							   col_name.c_str());

		auto encode_result = EncodeFilter(*filter_entry.second, escaped_col, col_type, ctx);

		if (encode_result.supported && !encode_result.sql.empty()) {
			where_conditions.push_back(encode_result.sql);
			MSSQL_FILTER_DEBUG_LOG(2, "    encoded: %s", encode_result.sql.c_str());
		}

		if (!encode_result.supported) {
			MSSQL_FILTER_DEBUG_LOG(2, "    filter not fully supported, will need DuckDB re-filter");
			result.needs_duckdb_filter = true;
		}
	}

	// Combine all conditions with AND
	if (!where_conditions.empty()) {
		for (idx_t i = 0; i < where_conditions.size(); i++) {
			if (i > 0) {
				result.where_clause += " AND ";
			}
			result.where_clause += where_conditions[i];
		}
		MSSQL_FILTER_DEBUG_LOG(1, "Encode: generated WHERE clause: %s", result.where_clause.c_str());
	}

	MSSQL_FILTER_DEBUG_LOG(1, "Encode: needs_duckdb_filter=%s", result.needs_duckdb_filter ? "true" : "false");
	return result;
}

//------------------------------------------------------------------------------
// TableFilter Encoding
//------------------------------------------------------------------------------

ExpressionEncodeResult FilterEncoder::EncodeFilter(const TableFilter &filter, const std::string &column_name,
												   const LogicalType &column_type, const ExpressionEncodeContext &ctx) {
	switch (filter.filter_type) {
	case TableFilterType::CONSTANT_COMPARISON:
		return EncodeConstantComparison(filter.Cast<ConstantFilter>(), column_name, column_type);

	case TableFilterType::IS_NULL:
		return EncodeIsNull(column_name);

	case TableFilterType::IS_NOT_NULL:
		return EncodeIsNotNull(column_name);

	case TableFilterType::IN_FILTER:
		return EncodeInFilter(filter.Cast<InFilter>(), column_name, column_type);

	case TableFilterType::CONJUNCTION_OR:
		return EncodeConjunctionOr(filter.Cast<ConjunctionOrFilter>(), column_name, column_type, ctx);

	case TableFilterType::CONJUNCTION_AND:
		return EncodeConjunctionAnd(filter.Cast<ConjunctionAndFilter>(), column_name, column_type, ctx);

	case TableFilterType::EXPRESSION_FILTER:
		// Expression filters (for complex expressions) - not yet fully supported
		// Will be enhanced in later phases
		return EncodeExpressionFilter(filter.Cast<ExpressionFilter>(), ctx);

	case TableFilterType::OPTIONAL_FILTER:
	case TableFilterType::STRUCT_EXTRACT:
	case TableFilterType::DYNAMIC_FILTER:
	default:
		// These filter types cannot be pushed down to SQL Server
		MSSQL_FILTER_DEBUG_LOG(1, "Filter type %d cannot be pushed down", (int)filter.filter_type);
		return {"", false};
	}
}

ExpressionEncodeResult FilterEncoder::EncodeConstantComparison(const ConstantFilter &filter,
															   const std::string &column_name,
															   const LogicalType &column_type) {
	std::string op;
	if (!GetComparisonOperator(filter.comparison_type, op)) {
		return {"", false};
	}

	std::string sql = column_name + op + ValueToSQLLiteral(filter.constant, column_type);
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeIsNull(const std::string &column_name) {
	return {column_name + " IS NULL", true};
}

ExpressionEncodeResult FilterEncoder::EncodeIsNotNull(const std::string &column_name) {
	return {column_name + " IS NOT NULL", true};
}

ExpressionEncodeResult FilterEncoder::EncodeInFilter(const InFilter &filter, const std::string &column_name,
													 const LogicalType &column_type) {
	std::string sql = column_name + " IN (";
	for (idx_t i = 0; i < filter.values.size(); i++) {
		if (i > 0) {
			sql += ", ";
		}
		sql += ValueToSQLLiteral(filter.values[i], column_type);
	}
	sql += ")";
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeConjunctionAnd(const ConjunctionAndFilter &filter,
														   const std::string &column_name,
														   const LogicalType &column_type,
														   const ExpressionEncodeContext &ctx) {
	if (filter.child_filters.empty()) {
		return {"", false};
	}

	std::vector<std::string> conditions;
	bool all_supported = true;

	for (const auto &child : filter.child_filters) {
		auto result = EncodeFilter(*child, column_name, column_type, ctx);
		if (result.supported && !result.sql.empty()) {
			conditions.push_back(result.sql);
		}
		if (!result.supported) {
			all_supported = false;
		}
	}

	if (conditions.empty()) {
		return {"", false};
	}

	if (conditions.size() == 1) {
		return {conditions[0], all_supported};
	}

	std::string sql = "(";
	for (idx_t i = 0; i < conditions.size(); i++) {
		if (i > 0) {
			sql += " AND ";
		}
		sql += conditions[i];
	}
	sql += ")";
	return {sql, all_supported};
}

ExpressionEncodeResult FilterEncoder::EncodeConjunctionOr(const ConjunctionOrFilter &filter,
														  const std::string &column_name,
														  const LogicalType &column_type,
														  const ExpressionEncodeContext &ctx) {
	if (filter.child_filters.empty()) {
		return {"", false};
	}

	// OR is all-or-nothing: if any child is unsupported, skip entire OR
	std::vector<std::string> conditions;
	for (const auto &child : filter.child_filters) {
		auto result = EncodeFilter(*child, column_name, column_type, ctx);
		if (!result.supported || result.sql.empty()) {
			// Cannot push OR if any child is unsupported
			MSSQL_FILTER_DEBUG_LOG(2, "  OR child not supported, skipping entire OR");
			return {"", false};
		}
		conditions.push_back(result.sql);
	}

	if (conditions.size() == 1) {
		return {conditions[0], true};
	}

	std::string sql = "(";
	for (idx_t i = 0; i < conditions.size(); i++) {
		if (i > 0) {
			sql += " OR ";
		}
		sql += conditions[i];
	}
	sql += ")";
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeExpressionFilter(const ExpressionFilter &filter,
															 const ExpressionEncodeContext &ctx) {
	// Expression filters contain arbitrary expressions
	MSSQL_FILTER_DEBUG_LOG(1, "EncodeExpressionFilter: encoding expression type %d", (int)filter.expr->type);
	return EncodeExpression(*filter.expr, ctx);
}

//------------------------------------------------------------------------------
// Expression Encoding
//------------------------------------------------------------------------------

ExpressionEncodeResult FilterEncoder::EncodeExpression(const Expression &expr, const ExpressionEncodeContext &ctx) {
	// Check recursion depth
	if (ctx.at_max_depth()) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeExpression: max depth reached");
		return {"", false};
	}

	MSSQL_FILTER_DEBUG_LOG(2, "EncodeExpression: type=%d class=%d", (int)expr.type, (int)expr.GetExpressionClass());

	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF:
		return EncodeColumnRef(expr.Cast<BoundColumnRefExpression>(), ctx);

	case ExpressionClass::BOUND_CONSTANT:
		return EncodeConstant(expr.Cast<BoundConstantExpression>());

	case ExpressionClass::BOUND_FUNCTION:
		return EncodeFunctionExpression(expr.Cast<BoundFunctionExpression>(), ctx);

	case ExpressionClass::BOUND_COMPARISON:
		return EncodeComparisonExpression(expr.Cast<BoundComparisonExpression>(), ctx);

	case ExpressionClass::BOUND_CONJUNCTION:
		return EncodeConjunctionExpression(expr.Cast<BoundConjunctionExpression>(), ctx);

	case ExpressionClass::BOUND_OPERATOR:
		return EncodeOperatorExpression(expr.Cast<BoundOperatorExpression>(), ctx);

	case ExpressionClass::BOUND_CASE:
		return EncodeCaseExpression(expr.Cast<BoundCaseExpression>(), ctx);

	default:
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeExpression: unsupported expression class %d", (int)expr.GetExpressionClass());
		return {"", false};
	}
}

ExpressionEncodeResult FilterEncoder::EncodeFunctionExpression(const BoundFunctionExpression &expr,
															   const ExpressionEncodeContext &ctx) {
	const std::string &func_name = expr.function.name;
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeFunctionExpression: function=%s, args=%zu", func_name.c_str(),
						   expr.children.size());

	// Check for LIKE pattern functions (prefix, suffix, contains, iprefix, isuffix, icontains)
	if (IsLikePatternFunction(func_name)) {
		if (expr.children.size() >= 2) {
			return EncodeLikePattern(func_name, *expr.children[0], *expr.children[1], ctx);
		}
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeFunctionExpression: LIKE pattern function %s needs 2 args, got %zu",
							   func_name.c_str(), expr.children.size());
		return {"", false};
	}

	// Check for supported functions in the mapping table
	const FunctionMapping *mapping = GetFunctionMapping(func_name);
	if (!mapping) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeFunctionExpression: function %s not supported", func_name.c_str());
		return {"", false};
	}

	// Validate argument count
	if (mapping->expected_args != static_cast<int>(expr.children.size())) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeFunctionExpression: %s expects %d args, got %zu", func_name.c_str(),
							   mapping->expected_args, expr.children.size());
		return {"", false};
	}

	// Encode all arguments
	auto child_ctx = ctx.child();
	std::vector<std::string> encoded_args;
	for (const auto &child : expr.children) {
		auto result = EncodeExpression(*child, child_ctx);
		if (!result.supported) {
			MSSQL_FILTER_DEBUG_LOG(1, "EncodeFunctionExpression: argument encoding failed for %s", func_name.c_str());
			return {"", false};
		}
		encoded_args.push_back(result.sql);
	}

	// Apply the template
	std::string sql = mapping->sql_template;
	for (size_t i = 0; i < encoded_args.size(); i++) {
		std::string placeholder = "{" + std::to_string(i) + "}";
		size_t pos = 0;
		while ((pos = sql.find(placeholder, pos)) != std::string::npos) {
			sql.replace(pos, placeholder.length(), encoded_args[i]);
			pos += encoded_args[i].length();
		}
	}

	MSSQL_FILTER_DEBUG_LOG(2, "EncodeFunctionExpression: encoded %s -> %s", func_name.c_str(), sql.c_str());
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeComparisonExpression(const BoundComparisonExpression &expr,
																 const ExpressionEncodeContext &ctx) {
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeComparisonExpression: type=%d", (int)expr.type);

	// Check for rowid equality: rowid = value (Spec 001-pk-rowid-semantics)
	if (expr.type == ExpressionType::COMPARE_EQUAL && ctx.HasPKInfo()) {
		// Check if left is rowid and right is constant
		if (IsRowidColumn(*expr.left, ctx)) {
			MSSQL_FILTER_DEBUG_LOG(2, "EncodeComparisonExpression: detected rowid = value");
			return EncodeRowidEquality(*expr.right, ctx);
		}
		// Check if right is rowid and left is constant (value = rowid)
		if (IsRowidColumn(*expr.right, ctx)) {
			MSSQL_FILTER_DEBUG_LOG(2, "EncodeComparisonExpression: detected value = rowid");
			return EncodeRowidEquality(*expr.left, ctx);
		}
	}

	// Get the comparison operator
	std::string op;
	if (!GetComparisonOperator(expr.type, op)) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeComparisonExpression: unsupported comparison type %d", (int)expr.type);
		return {"", false};
	}

	// Encode left and right sides
	auto child_ctx = ctx.child();
	auto left_result = EncodeExpression(*expr.left, child_ctx);
	if (!left_result.supported) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeComparisonExpression: left side encoding failed");
		return {"", false};
	}

	auto right_result = EncodeExpression(*expr.right, child_ctx);
	if (!right_result.supported) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeComparisonExpression: right side encoding failed");
		return {"", false};
	}

	std::string sql = "(" + left_result.sql + op + right_result.sql + ")";
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeComparisonExpression: encoded -> %s", sql.c_str());
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeOperatorExpression(const BoundOperatorExpression &expr,
															   const ExpressionEncodeContext &ctx) {
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeOperatorExpression: type=%d, children=%zu", (int)expr.type, expr.children.size());

	// Handle NOT operator
	if (expr.type == ExpressionType::OPERATOR_NOT) {
		if (expr.children.size() != 1) {
			return {"", false};
		}
		auto child_ctx = ctx.child();
		auto child_result = EncodeExpression(*expr.children[0], child_ctx);
		if (!child_result.supported) {
			return {"", false};
		}
		return {"(NOT " + child_result.sql + ")", true};
	}

	// Handle IS NULL / IS NOT NULL operators
	if (expr.type == ExpressionType::OPERATOR_IS_NULL) {
		if (expr.children.size() != 1) {
			return {"", false};
		}
		auto child_ctx = ctx.child();
		auto child_result = EncodeExpression(*expr.children[0], child_ctx);
		if (!child_result.supported) {
			return {"", false};
		}
		return {"(" + child_result.sql + " IS NULL)", true};
	}

	if (expr.type == ExpressionType::OPERATOR_IS_NOT_NULL) {
		if (expr.children.size() != 1) {
			return {"", false};
		}
		auto child_ctx = ctx.child();
		auto child_result = EncodeExpression(*expr.children[0], child_ctx);
		if (!child_result.supported) {
			return {"", false};
		}
		return {"(" + child_result.sql + " IS NOT NULL)", true};
	}

	// For other operators, we don't support them yet
	// Arithmetic is handled via BoundFunctionExpression in DuckDB stable
	MSSQL_FILTER_DEBUG_LOG(1, "EncodeOperatorExpression: unsupported operator type %d", (int)expr.type);
	return {"", false};
}

ExpressionEncodeResult FilterEncoder::EncodeCaseExpression(const BoundCaseExpression &expr,
														   const ExpressionEncodeContext &ctx) {
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeCaseExpression: case_checks=%zu", expr.case_checks.size());

	auto child_ctx = ctx.child();
	std::string sql = "CASE";

	// Encode each WHEN ... THEN clause
	for (const auto &check : expr.case_checks) {
		auto when_result = EncodeExpression(*check.when_expr, child_ctx);
		if (!when_result.supported) {
			MSSQL_FILTER_DEBUG_LOG(1, "EncodeCaseExpression: WHEN clause encoding failed");
			return {"", false};
		}

		auto then_result = EncodeExpression(*check.then_expr, child_ctx);
		if (!then_result.supported) {
			MSSQL_FILTER_DEBUG_LOG(1, "EncodeCaseExpression: THEN clause encoding failed");
			return {"", false};
		}

		sql += " WHEN " + when_result.sql + " THEN " + then_result.sql;
	}

	// Encode ELSE clause
	auto else_result = EncodeExpression(*expr.else_expr, child_ctx);
	if (!else_result.supported) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeCaseExpression: ELSE clause encoding failed");
		return {"", false};
	}
	sql += " ELSE " + else_result.sql + " END";

	MSSQL_FILTER_DEBUG_LOG(2, "EncodeCaseExpression: encoded -> %s", sql.c_str());
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeColumnRef(const BoundColumnRefExpression &expr,
													  const ExpressionEncodeContext &ctx) {
	// Get the column binding - this contains the table index and column index
	const auto &binding = expr.binding;
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeColumnRef: table_idx=%llu, column_idx=%llu",
						   (unsigned long long)binding.table_index, (unsigned long long)binding.column_index);

	// Virtual/special column identifiers start at 2^63
	constexpr column_t VIRTUAL_COL_START = UINT64_C(9223372036854775808);

	// The column_index from binding refers to the projected column index
	// We need to map it through column_ids to get the actual table column index
	column_t projected_idx = binding.column_index;

	column_t table_col_idx;
	if (ctx.column_ids.empty()) {
		// No projection - use binding index directly
		table_col_idx = projected_idx;
	} else if (projected_idx >= ctx.column_ids.size()) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeColumnRef: column index %llu out of range (projection has %zu)",
							   (unsigned long long)projected_idx, ctx.column_ids.size());
		return {"", false};
	} else {
		table_col_idx = ctx.column_ids[projected_idx];
	}

	// Handle rowid virtual column (Spec 001-pk-rowid-semantics)
	// For non-equality expressions like rowid > 100, we can use scalar PK
	if (table_col_idx == COLUMN_IDENTIFIER_ROW_ID) {
		// Only scalar PK can be used in arbitrary expressions
		if (ctx.HasPKInfo() && !ctx.pk_is_composite) {
			std::string sql = "[" + EscapeBracketIdentifier((*ctx.pk_column_names)[0]) + "]";
			MSSQL_FILTER_DEBUG_LOG(2, "EncodeColumnRef: rowid (scalar PK) -> %s", sql.c_str());
			return {sql, true};
		}
		// Composite PK rowid can only be used in equality (handled in EncodeComparisonExpression)
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeColumnRef: rowid not supported for non-equality (composite PK or no PK info)");
		return {"", false};
	}

	// Skip other virtual columns
	if (table_col_idx >= VIRTUAL_COL_START) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeColumnRef: virtual column %llu not supported",
							   (unsigned long long)table_col_idx);
		return {"", false};
	}

	if (table_col_idx >= ctx.column_names.size()) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeColumnRef: table column index %llu out of range (table has %zu)",
							   (unsigned long long)table_col_idx, ctx.column_names.size());
		return {"", false};
	}

	const std::string &col_name = ctx.column_names[table_col_idx];
	std::string sql = "[" + EscapeBracketIdentifier(col_name) + "]";
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeColumnRef: encoded -> %s", sql.c_str());
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeConstant(const BoundConstantExpression &expr) {
	std::string sql = ValueToSQLLiteral(expr.value, expr.return_type);
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeConstant: value=%s, type=%s -> %s", expr.value.ToString().c_str(),
						   expr.return_type.ToString().c_str(), sql.c_str());
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeConjunctionExpression(const BoundConjunctionExpression &expr,
																  const ExpressionEncodeContext &ctx) {
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeConjunctionExpression: type=%d, children=%zu", (int)expr.type,
						   expr.children.size());

	if (expr.children.empty()) {
		return {"", false};
	}

	bool is_and = (expr.type == ExpressionType::CONJUNCTION_AND);
	std::string conj_op = is_and ? " AND " : " OR ";

	auto child_ctx = ctx.child();
	std::vector<std::string> conditions;
	bool all_supported = true;

	for (const auto &child : expr.children) {
		auto result = EncodeExpression(*child, child_ctx);
		if (is_and) {
			// AND: partial pushdown allowed - skip unsupported children
			if (result.supported && !result.sql.empty()) {
				conditions.push_back(result.sql);
			}
			if (!result.supported) {
				all_supported = false;
			}
		} else {
			// OR: all-or-nothing - if any child is unsupported, reject entire OR
			if (!result.supported || result.sql.empty()) {
				MSSQL_FILTER_DEBUG_LOG(1, "EncodeConjunctionExpression: OR child not supported, rejecting entire OR");
				return {"", false};
			}
			conditions.push_back(result.sql);
		}
	}

	if (conditions.empty()) {
		return {"", false};
	}

	if (conditions.size() == 1) {
		return {conditions[0], all_supported};
	}

	std::string sql = "(";
	for (idx_t i = 0; i < conditions.size(); i++) {
		if (i > 0) {
			sql += conj_op;
		}
		sql += conditions[i];
	}
	sql += ")";

	MSSQL_FILTER_DEBUG_LOG(2, "EncodeConjunctionExpression: encoded -> %s", sql.c_str());
	return {sql, all_supported};
}

ExpressionEncodeResult FilterEncoder::EncodeLikePattern(const std::string &function_name, const Expression &column_expr,
														const Expression &pattern_expr,
														const ExpressionEncodeContext &ctx) {
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeLikePattern: function=%s", function_name.c_str());

	// Encode the column expression
	auto child_ctx = ctx.child();
	auto column_result = EncodeExpression(column_expr, child_ctx);
	if (!column_result.supported) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeLikePattern: column encoding failed");
		return {"", false};
	}

	// The pattern must be a constant for us to encode it properly
	// (We need to escape LIKE special characters in the pattern)
	if (pattern_expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeLikePattern: pattern is not a constant, cannot push down");
		return {"", false};
	}

	const auto &pattern_const = pattern_expr.Cast<BoundConstantExpression>();
	if (pattern_const.value.IsNull()) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeLikePattern: pattern is NULL");
		return {"", false};
	}

	std::string pattern_str = pattern_const.value.ToString();
	std::string escaped_pattern = EscapeLikePattern(pattern_str);

	// Convert function name to lowercase for comparison
	std::string lower_func = function_name;
	std::transform(lower_func.begin(), lower_func.end(), lower_func.begin(),
				   [](unsigned char c) { return std::tolower(c); });

	// Check if this is a case-insensitive LIKE function
	bool case_insensitive = IsCaseInsensitiveLikeFunction(function_name);

	// Build the LIKE pattern based on function type
	std::string like_pattern;
	if (lower_func == "prefix" || lower_func == "iprefix") {
		// prefix: column LIKE 'pattern%'
		like_pattern = "N'" + EscapeStringLiteral(escaped_pattern) + "%'";
	} else if (lower_func == "suffix" || lower_func == "isuffix") {
		// suffix: column LIKE '%pattern'
		like_pattern = "N'%" + EscapeStringLiteral(escaped_pattern) + "'";
	} else if (lower_func == "contains" || lower_func == "icontains") {
		// contains: column LIKE '%pattern%'
		like_pattern = "N'%" + EscapeStringLiteral(escaped_pattern) + "%'";
	} else {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeLikePattern: unknown LIKE pattern function %s", function_name.c_str());
		return {"", false};
	}

	// Build the T-SQL expression
	std::string sql;
	if (case_insensitive) {
		// ILIKE: apply LOWER() to both column and pattern
		sql = "(LOWER(" + column_result.sql + ") LIKE LOWER(" + like_pattern + "))";
	} else {
		// Case-sensitive LIKE
		sql = "(" + column_result.sql + " LIKE " + like_pattern + ")";
	}

	MSSQL_FILTER_DEBUG_LOG(2, "EncodeLikePattern: encoded -> %s", sql.c_str());
	return {sql, true};
}

//------------------------------------------------------------------------------
// Rowid Filter Pushdown Helpers (Spec 001-pk-rowid-semantics)
//------------------------------------------------------------------------------

bool FilterEncoder::IsRowidColumn(const Expression &expr, const ExpressionEncodeContext &ctx) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	auto &col_ref = expr.Cast<BoundColumnRefExpression>();
	idx_t projected_idx = col_ref.binding.column_index;

	if (ctx.column_ids.empty()) {
		// No projection - check if the index is COLUMN_IDENTIFIER_ROW_ID
		return projected_idx == COLUMN_IDENTIFIER_ROW_ID;
	}
	if (projected_idx >= ctx.column_ids.size()) {
		return false;
	}
	return ctx.column_ids[projected_idx] == COLUMN_IDENTIFIER_ROW_ID;
}

ExpressionEncodeResult FilterEncoder::EncodeRowidEquality(const Expression &value_expr,
                                                          const ExpressionEncodeContext &ctx) {
	if (!ctx.HasPKInfo()) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeRowidEquality: no PK info available");
		return {"", false};
	}

	// Value must be a constant
	if (value_expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeRowidEquality: value is not a constant");
		return {"", false};
	}
	auto &const_expr = value_expr.Cast<BoundConstantExpression>();

	if (ctx.pk_is_composite) {
		// Composite PK: rowid = {'col1': val1, 'col2': val2}
		// Extract struct children and build AND conditions
		if (const_expr.value.type().id() != LogicalTypeId::STRUCT) {
			MSSQL_FILTER_DEBUG_LOG(1, "EncodeRowidEquality: composite PK expects STRUCT value, got %s",
			                       const_expr.value.type().ToString().c_str());
			return {"", false};
		}
		auto &children = StructValue::GetChildren(const_expr.value);
		if (children.size() != ctx.pk_column_names->size()) {
			MSSQL_FILTER_DEBUG_LOG(1, "EncodeRowidEquality: STRUCT has %zu children, expected %zu",
			                       children.size(), ctx.pk_column_names->size());
			return {"", false};
		}

		std::string sql = "(";
		for (idx_t i = 0; i < children.size(); i++) {
			if (i > 0) {
				sql += " AND ";
			}
			sql += "[" + EscapeBracketIdentifier((*ctx.pk_column_names)[i]) + "]";
			sql += " = ";
			sql += ValueToSQLLiteral(children[i], (*ctx.pk_column_types)[i]);
		}
		sql += ")";
		MSSQL_FILTER_DEBUG_LOG(2, "EncodeRowidEquality: composite PK -> %s", sql.c_str());
		return {sql, true};
	} else {
		// Scalar PK: rowid = value
		std::string sql = "[" + EscapeBracketIdentifier((*ctx.pk_column_names)[0]) + "]";
		sql += " = ";
		sql += ValueToSQLLiteral(const_expr.value, (*ctx.pk_column_types)[0]);
		MSSQL_FILTER_DEBUG_LOG(2, "EncodeRowidEquality: scalar PK -> %s", sql.c_str());
		return {sql, true};
	}
}

}  // namespace mssql
}  // namespace duckdb
