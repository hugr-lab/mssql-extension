// Filter Encoder Implementation
// Feature: 013-table-scan-filter-refactor
//
// This is the initial minimal implementation for backward compatibility.
// Enhanced expression support (LIKE patterns, functions, CASE, arithmetic)
// will be added in subsequent phases.

#include "table_scan/filter_encoder.hpp"
#include <cstdlib>
#include "catalog/mssql_column_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/null_filter.hpp"
#include "pushdown/mssql_expression_vocabulary.hpp"
#include "query/mssql_identifier.hpp"
#include "query/mssql_sql_params.hpp"
#include "table_scan/function_mapping.hpp"

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
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

namespace {
// Does this expression already encode to a T-SQL search condition (a predicate
// valid after WHERE), as opposed to a bit-valued expression? A BOOLEAN column,
// constant or cast is a VALUE — `WHERE [b]` is a syntax error (SQL Server 4145),
// the predicate is `WHERE [b] = 1`. Comparisons, conjunctions, BETWEEN/IN,
// IS [NOT] NULL, NOT and the LIKE-pattern functions already encode as
// conditions and must not be wrapped again. Used only in predicate position;
// a boolean OPERAND (e.g. bit = bit) is never coerced (issue: bool pushdown).
bool EncodesAsSearchCondition(const Expression &expr) {
	if (BoundComparisonExpression::IsComparison(expr)) {
		return true;
	}
	switch (expr.GetExpressionType()) {
	case ExpressionType::COMPARE_BETWEEN:
	case ExpressionType::COMPARE_IN:
	case ExpressionType::COMPARE_NOT_IN:
	case ExpressionType::OPERATOR_IS_NULL:
	case ExpressionType::OPERATOR_IS_NOT_NULL:
	case ExpressionType::OPERATOR_NOT:
	case ExpressionType::CONJUNCTION_AND:
	case ExpressionType::CONJUNCTION_OR:
		return true;
	default:
		break;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
		IsLikePatternFunction(expr.Cast<BoundFunctionExpression>().Function().GetName().GetIdentifierName())) {
		return true;
	}
	return false;
}
}  // namespace

// Every VALUE position goes through here (job 1191). T-SQL has no boolean value
// type, so a search condition is illegal anywhere a value is expected —
// `CASE … THEN [id] > 5`, `([a] > 1) IS NULL`, `[flag] = ([id] > 5)`,
// `[x] BETWEEN ([a] > 1) AND …`, `LOWER([a] > 1)`. Each produces T-SQL the
// server REJECTS, and because ComplexFilterPushdown erases the expression from
// the DuckDB plan the query FAILS rather than degrading to the client net.
//
// The first fix guarded four such positions individually and left comparison
// operands, BETWEEN and function arguments open — the same bug, three doors
// down. One helper, routed everywhere, so the next value position cannot be
// added without it. Predicate positions use EncodeSearchCondition instead.
static ExpressionEncodeResult EncodeValueExpression(const Expression &expr, const ExpressionEncodeContext &ctx) {
	if (EncodesAsSearchCondition(expr)) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeValueExpression: refusing a search condition in value position");
		return {"", false};
	}
	return FilterEncoder::EncodeExpression(expr, ctx);
}

//------------------------------------------------------------------------------
// Utility Functions
//------------------------------------------------------------------------------

std::string FilterEncoder::EscapeLikePattern(const std::string &pattern) {
	return ExpressionVocabulary::EscapeLikePattern(pattern);
}

bool FilterEncoder::GetComparisonOperator(ExpressionType type, std::string &out_operator) {
	return ExpressionVocabulary::ComparisonOperator(type, out_operator);
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
	return ExpressionVocabulary::ValueToSQLLiteral(value, type);
}

//------------------------------------------------------------------------------
// Spec 076: constants as parameters, declared from the column
//------------------------------------------------------------------------------

std::string FilterEncoder::DeclarationForColumn(const MSSQLColumnInfo &column, const Value &value,
												const LogicalType &type) {
	return ExpressionVocabulary::DeclarationForColumn(column, value, type);
}

std::string FilterEncoder::EncodeConstantValue(const Value &value, const LogicalType &type,
											   const ExpressionEncodeContext &ctx, const MSSQLColumnInfo *peer) {
	const size_t before = ctx.params ? ctx.params->params.size() : 0;
	std::string sql = ExpressionVocabulary::Constant(value, type, ctx.params, peer);
	if (ctx.params && ctx.params->params.size() > before) {
		const auto &param = ctx.params->params.back();
		MSSQL_FILTER_DEBUG_LOG(2, "EncodeConstantValue: @%s %s = %s", param.name.c_str(), param.declaration.c_str(),
							   param.literal.c_str());
	} else {
		MSSQL_FILTER_DEBUG_LOG(2, "EncodeConstantValue: literal %s", sql.c_str());
	}
	return sql;
}

