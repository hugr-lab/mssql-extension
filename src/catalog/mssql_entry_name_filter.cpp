#include "catalog/mssql_entry_name_filter.hpp"

#include "duckdb/common/types.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/table_filter.hpp"

namespace duckdb {
namespace mssql {

static bool IsColumn(const Expression &expr) {
	const auto cls = expr.GetExpressionClass();
	return cls == ExpressionClass::BOUND_REF || cls == ExpressionClass::BOUND_COLUMN_REF;
}

static bool AddConstant(const Expression &expr, unordered_set<string> &out) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	const auto &value = expr.Cast<BoundConstantExpression>().GetValue();
	if (value.IsNull() || value.type().id() != LogicalTypeId::VARCHAR) {
		return false;
	}
	out.insert(value.ToString());
	return true;
}

static bool Collect(const Expression &expr, unordered_set<string> &out) {
	// DuckDB pushes an IN or an OR as an optional filter and applies the predicate again above
	// the scan, so the names under the wrapper still bound what can come out.
	if (ExpressionFilter::IsRootOptionalExpression(expr)) {
		auto child = ExpressionFilter::GetOptionalFilterChild(expr);
		return child && Collect(*child, out);
	}
	if (BoundComparisonExpression::IsComparison(expr)) {
		if (expr.GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
			return false;
		}
		const auto &comparison = expr.Cast<BoundFunctionExpression>();
		const auto &left = BoundComparisonExpression::Left(comparison);
		const auto &right = BoundComparisonExpression::Right(comparison);
		if (IsColumn(left)) {
			return AddConstant(right, out);
		}
		return IsColumn(right) && AddConstant(left, out);
	}
	switch (expr.GetExpressionType()) {
	case ExpressionType::COMPARE_IN: {
		const auto &children = expr.Cast<BoundOperatorExpression>().GetChildren();
		if (children.empty() || !IsColumn(*children[0])) {
			return false;
		}
		for (idx_t i = 1; i < children.size(); i++) {
			if (!AddConstant(*children[i], out)) {
				return false;
			}
		}
		return true;
	}
	case ExpressionType::CONJUNCTION_OR: {
		// Every branch must name its tables, or one of them could match anything.
		for (auto &child : expr.Cast<BoundConjunctionExpression>().GetChildren()) {
			if (!Collect(*child, out)) {
				return false;
			}
		}
		return true;
	}
	case ExpressionType::CONJUNCTION_AND: {
		// One branch that names its tables bounds the AND; the others are re-checked by DuckDB.
		for (auto &child : expr.Cast<BoundConjunctionExpression>().GetChildren()) {
			unordered_set<string> branch;
			if (Collect(*child, branch)) {
				out.insert(branch.begin(), branch.end());
				return true;
			}
		}
		return false;
	}
	default:
		return false;
	}
}

bool EntryNameCandidates(optional_ptr<const TableFilter> filter, const LogicalType &column_type,
						 vector<string> &names) {
	if (!filter) {
		return false;
	}
	// Every legacy form (constant comparison, IN, conjunction) as one expression.
	auto as_expression = ExpressionFilter::FromTableFilter(*filter, column_type);
	if (!as_expression || !as_expression->expr) {
		return false;
	}
	unordered_set<string> found;
	if (!Collect(*as_expression->expr, found) || found.size() > MAX_LOOKED_UP_NAMES) {
		return false;
	}
	names.assign(found.begin(), found.end());
	return true;
}

}  // namespace mssql
}  // namespace duckdb
