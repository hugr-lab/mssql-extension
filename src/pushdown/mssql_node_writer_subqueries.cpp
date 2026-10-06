#include "pushdown/mssql_node_writer.hpp"
#include "pushdown/mssql_sql_writer.hpp"

#include "catalog/mssql_column_info.hpp"
#include "codec/target_string_type.hpp"
#include "pushdown/mssql_expression_vocabulary.hpp"
#include "pushdown/mssql_order_term.hpp"
#include "query/mssql_identifier.hpp"
#include "query/mssql_sql_params.hpp"

#include <deque>

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/parser/expression/between_expression.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/expression/type_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

namespace duckdb {
namespace mssql {
namespace node_writer {

bool NodeWriter::KeyedToOneRow(const SelectNode &node) const {
	if (relations_.size() != 1 || relations_[0].derived || relations_[0].table.unique_key.empty() ||
		!node.where_clause || distinct_ || aggregated_) {
		return false;
	}
	std::vector<const ParsedExpression *> conjuncts;
	std::function<void(const ParsedExpression &)> collect = [&](const ParsedExpression &expr) {
		if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
			for (auto &child : expr.Cast<ConjunctionExpression>().GetChildren()) {
				collect(*child);
			}
			return;
		}
		conjuncts.push_back(&expr);
	};
	collect(*node.where_clause);
	// A side that is fixed for one outer row: a constant, or a column this
	// node does not have (an outer one -- the write resolved it). A string key
	// only against a constant: an outer column of another server type (varchar
	// against nvarchar) compares under other rules, where two keys can meet
	// one value.
	auto fixed = [&](const ParsedExpression &side, idx_t key_index) {
		if (side.GetExpressionClass() == ExpressionClass::CONSTANT) {
			return true;
		}
		idx_t unused;
		return KindOf(columns_[key_index]) != ComparableKind::String &&
			   side.GetExpressionClass() == ExpressionClass::COLUMN_REF &&
			   FindColumn(side.Cast<ColumnRefExpression>(), unused) != 1;
	};
	for (auto &key_column : relations_[0].table.unique_key) {
		bool equated = false;
		for (auto conjunct : conjuncts) {
			if (conjunct->GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
				continue;
			}
			auto &cmp = conjunct->Cast<ComparisonExpression>();
			for (auto pair : {std::make_pair(&cmp.Left(), &cmp.Right()), std::make_pair(&cmp.Right(), &cmp.Left())}) {
				idx_t index;
				equated =
					equated || (pair.first->GetExpressionClass() == ExpressionClass::COLUMN_REF &&
								FindColumn(pair.first->Cast<ColumnRefExpression>(), index) == 1 &&
								StringUtil::CIEquals(columns_[index].name, key_column) && fixed(*pair.second, index));
			}
		}
		if (!equated) {
			return false;
		}
	}
	return true;
}

bool NodeWriter::MayBeOuter(const ColumnRefExpression &ref, int found) const {
	auto &names = ref.ColumnNames();
	if (names.size() == 1) {
		// A name a column here answers, or two do (ambiguous), is this node's.
		return found == 0;
	}
	if (names.size() != 2) {
		return false;
	}
	for (auto &relation : relations_) {
		if (StringUtil::CIEquals(names[0].GetIdentifierName(), relation.name)) {
			return false;
		}
	}
	return true;
}

bool NodeWriter::WriteOuterColumn(const ColumnRefExpression &ref, Operand &out, bool &found) {
	found = false;
	for (auto outer = outer_; outer; outer = outer->outer_) {
		idx_t index;
		const int result = outer->FindColumn(ref, index);
		if (result != 1) {
			if (outer->MayBeOuter(ref, result)) {
				continue;
			}
			// That node's own reference, ambiguous or unknown there: its veto.
			return outer->ResolveColumn(ref, index);
		}
		if (in_aggregate_) {
			// DuckDB makes an aggregate over outer columns the outer node's.
			return Veto("a correlated column in a subquery's aggregate " + ref.ToString());
		}
		if (!outer->CheckGrouped(index)) {
			return false;
		}
		if (outer->divergent_of_[index]) {
			return Veto("a derived division / floating-point aggregate in a subquery");
		}
		for (auto writer = this; writer != outer; writer = writer->outer_) {
			writer->correlated_ = true;
		}
		const auto &column = outer->columns_[index];
		out.sql = outer->ColumnSql(index);
		out.type = column.duckdb_type;
		out.kind = KindOf(column);
		out.column = &column;
		out.result_type = outer->cast_of_[index];
		out.cast_result = outer->cast_of_[index].id() != LogicalTypeId::INVALID;
		found = true;
		return true;
	}
	// No node around this one has it: a node further out, beyond the one the
	// rewriter asked about on its own (a correlation that skips a level).
	out_.refers_outside = true;
	return true;
}

bool NodeWriter::WriteSubqueryNode(const SubqueryExpression &subquery, NodeWriter &inner, idx_t columns) {
	if (in_aggregate_) {
		// T-SQL refuses a subquery in an aggregate's argument (130).
		return Veto("a subquery in an aggregate");
	}
	auto &node = *subquery.Subquery()->node;
	bool ordered = false;
	bool limited = false;
	for (auto &modifier : node.modifiers) {
		ordered = ordered || modifier->type == ResultModifierType::ORDER_MODIFIER;
		limited = limited || modifier->type == ResultModifierType::LIMIT_MODIFIER;
	}
	if (ordered && !limited) {
		// 1033 there, as in a derived table.
		return Veto("an ORDER BY without LIMIT in a subquery");
	}
	if (!inner.WriteQueryNode(node)) {
		out_.refers_outside = out_.refers_outside || inner.out_.refers_outside;
		return false;
	}
	if (columns > 0 && inner.Outputs().size() != columns) {
		return Veto("a subquery of " + std::to_string(inner.Outputs().size()) + " columns");
	}
	return true;
}

bool NodeWriter::SubqueryOperand(const NodeWriter &inner, const WrittenQuery &inner_out, const std::string &sql,
								 Operand &out) {
	auto &output = inner.Outputs()[0];
	out = Operand();
	if (output.column_index != DConstants::INVALID_INDEX) {
		if (!output.plain_read) {
			// Read through a CAST / STAsBinary: not the column it compares as.
			return Veto("a subquery's result read through a conversion");
		}
		subquery_columns_.push_back(inner.FlatColumn(output.column_index));
		auto &column = subquery_columns_.back();
		// NULL when the subquery finds no row.
		column.is_nullable = true;
		// A derived table's division / floating-point aggregate stays one.
		out.division = inner.divergent_of_[output.column_index];
		out.approximate = inner.divergent_of_[output.column_index];
		if (out.approximate && predicate_depth_ > 0) {
			return Veto("a subquery's division / floating-point aggregate in a condition");
		}
		out.sql = sql;
		out.type = column.duckdb_type;
		out.kind = KindOf(column);
		out.column = &column;
	} else {
		out = output.value;
		out.sql = sql;
		// A string value has no column to carry its collation.
		if (out.kind == ComparableKind::String) {
			return Veto("a subquery's computed string result");
		}
		if ((out.division || out.approximate) && predicate_depth_ > 0) {
			return Veto("a subquery's division / floating-point aggregate in a condition");
		}
		out.column = nullptr;
		out.constant = nullptr;
		// DuckDB moves no constant across a subquery.
		out.moves_constant = false;
		out.orderable = false;
		out.not_null = false;
	}
	const auto &cast = inner_out.cast_types[0];
	out.cast_result = cast.id() != LogicalTypeId::INVALID;
	out.result_type = out.cast_result ? cast : inner_out.column_types[0];
	return true;
}

bool NodeWriter::WriteScalarSubquery(const SubqueryExpression &subquery, Operand &out) {
	if (subquery.GetSubqueryType() != SubqueryType::SCALAR) {
		return Veto("an EXISTS / IN subquery as a value");
	}
	if (!options_.scalar_subquery_errors) {
		// DuckDB would return one of several rows; the server errors (512).
		return Veto("a scalar subquery under scalar_subquery_error_on_multiple_rows = false");
	}
	WrittenQuery inner_out;
	NodeWriter inner(*this, inner_out, Correlated());
	inner.in_expression_ = true;
	if (!WriteSubqueryNode(subquery, inner, 1)) {
		return false;
	}
	out_.picks_rows = out_.picks_rows || inner_out.picks_rows;
	out_.total_input_rows += inner_out.total_input_rows;
	out_.total_size_unknown = out_.total_size_unknown || inner_out.total_size_unknown;
	// At most one row, provably: DuckDB checks every outer row (an
	// uncorrelated subquery once, even for no outer row) and raises on several;
	// the server evaluates it lazily -- never under a TOP that stops first, an
	// untaken CASE branch, a COALESCE already answered, a COUNT that drops the
	// column -- and returns rows where DuckDB errors (review of E1).
	const auto &subquery_node = *subquery.Subquery()->node;
	if (!(inner.aggregated_ && inner.group_keys_.empty()) && !(inner.limit_ >= 0 && inner.limit_ <= 1) &&
		!(subquery_node.type == QueryNodeType::SELECT_NODE && inner.KeyedToOneRow(subquery_node.Cast<SelectNode>()))) {
		return Veto("a scalar subquery that may return several rows");
	}
	return SubqueryOperand(inner, inner_out, "(" + inner_out.statement + ")", out);
}

bool NodeWriter::WriteSubqueryCondition(const SubqueryExpression &subquery, std::string &sql) {
	WrittenQuery inner_out;
	NodeWriter inner(*this, inner_out, Correlated());
	inner.in_expression_ = true;
	switch (subquery.GetSubqueryType()) {
	case SubqueryType::EXISTS:
	case SubqueryType::NOT_EXISTS: {
		if (!WriteSubqueryNode(subquery, inner, 0)) {
			return false;
		}
		out_.picks_rows = out_.picks_rows || inner_out.picks_rows;
		out_.total_input_rows += inner_out.total_input_rows;
		out_.total_size_unknown = out_.total_size_unknown || inner_out.total_size_unknown;
		const std::string exists = "EXISTS (" + inner_out.statement + ")";
		sql = subquery.GetSubqueryType() == SubqueryType::NOT_EXISTS ? ExpressionVocabulary::Not(exists) : exists;
		return true;
	}
	case SubqueryType::ANY: {
		// `x IN (SELECT ...)`; NOT IN is NOT over it, three-valued alike.
		if (subquery.GetComparisonType() != ExpressionType::COMPARE_EQUAL || !subquery.GetChild()) {
			return Veto("a quantified comparison " + subquery.ToString());
		}
		Operand left;
		if (!WriteValue(*subquery.GetChild(), left)) {
			return false;
		}
		if (!WriteSubqueryNode(subquery, inner, 1)) {
			return false;
		}
		out_.picks_rows = out_.picks_rows || inner_out.picks_rows;
		out_.total_input_rows += inner_out.total_input_rows;
		out_.total_size_unknown = out_.total_size_unknown || inner_out.total_size_unknown;
		Operand right;
		if (!SubqueryOperand(inner, inner_out, std::string(), right)) {
			return false;
		}
		if (left.constant) {
			// Typed from the subquery's column, as the binder types it.
			if (!BindConstant(right, left)) {
				return false;
			}
		} else if (!BindPair(left, right, subquery.ToString())) {
			return false;
		}
		sql = "(" + left.sql + " IN (" + inner_out.statement + "))";
		return true;
	}
	default:
		return Veto("subquery " + subquery.ToString());
	}
}

}  // namespace node_writer
}  // namespace mssql
}  // namespace duckdb