// The SQL Server column an expression IS, when it is a plain reference to
// one: the peer a constant compared with it is declared from.
static const MSSQLColumnInfo *ColumnInfoOf(const Expression &expr_in, const ExpressionEncodeContext &ctx) {
	if (!ctx.mssql_columns) {
		return nullptr;
	}
	// Spec 060 reports MSSQL_VARCHAR(n) / MSSQL_NVARCHAR(n) for string columns
	// and DuckDB reaches the plain VARCHAR operators through an implicit no-op
	// cast over the column: the column under it is still the peer.
	const Expression *expr = &expr_in;
	while (expr->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION && BoundCastExpression::IsCast(*expr)) {
		expr = &BoundCastExpression::Child(expr->Cast<BoundFunctionExpression>());
	}
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_REF) {
		// Inside an EXPRESSION_FILTER the combiner replaced the column reference
		// with BoundReference(0): the filter's own column.
		return ctx.filter_column_info;
	}
	if (expr->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return nullptr;
	}
	const auto &binding = expr->Cast<BoundColumnRefExpression>().Binding();
	column_t table_col_idx;
	if (ctx.column_ids.empty()) {
		table_col_idx = binding.column_index;
	} else if (binding.column_index < ctx.column_ids.size()) {
		table_col_idx = ctx.column_ids[binding.column_index];
	} else {
		return nullptr;
	}
	if (table_col_idx == COLUMN_IDENTIFIER_ROW_ID) {
		// rowid IS the scalar primary-key column (EncodeColumnRef renders it as
		// such), so a constant compared with it is declared from that column --
		// a varchar key stays varchar (review of #345).
		if (ctx.HasPKInfo() && !ctx.pk_is_composite) {
			for (const auto &col : *ctx.mssql_columns) {
				if (col.name == (*ctx.pk_column_names)[0]) {
					return &col;
				}
			}
		}
		return nullptr;
	}
	if (table_col_idx < ctx.mssql_columns->size()) {
		return &(*ctx.mssql_columns)[table_col_idx];
	}
	return nullptr;
}

static const MSSQLColumnInfo *ColumnInfoByName(const std::string &name, const ExpressionEncodeContext &ctx) {
	if (!ctx.mssql_columns) {
		return nullptr;
	}
	for (const auto &col : *ctx.mssql_columns) {
		if (col.name == name) {
			return &col;
		}
	}
	return nullptr;
}

//------------------------------------------------------------------------------
// Main Encode Function
//------------------------------------------------------------------------------

FilterEncoderResult FilterEncoder::Encode(const TableFilterSet *filters, const std::vector<column_t> &column_ids,
										  const std::vector<std::string> &column_names,
										  const std::vector<LogicalType> &column_types) {
	return Encode(filters, column_ids, column_names, column_types, nullptr, nullptr);
}

