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

int NodeWriter::FindColumn(const ColumnRefExpression &ref, idx_t &out) const {
	if (relations_.empty()) {
		// A set operation's writer: no relation of its own to name.
		return 0;
	}
	auto &names = ref.ColumnNames();
	if (names.size() > 2) {
		return -1;
	}
	const auto &name = names.back().GetIdentifierName();
	idx_t relation = DConstants::INVALID_INDEX;
	if (names.size() == 2) {
		for (idx_t r = 0; r < relations_.size(); r++) {
			if (StringUtil::CIEquals(names[0].GetIdentifierName(), relations_[r].name)) {
				relation = r;
			}
		}
		if (relation == DConstants::INVALID_INDEX) {
			return -1;
		}
	} else {
		// An unqualified USING column is the merged one DuckDB's binder makes.
		for (auto &merged : using_columns_) {
			if (StringUtil::CIEquals(merged.first, name)) {
				out = merged.second;
				return 1;
			}
		}
	}
	if (relation != DConstants::INVALID_INDEX && ((visible_ != DConstants::INVALID_INDEX && relation >= visible_) ||
												  (relations_[relation].semi && relation != semi_scope_))) {
		return -1;
	}
	bool found = false;
	for (idx_t i = 0; i < columns_.size(); i++) {
		if ((visible_ != DConstants::INVALID_INDEX && relation_of_[i] >= visible_) ||
			(relations_[relation_of_[i]].semi && relation_of_[i] != semi_scope_) ||
			(relation != DConstants::INVALID_INDEX && relation_of_[i] != relation) ||
			!StringUtil::CIEquals(columns_[i].name, name)) {
			continue;
		}
		if (found) {
			// Two relations with the column (DuckDB's ambiguity error), or a
			// case-sensitive database holding `a` and `A`.
			return -1;
		}
		found = true;
		out = i;
	}
	return found ? 1 : 0;
}

bool NodeWriter::ResolveColumn(const ColumnRefExpression &ref, idx_t &out) {
	auto &names = ref.ColumnNames();
	const int found = FindColumn(ref, out);
	bool alias = names.size() == 1 && StringUtil::CIEquals(names[0].GetIdentifierName(), "rowid");
	for (auto &name : select_aliases_) {
		alias = alias || (names.size() == 1 && StringUtil::CIEquals(names[0].GetIdentifierName(), name));
	}
	if (found != 1 && !HasOuterRelations() && !alias && MayBeOuter(ref, found)) {
		out_.refers_outside = true;
	}
	switch (found) {
	case 1:
		return true;
	case 0:
		return Veto("column " + names.back().GetIdentifierName() + " is not a column of the query's tables");
	default:
		if (names.size() > 2) {
			return Veto("column reference " + ref.ToString() + " has more than two parts");
		}
		if (names.size() == 2) {
			bool known = false;
			for (auto &relation : relations_) {
				known = known || StringUtil::CIEquals(names[0].GetIdentifierName(), relation.name);
			}
			if (!known) {
				return Veto("column reference " + ref.ToString() + " names another relation");
			}
			for (auto &relation : relations_) {
				if (relation.semi && StringUtil::CIEquals(names[0].GetIdentifierName(), relation.name)) {
					return Veto("column reference " + ref.ToString() +
								" names a SEMI / ANTI join's table, which only its condition sees");
				}
			}
		}
		return Veto("column name " + names.back().GetIdentifierName() + " matches more than one column");
	}
}

bool NodeWriter::IsHiddenUsingColumn(idx_t index) const {
	for (auto hidden : hidden_using_) {
		if (hidden == index) {
			return true;
		}
	}
	return false;
}

bool NodeWriter::CollectRelations(const TableRef &ref, std::vector<idx_t> &members) {
	if (ref.type == TableReferenceType::BASE_TABLE) {
		auto &base = ref.Cast<BaseTableRef>();
		if (base.sample || !base.column_name_alias.empty() || base.at_clause) {
			return Veto("TABLESAMPLE / column aliases / AT on a table");
		}
		NodeWriter *owner = nullptr;
		idx_t index = 0;
		auto cte = IsWrittenUnqualified(base) ? FindCte(base.Table().GetIdentifierName(), owner, index) : nullptr;
		if (cte) {
			// Evaluated per row of a node around it: a subquery expression between
			// this reference and the CTE's WITH (or around a CTE reading it).
			bool per_row = false;
			for (auto writer = this; writer && writer != owner; writer = writer->cte_parent_) {
				per_row = per_row || writer->in_expression_;
			}
			bool picks_rows = false;
			if (!CollectDerivedNode(*cte->body, base.alias.empty() ? cte->name : base.alias.GetIdentifierName(), false,
									owner, index, per_row, &picks_rows, members)) {
				return false;
			}
			// A body that picks rows anywhere in it (a LIMIT among ties) differs
			// between evaluations; DuckDB evaluates it once. Deterministic bodies
			// are inlined as often as referenced -- each a read of its own, which
			// under concurrent writes can see other committed rows.
			if (++cte->uses > 1 && picks_rows) {
				return Veto("a CTE with a LIMIT referenced more than once");
			}
			if (picks_rows && per_row) {
				return Veto("a CTE with a LIMIT under a subquery expression");
			}
			return true;
		}
		Relation relation;
		if (!resolver_(base, relation.table) || !relation.table.columns) {
			return Veto("table " + base.Table().GetIdentifierName() + " does not resolve in this catalog");
		}
		relation.name = base.alias.empty() ? relation.table.name : base.alias.GetIdentifierName();
		for (auto &other : relations_) {
			if (StringUtil::CIEquals(other.name, relation.name)) {
				// A self-join without aliases, or two schemas' tables of one name --
				// which the rewriter's strip would also merge (`db.s1.t.c` -> `t.c`).
				return Veto("two tables named " + relation.name);
			}
		}
		const auto &columns = *relation.table.columns;
		for (idx_t i = 0; i < columns.size(); i++) {
			columns_.push_back(columns[i]);
			types_.push_back(relation.table.types ? (*relation.table.types)[i] : columns[i].NativeDuckDBType());
			relation_of_.push_back(relations_.size());
			cast_of_.push_back(LogicalType::INVALID);
			already_read_.push_back(false);
			divergent_of_.push_back(false);
		}
		members.push_back(relations_.size());
		relations_.push_back(std::move(relation));
		return true;
	}
	if (ref.type == TableReferenceType::SUBQUERY) {
		return CollectDerived(ref.Cast<SubqueryRef>(), members);
	}
	if (ref.type != TableReferenceType::JOIN) {
		return Veto("FROM holds a table function, VALUES or another non-table");
	}
	auto &join = ref.Cast<JoinRef>();
	if (!join.alias.empty() || !join.column_name_alias.empty() || !join.duplicate_eliminated_columns.empty() ||
		join.sample) {
		return Veto("an alias, sample or column aliases on a join");
	}
	if (join.ref_type != JoinRefType::REGULAR && join.ref_type != JoinRefType::CROSS) {
		return Veto("a NATURAL / POSITIONAL / ASOF / LATERAL / NEAREST join");
	}
	const bool semi = join.type == JoinType::SEMI || join.type == JoinType::ANTI;
	if (join.type != JoinType::INNER && join.type != JoinType::LEFT && join.type != JoinType::RIGHT &&
		join.type != JoinType::OUTER && !semi) {
		return Veto("a MARK / SINGLE join");
	}
	if (semi && join.ref_type == JoinRefType::CROSS) {
		return Veto("a SEMI / ANTI join without a condition");
	}
	if (join.right->type != TableReferenceType::BASE_TABLE && join.right->type != TableReferenceType::SUBQUERY) {
		// Left-deep only: `a JOIN (b JOIN c)` would need its own parentheses and
		// the nullability of a nested outer join.
		return Veto("a join whose right side is not a table or a subquery");
	}
	std::vector<idx_t> left;
	std::vector<idx_t> right;
	if (!CollectRelations(*join.left, left) || !CollectRelations(*join.right, right)) {
		return false;
	}
	bool semi_on_left = false;
	for (auto r : left) {
		semi_on_left = semi_on_left || relations_[r].semi;
	}
	if (semi) {
		for (auto r : right) {
			relations_[r].semi = true;
		}
	}
	if (semi_on_left && (join.type == JoinType::RIGHT || join.type == JoinType::OUTER)) {
		// A SEMI join's EXISTS is ANDed to WHERE, after every join. That is
		// exact for the rows its left side produces, which later INNER and LEFT
		// joins keep, and whatever joins came before it -- but a later RIGHT /
		// FULL join adds rows with that side NULL, which the moved EXISTS would
		// drop.
		return Veto("a RIGHT / FULL join over a SEMI / ANTI join");
	}
	// The NULL-supplying side: its columns can be NULL whatever they declare.
	if (join.type == JoinType::LEFT || join.type == JoinType::OUTER) {
		for (auto r : right) {
			relations_[r].nullable = true;
		}
	}
	if (join.type == JoinType::RIGHT || join.type == JoinType::OUTER) {
		for (auto r : left) {
			relations_[r].nullable = true;
		}
	}
	members.insert(members.end(), left.begin(), left.end());
	members.insert(members.end(), right.begin(), right.end());
	return true;
}