FilterEncoderResult FilterEncoder::Encode(const TableFilterSet *filters, const std::vector<column_t> &column_ids,
										  const std::vector<std::string> &column_names,
										  const std::vector<LogicalType> &column_types,
										  const std::vector<MSSQLColumnInfo> *mssql_columns,
										  mssql::SqlParamSet *params) {
	FilterEncoderResult result;
	result.needs_duckdb_filter = false;

	if (!filters || !filters->HasFilters()) {
		MSSQL_FILTER_DEBUG_LOG(1, "Encode: no filters to encode");
		return result;
	}

	MSSQL_FILTER_DEBUG_LOG(1, "Encode: encoding %zu filter(s)", static_cast<size_t>(filters->FilterCount()));

	ExpressionEncodeContext ctx(column_ids, column_names, column_types);
	ctx.mssql_columns = mssql_columns;
	ctx.params = params;
	std::vector<std::string> where_conditions;

	// Virtual/special column identifiers start at 2^63
	constexpr column_t VIRTUAL_COL_START = UINT64_C(9223372036854775808);

	for (const auto &filter_entry : *filters) {
		idx_t projected_col_idx = filter_entry.GetIndex();

		// Map from projected column index to actual table column index
		idx_t table_col_idx;
		if (column_ids.empty()) {
			// No projection - use filter index directly as table column index
			table_col_idx = projected_col_idx;
		} else if (projected_col_idx >= column_ids.size()) {
			// Cannot map this filter to a column, so it can be neither pushed
			// to the server nor handed to the client-side net (which needs the
			// column type). 2.0 does not re-apply TableFilters behind a
			// filter_pushdown scan, so skipping here would silently return a
			// superset. Fail instead — a filter over a column outside the
			// projection is a malformed plan, not a pushdown limitation.
			throw InternalException("MSSQL scan: filter column %llu outside projection (%llu columns)",
									(unsigned long long)projected_col_idx, (unsigned long long)column_ids.size());
		} else {
			// Map through column_ids to get actual table column index
			table_col_idx = column_ids[projected_col_idx];
		}

		// Skip virtual/special columns (rowid): not pushable as SQL, but the
		// filter still binds to an output column, so the client net can run it.
		if (table_col_idx >= VIRTUAL_COL_START) {
			MSSQL_FILTER_DEBUG_LOG(2, "  skipping virtual column_id=%llu", (unsigned long long)table_col_idx);
			result.needs_duckdb_filter = true;
			result.unhandled.emplace_back(projected_col_idx, &filter_entry.Filter());
			continue;
		}

		if (table_col_idx >= column_names.size()) {
			// Same reasoning as the projection guard above: unmappable means
			// undroppable, or the scan returns rows the filter excluded.
			throw InternalException("MSSQL scan: filter maps to table column %llu, table has %llu",
									(unsigned long long)table_col_idx, (unsigned long long)column_names.size());
		}

		const std::string &col_name = column_names[table_col_idx];
		const LogicalType &col_type = column_types[table_col_idx];
		std::string escaped_col = mssql::QuoteIdentifier(col_name);
		ctx.filter_column_info =
			(mssql_columns && table_col_idx < mssql_columns->size()) ? &(*mssql_columns)[table_col_idx] : nullptr;

		MSSQL_FILTER_DEBUG_LOG(2, "  encoding filter for column: projected_idx=%llu -> table_idx=%llu -> %s",
							   (unsigned long long)projected_col_idx, (unsigned long long)table_col_idx,
							   col_name.c_str());

		auto encode_result = EncodeFilter(filter_entry.Filter(), escaped_col, col_type, ctx);

		if (encode_result.supported && !encode_result.sql.empty()) {
			where_conditions.push_back(encode_result.sql);
			MSSQL_FILTER_DEBUG_LOG(2, "    encoded: %s", encode_result.sql.c_str());
		}

		if (!encode_result.supported) {
			MSSQL_FILTER_DEBUG_LOG(2, "    filter not fully supported, will need client-side re-filter");
			result.needs_duckdb_filter = true;
			result.unhandled.emplace_back(projected_col_idx, &filter_entry.Filter());
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
	case TableFilterType::LEGACY_CONSTANT_COMPARISON:
		return EncodeConstantComparison(filter.Cast<LegacyConstantFilter>(), ctx, column_name, column_type);

	case TableFilterType::LEGACY_IS_NULL:
		return EncodeIsNull(column_name);

	case TableFilterType::LEGACY_IS_NOT_NULL:
		return EncodeIsNotNull(column_name);

	case TableFilterType::LEGACY_IN_FILTER:
		return EncodeInFilter(filter.Cast<LegacyInFilter>(), column_name, column_type);

	case TableFilterType::LEGACY_CONJUNCTION_OR:
		return EncodeConjunctionOr(filter.Cast<LegacyConjunctionOrFilter>(), column_name, column_type, ctx);

	case TableFilterType::LEGACY_CONJUNCTION_AND:
		return EncodeConjunctionAnd(filter.Cast<LegacyConjunctionAndFilter>(), column_name, column_type, ctx);

	case TableFilterType::EXPRESSION_FILTER: {
		// The 2.0 filter combiner rewrites a pushed predicate's column reference
		// into BoundReference(0) before wrapping it in an ExpressionFilter, so
		// the expression no longer names its column — the filter's slot does.
		// Carry the escaped name in the context for the BOUND_REF arm.
		ExpressionEncodeContext expr_ctx = ctx;
		expr_ctx.filter_column = &column_name;
		return EncodeExpressionFilter(filter.Cast<ExpressionFilter>(), expr_ctx);
	}

	case TableFilterType::LEGACY_OPTIONAL_FILTER:
	case TableFilterType::LEGACY_STRUCT_EXTRACT:
	case TableFilterType::LEGACY_DYNAMIC_FILTER:
	default:
		// These filter types cannot be pushed down to SQL Server
		MSSQL_FILTER_DEBUG_LOG(1, "Filter type %d cannot be pushed down", (int)filter.filter_type);
		return {"", false};
	}
}

ExpressionEncodeResult FilterEncoder::EncodeConstantComparison(const LegacyConstantFilter &filter,
															   const ExpressionEncodeContext &ctx,
															   const std::string &column_name,
															   const LogicalType &column_type) {
	std::string op;
	if (!GetComparisonOperator(filter.comparison_type, op)) {
		return {"", false};
	}

	std::string sql = column_name + op + EncodeConstantValue(filter.constant, column_type, ctx, ctx.filter_column_info);
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeIsNull(const std::string &column_name) {
	return {column_name + " IS NULL", true};
}

ExpressionEncodeResult FilterEncoder::EncodeIsNotNull(const std::string &column_name) {
	return {column_name + " IS NOT NULL", true};
}

ExpressionEncodeResult FilterEncoder::EncodeInFilter(const LegacyInFilter &filter, const std::string &column_name,
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

ExpressionEncodeResult FilterEncoder::EncodeConjunctionAnd(const LegacyConjunctionAndFilter &filter,
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

	return {ExpressionVocabulary::Conjunction(conditions, true), all_supported};
}

ExpressionEncodeResult FilterEncoder::EncodeConjunctionOr(const LegacyConjunctionOrFilter &filter,
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

	return {ExpressionVocabulary::Conjunction(conditions, false), true};
}

ExpressionEncodeResult FilterEncoder::EncodeExpressionFilter(const ExpressionFilter &filter,
															 const ExpressionEncodeContext &ctx) {
	// Expression filters contain arbitrary expressions. This is predicate
	// position (the expression IS the WHERE clause), so a bare bit value must be
	// coerced to `= 1`.
	MSSQL_FILTER_DEBUG_LOG(1, "EncodeExpressionFilter: encoding expression type %d",
						   (int)filter.expr->GetExpressionType());
	return EncodeSearchCondition(*filter.expr, ctx);
}

ExpressionEncodeResult FilterEncoder::EncodeSearchCondition(const Expression &expr,
															const ExpressionEncodeContext &ctx) {
	auto result = EncodeExpression(expr, ctx);
	// A BOOLEAN value (bit column / constant / cast) is not a T-SQL predicate:
	// `WHERE [b]` errors 4145, `WHERE [b] = 1` is the condition. Expressions that
	// already encode AS a condition (comparison/conjunction/BETWEEN/IN/IS NULL/
	// NOT/LIKE) are left alone. Boolean operands elsewhere (bit = bit) go through
	// EncodeExpression, never here, so they keep their bare form.
	if (result.supported) {
		SqlOperand operand;
		operand.sql = result.sql;
		operand.is_condition = EncodesAsSearchCondition(expr);
		operand.type = expr.GetReturnType();
		result.sql = ExpressionVocabulary::AsCondition(operand);
	}
	return result;
}

//------------------------------------------------------------------------------
// Expression Encoding
//------------------------------------------------------------------------------

ExpressionEncodeResult FilterEncoder::EncodeExpression(const Expression &expr, const ExpressionEncodeContext &ctx) {
	// Spec 076: constants register themselves in the sink as they are met, so
	// an expression refused halfway (a comparison whose other side does not
	// encode) would leave its parameters in the DECLARE line -- unused, and a
	// different text per value for nothing. Roll them back on refusal.
	const size_t mark = ctx.params ? ctx.params->params.size() : 0;
	auto result = EncodeExpressionImpl(expr, ctx);
	if (!result.supported && ctx.params && ctx.params->params.size() > mark) {
		ctx.params->params.resize(mark);
	}
	return result;
}

ExpressionEncodeResult FilterEncoder::EncodeExpressionImpl(const Expression &expr, const ExpressionEncodeContext &ctx) {
	// Check recursion depth
	if (ctx.at_max_depth()) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeExpression: max depth reached");
		return {"", false};
	}

	MSSQL_FILTER_DEBUG_LOG(2, "EncodeExpression: type=%d class=%d", (int)expr.GetExpressionType(),
						   (int)expr.GetExpressionClass());

	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF:
		return EncodeColumnRef(expr.Cast<BoundColumnRefExpression>(), ctx);

	case ExpressionClass::BOUND_REF: {
		// Only inside an EXPRESSION_FILTER, where the combiner replaced the
		// filter column's reference with BoundReference(0) — the combiner
		// rejects multi-column expressions before pushing, so index 0 is the
		// filter's own column and anything else is unencodable.
		auto &ref = expr.Cast<BoundReferenceExpression>();
		if (ctx.filter_column && ref.Index() == 0) {
			return {*ctx.filter_column, true};
		}
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeExpression: BOUND_REF outside a filter context, not pushed");
		return {"", false};
	}

	case ExpressionClass::BOUND_CONSTANT:
		return EncodeConstant(expr.Cast<BoundConstantExpression>(), ctx);

	case ExpressionClass::BOUND_FUNCTION: {
		auto &func_expr = expr.Cast<BoundFunctionExpression>();
		if (BoundCastExpression::IsCast(expr)) {
			// Spec 060: the catalog reports MSSQL_VARCHAR(n) / MSSQL_NVARCHAR(n)
			// for string columns, and DuckDB reaches a plain VARCHAR overload
			// through the implicit no-op cast registered with those types. On the
			// server there is nothing to cast — the column already IS that string
			// — so encode straight through it. Without this arm the cast reads as
			// an unsupported expression, the filter stops being pushed, and the
			// whole column is fetched and filtered on the client instead.
			//
			// VARCHAR to VARCHAR only. A real conversion (INTEGER to VARCHAR) has
			// formatting semantics SQL Server need not reproduce, and stays
			// unpushed.
			const auto &target_type = BoundCastExpression::TargetType(func_expr);
			const auto &cast_child = BoundCastExpression::Child(func_expr);
			if (ExpressionVocabulary::IsTransparentCast(cast_child.GetReturnType(), target_type)) {
				return EncodeExpression(cast_child, ctx);
			}
			// A temporal cast is NOT stripped here. It is a free view only when the
			// surrounding operation is precision-insensitive (a date-part
			// extraction), and only for a naive-timestamp precision change — never
			// in a comparison, where dt::DATE / dt::TIME / dt::TIMESTAMPTZ change
			// which rows match. That narrow, provably-exact strip lives in
			// EncodeFunctionExpression for the date-part functions (spec 070 W1);
			// every other temporal cast falls through to the client filter net.
			MSSQL_FILTER_DEBUG_LOG(1, "EncodeExpression: cast %s -> %s not pushed",
								   cast_child.GetReturnType().ToString().c_str(), target_type.ToString().c_str());
			return {"", false};
		}
		// Comparisons and BETWEEN are BoundFunctionExpressions too (their
		// helper structs only accept that class), dispatched by expression
		// type, not class.
		if (BoundComparisonExpression::IsComparison(expr)) {
			return EncodeComparisonExpression(func_expr, ctx);
		}
		if (expr.GetExpressionType() == ExpressionType::COMPARE_BETWEEN) {
			return EncodeBetweenExpression(func_expr, ctx);
		}
		return EncodeFunctionExpression(func_expr, ctx);
	}

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
	const std::string &func_name = expr.Function().GetName().GetIdentifierName();
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeFunctionExpression: function=%s, args=%zu", func_name.c_str(),
						   expr.GetChildren().size());

	// Check for LIKE pattern functions (prefix, suffix, contains)
	if (IsLikePatternFunction(func_name)) {
		if (expr.GetChildren().size() >= 2) {
			return EncodeLikePattern(func_name, *expr.GetChildren()[0], *expr.GetChildren()[1], ctx);
		}
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeFunctionExpression: LIKE pattern function %s needs 2 args, got %zu",
							   func_name.c_str(), expr.GetChildren().size());
		return {"", false};
	}

	std::vector<LogicalType> arg_types;
	for (const auto &child : expr.GetChildren()) {
		arg_types.push_back(child->GetReturnType());
	}
	std::string why;
	const FunctionMapping *mapping =
		ExpressionVocabulary::FunctionFor(func_name, arg_types, why, ctx.division_by_zero_errors);
	if (!mapping) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeFunctionExpression: %s", why.c_str());
		return {"", false};
	}

	// Encode all arguments
	auto child_ctx = ctx.child();
	const bool datepart = ExpressionVocabulary::IsDatePartFunction(func_name);
	std::vector<std::string> encoded_args;
	for (const auto &child : expr.GetChildren()) {
		const Expression *arg = child.get();
		// Spec 070 W1: strip the implicit precision cast DuckDB puts over a
		// DATETIME2 column when a date-part function takes it (year() takes
		// TIMESTAMP, the column is TIMESTAMP_NS). SQL Server's YEAR([col])
		// applies to the column's own DATETIME2 and the extraction is invariant
		// to the sub-second precision the cast changed, so encoding the cast's
		// child directly is exact. The ONLY place a temporal cast is stripped.
		if (datepart && BoundCastExpression::IsCast(*arg)) {
			const auto &cast_fn = arg->Cast<BoundFunctionExpression>();
			const auto &cast_child = BoundCastExpression::Child(cast_fn);
			if (ExpressionVocabulary::IsNaiveTimestampCast(cast_child.GetReturnType(),
														   BoundCastExpression::TargetType(cast_fn))) {
				arg = &cast_child;
			}
		}
		auto result = EncodeValueExpression(*arg, child_ctx);
		if (!result.supported) {
			MSSQL_FILTER_DEBUG_LOG(1, "EncodeFunctionExpression: argument encoding failed for %s", func_name.c_str());
			return {"", false};
		}
		encoded_args.push_back(result.sql);
	}

	std::string sql = ExpressionVocabulary::ApplyFunction(*mapping, encoded_args);
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeFunctionExpression: encoded %s -> %s", func_name.c_str(), sql.c_str());
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeComparisonExpression(const BoundFunctionExpression &expr,
																 const ExpressionEncodeContext &ctx) {
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeComparisonExpression: type=%d", (int)expr.GetExpressionType());

	const auto &left = BoundComparisonExpression::Left(expr);
	const auto &right = BoundComparisonExpression::Right(expr);

	// Check for rowid equality: rowid = value (Spec 001-pk-rowid-semantics)
	if (expr.GetExpressionType() == ExpressionType::COMPARE_EQUAL && ctx.HasPKInfo()) {
		// Check if left is rowid and right is constant
		if (IsRowidColumn(left, ctx)) {
			MSSQL_FILTER_DEBUG_LOG(2, "EncodeComparisonExpression: detected rowid = value");
			return EncodeRowidEquality(right, ctx);
		}
		// Check if right is rowid and left is constant (value = rowid)
		if (IsRowidColumn(right, ctx)) {
			MSSQL_FILTER_DEBUG_LOG(2, "EncodeComparisonExpression: detected value = rowid");
			return EncodeRowidEquality(left, ctx);
		}
	}

	// Get the comparison operator
	std::string op;
	if (!GetComparisonOperator(expr.GetExpressionType(), op)) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeComparisonExpression: unsupported comparison type %d",
							   (int)expr.GetExpressionType());
		return {"", false};
	}

	// Encode left and right sides
	// Spec 076: a constant on one side is declared from the column on the other.
	auto left_ctx = ctx.child();
	left_ctx.constant_peer = ColumnInfoOf(right, ctx);
	auto right_ctx = ctx.child();
	right_ctx.constant_peer = ColumnInfoOf(left, ctx);
	auto left_result = EncodeValueExpression(left, left_ctx);
	if (!left_result.supported) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeComparisonExpression: left side encoding failed");
		return {"", false};
	}

	auto right_result = EncodeValueExpression(right, right_ctx);
	if (!right_result.supported) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeComparisonExpression: right side encoding failed");
		return {"", false};
	}

	std::string sql = ExpressionVocabulary::Comparison(op, left_result.sql, right_result.sql);
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeComparisonExpression: encoded -> %s", sql.c_str());
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeOperatorExpression(const BoundOperatorExpression &expr,
															   const ExpressionEncodeContext &ctx) {
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeOperatorExpression: type=%d, children=%zu", (int)expr.GetExpressionType(),
						   expr.GetChildren().size());

	// Handle NOT operator
	if (expr.GetExpressionType() == ExpressionType::OPERATOR_NOT) {
		if (expr.GetChildren().size() != 1) {
			return {"", false};
		}
		auto child_ctx = ctx.child();
		// NOT negates a predicate: its operand is itself predicate position, so a
		// bare bit child (`NOT flag`) becomes `NOT ([flag] = 1)`, not `NOT [flag]`.
		auto child_result = EncodeSearchCondition(*expr.GetChildren()[0], child_ctx);
		if (!child_result.supported) {
			return {"", false};
		}
		return {ExpressionVocabulary::Not(child_result.sql), true};
	}

	// Handle IS NULL / IS NOT NULL operators
	if (expr.GetExpressionType() == ExpressionType::OPERATOR_IS_NULL) {
		if (expr.GetChildren().size() != 1) {
			return {"", false};
		}
		auto child_ctx = ctx.child();
		auto child_result = EncodeValueExpression(*expr.GetChildren()[0], child_ctx);
		if (!child_result.supported) {
			return {"", false};
		}
		return {ExpressionVocabulary::IsNull(child_result.sql, false), true};
	}

	if (expr.GetExpressionType() == ExpressionType::OPERATOR_IS_NOT_NULL) {
		if (expr.GetChildren().size() != 1) {
			return {"", false};
		}
		auto child_ctx = ctx.child();
		auto child_result = EncodeValueExpression(*expr.GetChildren()[0], child_ctx);
		if (!child_result.supported) {
			return {"", false};
		}
		return {ExpressionVocabulary::IsNull(child_result.sql, true), true};
	}

	// IN arrives here as an operator expression whenever the filter combiner
	// did not turn it into a table filter: always for NOT IN, which DuckDB
	// binds as NOT over COMPARE_IN (the NOT case above wraps this one; there
	// is no NotInFilter, issue #366), and for IN whose operand is not a bare
	// column. COMPARE_NOT_IN is rendered too, should a plan ever carry it.
	// Children: [0] the operand, [1..] the list. The list's constants are
	// declared from the operand's column exactly as a comparison's constant is
	// (spec 076), so a varchar list keeps the seek. A NULL in the list gives
	// the same three-valued answer on both sides (no row for NOT IN), so
	// nothing is special-cased. Without this case NOT IN was the one string
	// predicate evaluated client-side, under DuckDB's equality, while <>,
	// NOT LIKE and the ranges beside it were the server's (spec 079 D4).
	if (expr.GetExpressionType() == ExpressionType::COMPARE_IN ||
		expr.GetExpressionType() == ExpressionType::COMPARE_NOT_IN) {
		const auto &children = expr.GetChildren();
		if (children.size() < 2) {
			return {"", false};
		}
		// A long list stays client-side, as it always was (#367 review): every
		// item is one sp_executesql parameter out of the statement's 2000, or
		// one literal of an unbounded batch with parameterisation off, and a
		// list of thousands is where the server answers 8623/8632 instead of
		// rows. 256 covers the hand-written and the tool-generated lists;
		// above it the whole predicate is refused and DuckDB evaluates it. The
		// cap is per predicate: several lists in one WHERE each get it, and
		// their total is bounded by SqlParamSet::MAX_PARAMS when parameterised
		// (the sink turns the rest into literals) and by their count when not.
		// A bare-column IN is a table filter (EncodeInFilter) and is not capped.
		if (!ExpressionVocabulary::InListFits(children.size() - 1)) {
			MSSQL_FILTER_DEBUG_LOG(1, "EncodeOperatorExpression: IN list of %llu items exceeds %llu, left to DuckDB",
								   (unsigned long long)(children.size() - 1),
								   (unsigned long long)ExpressionVocabulary::MAX_IN_ITEMS);
			return {"", false};
		}
		auto operand_ctx = ctx.child();
		auto operand_result = EncodeValueExpression(*children[0], operand_ctx);
		if (!operand_result.supported) {
			MSSQL_FILTER_DEBUG_LOG(1, "EncodeOperatorExpression: IN operand encoding failed");
			return {"", false};
		}
		auto item_ctx = ctx.child();
		item_ctx.constant_peer = ColumnInfoOf(*children[0], ctx);
		const bool negated = expr.GetExpressionType() == ExpressionType::COMPARE_NOT_IN;
		std::vector<std::string> items;
		for (idx_t i = 1; i < children.size(); i++) {
			auto item_result = EncodeValueExpression(*children[i], item_ctx);
			if (!item_result.supported) {
				MSSQL_FILTER_DEBUG_LOG(1, "EncodeOperatorExpression: IN list item %llu encoding failed",
									   (unsigned long long)i);
				return {"", false};
			}
			items.push_back(item_result.sql);
		}
		std::string sql = ExpressionVocabulary::In(operand_result.sql, items, negated);
		MSSQL_FILTER_DEBUG_LOG(2, "EncodeOperatorExpression: encoded -> %s", sql.c_str());
		return {sql, true};
	}

	// For other operators, we don't support them yet
	// Arithmetic is handled via BoundFunctionExpression in DuckDB stable
	MSSQL_FILTER_DEBUG_LOG(1, "EncodeOperatorExpression: unsupported operator type %d", (int)expr.GetExpressionType());
	return {"", false};
}