void NodeWriter::CollectEqualities(const ParsedExpression &condition) {
	if (condition.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		for (auto &child : condition.Cast<ConjunctionExpression>().GetChildren()) {
			CollectEqualities(*child);
		}
		return;
	}
	if (condition.GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
		return;
	}
	auto &cmp = condition.Cast<ComparisonExpression>();
	if (cmp.Left().GetExpressionClass() != ExpressionClass::COLUMN_REF ||
		cmp.Right().GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return;
	}
	idx_t left;
	idx_t right;
	if (FindColumn(cmp.Left().Cast<ColumnRefExpression>(), left) == 1 &&
		FindColumn(cmp.Right().Cast<ColumnRefExpression>(), right) == 1) {
		link_equalities_.emplace_back(left, right);
	}
}

// Whether every column of `relation`'s unique key is equated, by this link,
// with a column of the relations [other_first, other_last).
bool NodeWriter::KeyCovered(idx_t relation, idx_t other_first, idx_t other_last) const {
	auto &key = relations_[relation].table.unique_key;
	if (key.empty()) {
		return false;
	}
	for (auto &key_column : key) {
		bool equated = false;
		for (auto &pair : link_equalities_) {
			for (auto side : {std::make_pair(pair.first, pair.second), std::make_pair(pair.second, pair.first)}) {
				const auto rel = relation_of_[side.first];
				const auto other = relation_of_[side.second];
				equated = equated || (rel == relation && StringUtil::CIEquals(columns_[side.first].name, key_column) &&
									  other >= other_first && other < other_last);
			}
		}
		if (!equated) {
			return false;
		}
	}
	return true;
}

bool NodeWriter::CoversEveryKey(const std::vector<idx_t> &columns) const {
	for (idx_t r = 0; r < relations_.size(); r++) {
		if (relations_[r].semi) {
			continue;
		}
		auto &key = relations_[r].table.unique_key;
		if (key.empty()) {
			return false;
		}
		for (auto &key_column : key) {
			bool found = false;
			for (auto index : columns) {
				found = found || (relation_of_[index] == r && StringUtil::CIEquals(columns_[index].name, key_column));
			}
			if (!found) {
				return false;
			}
		}
	}
	return true;
}

NodeWriter::CteEntry *NodeWriter::FindCte(const std::string &name, NodeWriter *&owner, idx_t &index) {
	idx_t limit = ctes_.size();
	for (NodeWriter *writer = this; writer;) {
		for (idx_t i = 0; i < MinValue<idx_t>(limit, writer->ctes_.size()); i++) {
			if (StringUtil::CIEquals(writer->ctes_[i].name, name)) {
				owner = writer;
				index = i;
				return &writer->ctes_[i];
			}
		}
		limit = writer->cte_parent_limit_;
		writer = writer->cte_parent_;
	}
	return nullptr;
}

bool NodeWriter::IsWrittenUnqualified(const BaseTableRef &ref) const {
	if (written_unqualified_ && *written_unqualified_) {
		return (*written_unqualified_)(ref);
	}
	auto &name = ref.GetQualifiedName();
	return name.Catalog().empty() && name.Schema().empty();
}

bool NodeWriter::CollectDerived(const SubqueryRef &ref, std::vector<idx_t> &members) {
	if (!ref.column_name_alias.empty()) {
		return Veto("column aliases on a subquery");
	}
	return CollectDerivedNode(*ref.subquery->node,
							  ref.alias.empty() ? "unnamed_subquery" : ref.alias.GetIdentifierName(),
							  &ref == synthetic_ref_, nullptr, 0, false, nullptr, members);
}

bool NodeWriter::CollectDerivedNode(const QueryNode &node, const std::string &name, bool synthetic,
									NodeWriter *cte_owner, idx_t cte_index, bool per_row, bool *picks_rows,
									std::vector<idx_t> &members) {
	bool ordered = false;
	bool limited = false;
	for (auto &modifier : node.modifiers) {
		ordered = ordered || modifier->type == ResultModifierType::ORDER_MODIFIER;
		limited = limited || modifier->type == ResultModifierType::LIMIT_MODIFIER;
	}
	if (ordered && !limited) {
		// T-SQL refuses an ORDER BY in a derived table without TOP / OFFSET
		// (1033); DuckDB promises no order there either.
		return Veto("an ORDER BY without LIMIT in a subquery");
	}
	WrittenQuery inner_out;
	NodeWriter inner(*this, inner_out);
	if (synthetic) {
		// The set operation of a wrapper this writer stands in for: correlated
		// as the wrapper is, its members aliased past the nodes around it.
		inner.outer_ = outer_;
		inner.alias_base_ = alias_base_ + relations_.size() + 1;
	}
	if (cte_owner) {
		inner.cte_parent_ = cte_owner;
		inner.cte_parent_limit_ = cte_index;
		inner.in_expression_ = per_row;
	}
	if (synthetic && synthetic_select_) {
		inner.qualifying_ = true;
		inner.qualify_names_ = qualify_names_;
	}
	if (synthetic ? (synthetic_select_ ? !inner.Write(*synthetic_select_) : !inner.WriteSetOperation(*synthetic_setop_))
				  : !inner.WriteQueryNode(node)) {
		out_.refers_outside = out_.refers_outside || inner_out.refers_outside;
		return false;
	}
	correlated_ = correlated_ || inner.correlated_;
	out_.picks_rows = out_.picks_rows || inner_out.picks_rows;
	// Its joins' gain is this node's to settle (review of E1: a many-to-many
	// join wrapped in a derived table or a CTE skipped the check).
	derived_uncertain_ = derived_uncertain_ || inner_out.gain_uncertain;
	if (picks_rows) {
		*picks_rows = inner_out.picks_rows;
	}
	if (inner_out.statement.size() > MAX_DERIVED_SQL) {
		// A CTE read twice by a CTE read twice ... doubles per level.
		return Veto("a derived table or inlined CTE past " + std::to_string(MAX_DERIVED_SQL) + " bytes of T-SQL");
	}
	Relation relation;
	relation.derived = true;
	relation.derived_sql = inner_out.statement;
	relation.name = name;
	relation.table.name = relation.name;
	// Sized by its inputs, as a join's tables are; a view among them unknown.
	relation.table.size_known = !inner_out.input_size_unknown;
	relation.table.approx_rows = inner_out.largest_input_rows;
	relation.total_rows = inner_out.total_input_rows;
	relation.total_unknown = inner_out.total_size_unknown;
	// Its GROUP BY columns (or all of them under DISTINCT) are a unique key,
	// when every one of them is a result column.
	if (inner.aggregated_ && !inner.group_keys_.empty()) {
		for (auto key : inner.group_keys_) {
			for (auto &output : inner.Outputs()) {
				if (output.column_index == key && output.plain_read) {
					relation.table.unique_key.push_back(output.name);
					break;
				}
			}
		}
		if (relation.table.unique_key.size() != inner.group_keys_.size()) {
			relation.table.unique_key.clear();
		}
	} else if (inner.distinct_ && !inner.aggregated_) {
		for (auto &output : inner.Outputs()) {
			relation.table.unique_key.push_back(output.name);
		}
	}
	for (auto &other : relations_) {
		if (StringUtil::CIEquals(other.name, relation.name)) {
			return Veto("two tables named " + relation.name);
		}
	}
	auto &outputs = inner.Outputs();
	for (idx_t i = 0; i < outputs.size(); i++) {
		auto &output = outputs[i];
		if (output.column_index != DConstants::INVALID_INDEX && output.plain_read) {
			auto column = inner.FlatColumn(output.column_index);
			column.name = output.name;
			columns_.push_back(std::move(column));
		} else {
			// The operand type the server's value has (an integer SUM is
			// decimal(38,0) there); computed values of unknown type are opaque.
			// So is a division or a floating-point aggregate: its divergence
			// (NULL for inf, the last bits) is recorded for a VALUE only, and
			// outside its node it would be compared, ordered, grouped or
			// COALESCEd as a plain float (review of E1: rows differed).
			const bool opaque =
				output.column_index != DConstants::INVALID_INDEX || output.value.division || output.value.approximate;
			columns_.push_back(DerivedColumn(output.name, opaque ? LogicalType::INVALID : output.value.type));
		}
		types_.push_back(inner_out.column_types[i]);
		cast_of_.push_back(inner_out.cast_types[i]);
		already_read_.push_back(true);
		divergent_of_.push_back(output.column_index == DConstants::INVALID_INDEX &&
								(output.value.division || output.value.approximate));
		relation_of_.push_back(relations_.size());
	}
	members.push_back(relations_.size());
	relations_.push_back(std::move(relation));
	return true;
}