ExpressionEncodeResult FilterEncoder::EncodeCaseExpression(const BoundCaseExpression &expr,
														   const ExpressionEncodeContext &ctx) {
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeCaseExpression: case_checks=%zu", expr.CaseChecks().size());

	auto child_ctx = ctx.child();
	std::vector<std::string> whens;
	std::vector<std::string> thens;

	// Encode each WHEN ... THEN clause
	for (const auto &check : expr.CaseChecks()) {
		// A CASE `WHEN` operand is predicate position like any other: a bare bit
		// there emits `CASE WHEN [flag] THEN ...`, which SQL Server rejects with
		// error 4145. Coerce it to `([flag] = 1)` (PR #269 review).
		auto when_result = EncodeSearchCondition(*check.when_expr, child_ctx);
		if (!when_result.supported) {
			MSSQL_FILTER_DEBUG_LOG(1, "EncodeCaseExpression: WHEN clause encoding failed");
			return {"", false};
		}

		// THEN is VALUE position — the mirror of the WHEN case above. T-SQL has no
		// boolean value type, so a search condition is illegal here:
		// `CASE WHEN c THEN [id] > 5 ...` is "Incorrect syntax near '>'". And the
		// failure is the bad kind — ComplexFilterPushdown erases the expression
		// from the plan, so the query FAILS instead of falling to the client net
		// (job 1113).
		auto then_result = EncodeValueExpression(*check.then_expr, child_ctx);
		if (!then_result.supported) {
			MSSQL_FILTER_DEBUG_LOG(1, "EncodeCaseExpression: THEN clause encoding failed");
			return {"", false};
		}

		whens.push_back(when_result.sql);
		thens.push_back(then_result.sql);
	}

	// ELSE is VALUE position too — same refusal as THEN.
	auto else_result = EncodeValueExpression(expr.Else(), child_ctx);
	if (!else_result.supported) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeCaseExpression: ELSE clause encoding failed");
		return {"", false};
	}
	std::string sql = ExpressionVocabulary::Case(whens, thens, else_result.sql);

	MSSQL_FILTER_DEBUG_LOG(2, "EncodeCaseExpression: encoded -> %s", sql.c_str());
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeBetweenExpression(const BoundFunctionExpression &expr,
															  const ExpressionEncodeContext &ctx) {
	const bool lower_inclusive = BoundBetweenExpression::LowerInclusive(expr);
	const bool upper_inclusive = BoundBetweenExpression::UpperInclusive(expr);
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeBetweenExpression: lower_inclusive=%s, upper_inclusive=%s",
						   lower_inclusive ? "true" : "false", upper_inclusive ? "true" : "false");

	auto child_ctx = ctx.child();

	// Encode the input expression (the column or expression being checked)
	auto input_result = EncodeValueExpression(BoundBetweenExpression::Input(expr), child_ctx);
	if (!input_result.supported) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeBetweenExpression: input encoding failed");
		return {"", false};
	}

	// Encode the lower bound
	// Spec 076: the bounds are compared with the input column.
	child_ctx.constant_peer = ColumnInfoOf(BoundBetweenExpression::Input(expr), ctx);
	auto lower_result = EncodeValueExpression(BoundBetweenExpression::LowerBound(expr), child_ctx);
	if (!lower_result.supported) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeBetweenExpression: lower bound encoding failed");
		return {"", false};
	}

	// Encode the upper bound
	auto upper_result = EncodeValueExpression(BoundBetweenExpression::UpperBound(expr), child_ctx);
	if (!upper_result.supported) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeBetweenExpression: upper bound encoding failed");
		return {"", false};
	}

	std::string sql = ExpressionVocabulary::Between(input_result.sql, lower_result.sql, upper_result.sql,
													lower_inclusive, upper_inclusive);
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeBetweenExpression: encoded -> %s", sql.c_str());
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeColumnRef(const BoundColumnRefExpression &expr,
													  const ExpressionEncodeContext &ctx) {
	// Get the column binding - this contains the table index and column index
	const auto &binding = expr.Binding();
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeColumnRef: table_idx=%llu, column_idx=%llu",
						   (unsigned long long)binding.table_index.index, (unsigned long long)binding.column_index);

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
			std::string sql = mssql::QuoteIdentifier((*ctx.pk_column_names)[0]);
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
	std::string sql = mssql::QuoteIdentifier(col_name);
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeColumnRef: encoded -> %s", sql.c_str());
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeConstant(const BoundConstantExpression &expr,
													 const ExpressionEncodeContext &ctx) {
	std::string sql = EncodeConstantValue(expr.GetValue(), expr.GetReturnType(), ctx, ctx.constant_peer);
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeConstant: value=%s, type=%s -> %s", expr.GetValue().ToString().c_str(),
						   expr.GetReturnType().ToString().c_str(), sql.c_str());
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeConjunctionExpression(const BoundConjunctionExpression &expr,
																  const ExpressionEncodeContext &ctx) {
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeConjunctionExpression: type=%d, children=%zu", (int)expr.GetExpressionType(),
						   expr.GetChildren().size());

	if (expr.GetChildren().empty()) {
		return {"", false};
	}

	bool is_and = (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND);

	auto child_ctx = ctx.child();
	std::vector<std::string> conditions;
	bool all_supported = true;

	for (const auto &child : expr.GetChildren()) {
		// Each conjunct is predicate position — a bare bit child needs `= 1`.
		auto result = EncodeSearchCondition(*child, child_ctx);
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

	std::string sql = ExpressionVocabulary::Conjunction(conditions, is_and);
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeConjunctionExpression: encoded -> %s", sql.c_str());
	return {sql, all_supported};
}