bool NodeWriter::WriteFrom(const TableRef &ref, std::string &sql) {
	// A subquery, or a CTE reference inlined as one.
	if (ref.type == TableReferenceType::SUBQUERY ||
		(ref.type == TableReferenceType::BASE_TABLE && relations_[visible_].derived)) {
		auto &relation = relations_[visible_++];
		sql = "(" + relation.derived_sql + ") AS " + relation.sql;
		return true;
	}
	if (ref.type == TableReferenceType::BASE_TABLE) {
		// The relations were collected in this same order.
		auto &relation = relations_[visible_++];
		sql = QuoteIdentifier(relation.table.schema) + "." + QuoteIdentifier(relation.table.name);
		if (!relation.sql.empty()) {
			sql += " AS " + relation.sql;
		}
		return true;
	}
	auto &join = ref.Cast<JoinRef>();
	const idx_t left_first = visible_;
	std::string left;
	std::string right;
	if (!WriteFrom(*join.left, left)) {
		return false;
	}
	const idx_t right_relation = visible_;
	if (!WriteFrom(*join.right, right)) {
		return false;
	}
	if (join.ref_type == JoinRefType::CROSS) {
		sql = left + " CROSS JOIN " + right;
		cross_links_.emplace_back(left_first, right_relation);
		return true;
	}
	link_equalities_.clear();
	const bool semi = join.type == JoinType::SEMI || join.type == JoinType::ANTI;
	if (semi) {
		// Its columns are named in its condition only.
		semi_scope_ = right_relation;
	}
	const char *keyword = join.type == JoinType::INNER	 ? " INNER JOIN "
						  : join.type == JoinType::LEFT	 ? " LEFT JOIN "
						  : join.type == JoinType::RIGHT ? " RIGHT JOIN "
														 : " FULL JOIN ";
	std::string condition;
	if (!join.using_columns.empty()) {
		if (join.type == JoinType::RIGHT || join.type == JoinType::OUTER) {
			// The merged column is the right side's, or COALESCE of both: its
			// place in `*` and its value are not the left column's.
			return Veto("USING in a RIGHT / FULL join");
		}
		std::vector<std::string> parts;
		for (auto &using_name : join.using_columns) {
			const auto &name = using_name.GetIdentifierName();
			idx_t left_column = DConstants::INVALID_INDEX;
			idx_t right_column = DConstants::INVALID_INDEX;
			for (idx_t i = 0; i < columns_.size(); i++) {
				if (!StringUtil::CIEquals(columns_[i].name, name)) {
					continue;
				}
				// A SEMI / ANTI join's table is not on the left: it left no columns.
				if (relation_of_[i] >= left_first && relation_of_[i] < right_relation &&
					!relations_[relation_of_[i]].semi) {
					if (left_column != DConstants::INVALID_INDEX) {
						return Veto("USING column " + name + " is in more than one table on the left");
					}
					left_column = i;
				} else if (relation_of_[i] == right_relation) {
					right_column = i;
				}
			}
			if (left_column == DConstants::INVALID_INDEX || right_column == DConstants::INVALID_INDEX) {
				return Veto("USING column " + name + " is not on both sides");
			}
			Operand l;
			Operand r;
			for (auto side : {std::make_pair(left_column, &l), std::make_pair(right_column, &r)}) {
				auto &operand = *side.second;
				operand.sql = ColumnSql(side.first);
				operand.type = columns_[side.first].duckdb_type;
				operand.kind = KindOf(columns_[side.first]);
				operand.column = &columns_[side.first];
			}
			if (!BindPair(l, r, "USING (" + name + ")")) {
				return false;
			}
			parts.push_back(ExpressionVocabulary::Comparison(" = ", l.sql, r.sql));
			using_columns_.emplace_back(name, left_column);
			hidden_using_.push_back(right_column);
			link_equalities_.emplace_back(left_column, right_column);
		}
		condition = ExpressionVocabulary::Conjunction(parts, true);
	} else {
		if (!join.condition) {
			return Veto("a join without a condition");
		}
		// ON sees the tables joined so far -- a later one is DuckDB's binder
		// error, not a statement for the server.
		if (!WritePredicate(*join.condition, condition)) {
			return false;
		}
		CollectEqualities(*join.condition);
	}
	// The gain check (PR E1): a link that equates a unique key of one side
	// with the other cannot send more rows than the other side has -- FK to
	// PK. A SEMI / ANTI link never sends more than its left side.
	if (!semi && !KeyCovered(right_relation, left_first, right_relation) &&
		!(right_relation - left_first == 1 && KeyCovered(left_first, right_relation, right_relation + 1))) {
		unbounded_links_++;
	}
	if (semi) {
		// A SEMI join keeps each left row that has a match, once; an ANTI join
		// each that has none -- EXISTS / NOT EXISTS, not IN / NOT IN, which a
		// NULL on the right would empty (measured on DuckDB: ANTI JOIN gives
		// [2, NULL] where NOT IN gives nothing).
		semi_scope_ = DConstants::INVALID_INDEX;
		const std::string exists = "EXISTS (SELECT 1 FROM " + right + " WHERE " + condition + ")";
		semi_filters_.push_back(join.type == JoinType::ANTI ? ExpressionVocabulary::Not(exists) : exists);
		sql = left;
		return true;
	}
	sql = left + keyword + right + " ON " + condition;
	return true;
}

}  // namespace node_writer
}  // namespace mssql
}  // namespace duckdb