ExpressionEncodeResult FilterEncoder::EncodeLikePattern(const std::string &function_name, const Expression &column_expr,
														const Expression &pattern_expr,
														const ExpressionEncodeContext &ctx) {
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeLikePattern: function=%s", function_name.c_str());

	// Encode the column expression
	auto child_ctx = ctx.child();
	// Value position like every other (job 1203) — this was the one site the
	// helper did not cover, which contradicted its own stated invariant. No live
	// bug today (reaching it with a condition needs a BOOLEAN->VARCHAR cast, which
	// the cast arm refuses), but it is the same "three doors down" shape.
	auto column_result = EncodeValueExpression(column_expr, child_ctx);
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
	if (pattern_const.GetValue().IsNull()) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeLikePattern: pattern is NULL");
		return {"", false};
	}

	std::string pattern_text;
	if (!ExpressionVocabulary::LikePatternText(function_name, pattern_const.GetValue().ToString(), pattern_text)) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeLikePattern: unknown LIKE pattern function %s", function_name.c_str());
		return {"", false};
	}
	// Spec 076: the pattern is compared with the column, so it is declared from
	// it (a varchar column keeps a seekable prefix pattern); without a sink this
	// is the N'...' literal it always was.
	std::string like_pattern =
		EncodeConstantValue(Value(pattern_text), LogicalType::VARCHAR, child_ctx, ColumnInfoOf(column_expr, ctx));
	std::string sql = ExpressionVocabulary::Like(column_result.sql, like_pattern);

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
	idx_t projected_idx = col_ref.Binding().column_index;

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
		if (const_expr.GetValue().type().id() != LogicalTypeId::STRUCT) {
			MSSQL_FILTER_DEBUG_LOG(1, "EncodeRowidEquality: composite PK expects STRUCT value, got %s",
								   const_expr.GetValue().type().ToString().c_str());
			return {"", false};
		}
		auto &children = StructValue::GetChildren(const_expr.GetValue());
		if (children.size() != ctx.pk_column_names->size()) {
			MSSQL_FILTER_DEBUG_LOG(1, "EncodeRowidEquality: STRUCT has %zu children, expected %zu", children.size(),
								   ctx.pk_column_names->size());
			return {"", false};
		}

		std::string sql = "(";
		for (idx_t i = 0; i < children.size(); i++) {
			if (i > 0) {
				sql += " AND ";
			}
			sql += mssql::QuoteIdentifier((*ctx.pk_column_names)[i]);
			sql += " = ";
			sql += EncodeConstantValue(children[i], (*ctx.pk_column_types)[i], ctx,
									   ColumnInfoByName((*ctx.pk_column_names)[i], ctx));
		}
		sql += ")";
		MSSQL_FILTER_DEBUG_LOG(2, "EncodeRowidEquality: composite PK -> %s", sql.c_str());
		return {sql, true};
	} else {
		// Scalar PK: rowid = value
		std::string sql = mssql::QuoteIdentifier((*ctx.pk_column_names)[0]);
		sql += " = ";
		sql += EncodeConstantValue(const_expr.GetValue(), (*ctx.pk_column_types)[0], ctx,
								   ColumnInfoByName((*ctx.pk_column_names)[0], ctx));
		MSSQL_FILTER_DEBUG_LOG(2, "EncodeRowidEquality: scalar PK -> %s", sql.c_str());
		return {sql, true};
	}
}

}  // namespace mssql
}  // namespace duckdb
