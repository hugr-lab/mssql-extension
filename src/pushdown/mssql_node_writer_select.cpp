#include "pushdown/mssql_node_writer.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "pushdown/mssql_expression_vocabulary.hpp"
#include "query/mssql_identifier.hpp"

namespace duckdb {
namespace mssql {
namespace node_writer {

//! A name's length as SQL Server counts an identifier's (UTF-16 code units: one per code point, two above
//! U+FFFF), from its UTF-8 bytes.
static size_t IdentifierLength(const std::string &utf8) {
	size_t units = 0;
	for (const unsigned char b : utf8) {
		if ((b & 0xC0) != 0x80) {
			units += b >= 0xF0 ? 2 : 1;
		}
	}
	return units;
}

bool NodeWriter::CheckNewResultName(const std::string &output) {
	for (auto &existing : outputs_) {
		// Two equal result names would come back deduplicated differently than DuckDB names
		// them.
		if (StringUtil::CIEquals(existing.name, output)) {
			return Veto("two result columns are named " + output);
		}
	}
	return true;
}

void NodeWriter::WriteColumnRead(idx_t index, const std::string &output, const std::string &as, std::string &list,
								 OutputColumn &entry) {
	const auto &column = columns_[index];
	const auto &relation = relations_[relation_of_[index]];
	if (already_read_[index]) {
		// A derived table's column: its node read it already.
		list += ColumnSql(index) + " AS " + QuoteIdentifier(output);
		entry.plain_read = true;
	} else if (raw_columns_) {
		list += ColumnSql(index) + (as.empty() ? "" : " AS " + QuoteIdentifier(as));
		entry.plain_read = true;
	} else {
		const auto read = MSSQLColumnInfo::BuildReadExpression(
			column.name, column.sql_type_name, column.max_length, column.collation_name, options_.convert_varchar_max,
			options_.client_transcode, "", "", options_.utf8_on_wire);
		list += MSSQLColumnInfo::BuildReadExpression(
			column.name, column.sql_type_name, column.max_length, column.collation_name, options_.convert_varchar_max,
			options_.client_transcode, relation.sql.empty() ? "" : relation.sql + ".", as, options_.utf8_on_wire);
		entry.plain_read = read == QuoteIdentifier(column.name);
	}
}

bool NodeWriter::AddResultColumn(idx_t index, const std::string &alias, std::string &list) {
	const auto &column = columns_[index];
	// Spec 093 D4: a vector column streams VECTORTYPE or its JSON text as the session negotiated, and a
	// pushed statement's result types are fixed before it runs; the catalog scan reads either form.
	// Copied as written (a pushed INSERT ... SELECT), the server keeps its bytes.
	if (!raw_columns_ && StringUtil::CIEquals(column.sql_type_name, "vector")) {
		return Veto("a vector column in the result");
	}
	// A column's result name is its DuckDB name (issue #572: the server's `ID` beside `Id` reads as
	// ID_1), sent as an alias where the two differ.
	const std::string output = alias.empty() ? column.DuckDBName() : alias;
	const std::string as = alias.empty() && column.DuckDBName() != column.name ? column.DuckDBName() : alias;
	if (IdentifierLength(as) > 128) {
		return Veto("a result column name longer than 128 characters");
	}
	if (!CheckNewResultName(output) || !CheckGrouped(index)) {
		return false;
	}
	if (!list.empty()) {
		list += ", ";
	}
	OutputColumn entry;
	entry.name = output;
	entry.column_index = index;
	WriteColumnRead(index, output, as, list, entry);
	outputs_.push_back(std::move(entry));
	const bool cast_back = cast_of_[index].id() != LogicalTypeId::INVALID;
	out_.column_types.push_back(cast_back ? LogicalType::INVALID : types_[index]);
	out_.cast_types.push_back(cast_of_[index]);
	out_.column_names.push_back(output);
	return true;
}

bool NodeWriter::StarRelation(const StarExpression &star, idx_t &only) {
	only = DConstants::INVALID_INDEX;
	if (star.RelationName().empty()) {
		return true;
	}
	for (idx_t r = 0; r < relations_.size(); r++) {
		if (StringUtil::CIEquals(star.RelationName().GetIdentifierName(), relations_[r].name)) {
			only = r;
		}
	}
	if (only == DConstants::INVALID_INDEX) {
		return Veto("a * of another relation");
	}
	if (relations_[only].semi) {
		return Veto("a * of a SEMI / ANTI join's table");
	}
	return true;
}

bool NodeWriter::StarLeavesOut(idx_t index, idx_t only) const {
	if (only != DConstants::INVALID_INDEX) {
		return relation_of_[index] != only;
	}
	return IsHiddenUsingColumn(index) || relations_[relation_of_[index]].semi ||
		   (!hidden_column_.empty() && StringUtil::CIEquals(columns_[index].name, hidden_column_));
}

bool NodeWriter::WriteStarItem(const StarExpression &star, std::string &list) {
	if (star.IsColumns() || !star.ExcludeList().empty() || !star.ReplaceList().empty() || !star.RenameList().empty() ||
		star.Expression()) {
		return Veto("a * with COLUMNS / EXCLUDE / REPLACE / RENAME");
	}
	if (aggregated_) {
		return Veto("a * in an aggregate query");
	}
	// `*` lists every relation's columns in FROM order, a USING column
	// once; `r.*` one relation's, all of them.
	idx_t only;
	if (!StarRelation(star, only)) {
		return false;
	}
	for (idx_t i = 0; i < columns_.size(); i++) {
		if (StarLeavesOut(i, only)) {
			continue;
		}
		if (!AddResultColumn(i, "", list)) {
			return false;
		}
	}
	return true;
}

bool NodeWriter::WriteConstantItem(const ParsedExpression &item, std::string &list) {
	// An integer constant (`EXISTS (SELECT 1 ...)`): INTEGER to DuckDB's
	// binder as an int literal is to the server. Any other literal is
	// typed by rules of its own on each side.
	auto &literal = item.Cast<ConstantExpression>().GetLiteral();
	int64_t number;
	if (aggregated_ || literal.kind != LiteralKind::INTEGER || !literal.TryGetInt64(number) ||
		number > NumericLimits<int32_t>::Maximum() || number < NumericLimits<int32_t>::Minimum()) {
		return Veto("a constant in the select list");
	}
	const std::string output = item.GetName().GetIdentifierName();
	if (output.size() > 128) {
		return Veto("a result column name longer than 128 characters");
	}
	if (!CheckNewResultName(output)) {
		return false;
	}
	list += (list.empty() ? "" : ", ") + std::to_string(number) + " AS " + QuoteIdentifier(output);
	out_.column_types.push_back(LogicalType::INTEGER);
	out_.cast_types.push_back(LogicalType::INVALID);
	out_.column_names.push_back(output);
	OutputColumn entry;
	entry.name = output;
	entry.column_index = DConstants::INVALID_INDEX;
	entry.value.sql = std::to_string(number);
	entry.value.type = LogicalType::INTEGER;
	entry.value.result_type = LogicalType::INTEGER;
	entry.value.kind = ComparableKind::ExactNumeric;
	entry.value.not_null = true;
	outputs_.push_back(std::move(entry));
	return true;
}

bool NodeWriter::WriteComputedItem(const ParsedExpression &item, const std::string &alias, std::string &list) {
	// A computed column: rendered by the same rules as WHERE, named as
	// DuckDB names it (the alias, else the expression's text), typed by
	// the server's describe -- the spec's "types of a pushed query are the
	// server's" (column_types '' for it) -- or, for an aggregate, as DuckDB
	// types it.
	Operand value;
	if (!WriteValue(item, value)) {
		return false;
	}
	if (value.constant) {
		return Veto("a constant in the select list");
	}
	// A result column carries its type: a decimal whose DuckDB type the
	// writer cannot tell would come back as the server's (fuzz of PR D:
	// `v + 700` DECIMAL(11,2) pushed, DECIMAL(13,2) local).
	if (value.type_uncertain ||
		(value.type.id() == LogicalTypeId::INVALID && value.result_type.id() == LogicalTypeId::INVALID)) {
		return Veto("a computed decimal whose DuckDB type differs from the server's");
	}
	if (value.result_type.id() == LogicalTypeId::INVALID && value.type.id() == LogicalTypeId::DECIMAL) {
		value.result_type = value.type;
	}
	if (alias.empty() && item.HasSubquery()) {
		// Named after its text, which holds the table names the rewriter
		// stripped of their catalog (review of E1).
		return Veto("a subquery in a result column without an alias");
	}
	const std::string output = item.GetName().GetIdentifierName();
	if (output.size() > 128) {
		// SQL Server refuses a longer identifier (103); DuckDB does not.
		return Veto("a result column name longer than 128 characters");
	}
	if (!CheckNewResultName(output)) {
		return false;
	}
	if (!list.empty()) {
		list += ", ";
	}
	list += value.sql + " AS " + QuoteIdentifier(output);
	out_.column_types.push_back(value.cast_result ? LogicalType::INVALID : value.result_type);
	out_.cast_types.push_back(value.cast_result ? value.result_type : LogicalType::INVALID);
	out_.column_names.push_back(output);
	OutputColumn entry;
	entry.name = output;
	entry.column_index = DConstants::INVALID_INDEX;
	entry.value = std::move(value);
	outputs_.push_back(std::move(entry));
	return true;
}

bool NodeWriter::WriteSelectItem(const ParsedExpression &item, std::string &list) {
	const auto &alias = item.GetAlias().GetIdentifierName();
	switch (item.GetExpressionClass()) {
	case ExpressionClass::STAR:
		return WriteStarItem(item.Cast<StarExpression>(), list);
	case ExpressionClass::COLUMN_REF: {
		idx_t index;
		if (!ResolveColumn(item.Cast<ColumnRefExpression>(), index)) {
			return false;
		}
		// An alias equal to the column's own name renders as the bare column.
		const bool renamed = !alias.empty() && alias != columns_[index].DuckDBName();
		return AddResultColumn(index, renamed ? alias : "", list);
	}
	case ExpressionClass::CONSTANT:
		return WriteConstantItem(item, list);
	case ExpressionClass::FUNCTION:
	case ExpressionClass::CASE:
	case ExpressionClass::CAST:
	case ExpressionClass::OPERATOR:
	case ExpressionClass::SUBQUERY:
	case ExpressionClass::WINDOW:
		return WriteComputedItem(item, alias, list);
	default:
		return Veto("select-list expression " + item.ToString());
	}
}

bool NodeWriter::WriteSelectList(const SelectNode &node) {
	std::string list;
	for (auto &item : node.select_list) {
		if (!WriteSelectItem(*item, list)) {
			return false;
		}
	}
	out_.statement += list;
	return true;
}

bool NodeWriter::ConstantCount(const ParsedExpression &expr, int64_t &out, const char *what) {
	if (expr.GetExpressionClass() != ExpressionClass::CONSTANT) {
		return Veto(std::string(what) + " " + expr.ToString() + " is not a constant");
	}
	auto &literal = expr.Cast<ConstantExpression>().GetLiteral();
	if (literal.kind != LiteralKind::INTEGER || !literal.TryGetInt64(out) || out < 0) {
		return Veto(std::string(what) + " " + expr.ToString());
	}
	return true;
}

bool NodeWriter::BeginSelect(const SelectNode &node) {
	for (auto &cte : node.cte_map.map) {
		auto &info = *cte.second;
		if (!info.query_node || !info.aliases.empty() || !info.key_targets.empty() ||
			!info.payload_aggregates.empty()) {
			return Veto("a CTE with column aliases, USING KEY or no query");
		}
		ctes_.push_back(CteEntry{cte.first.GetIdentifierName(), info.query_node.get(), 0});
	}
	if (node.aggregate_handling != AggregateHandling::STANDARD_HANDLING) {
		return Veto("GROUP BY ALL");
	}
	if (node.sample) {
		return Veto("USING SAMPLE");
	}
	if (!node.from_table) {
		return Veto("no FROM");
	}
	for (auto &item : node.select_list) {
		if (!item->GetAlias().empty()) {
			select_aliases_.push_back(item->GetAlias().GetIdentifierName());
		}
	}
	return true;
}

bool NodeWriter::WriteRelations(const SelectNode &node, std::string &from_sql) {
	std::vector<idx_t> members;
	if (!CollectRelations(*node.from_table, members)) {
		return false;
	}
	bool any_derived = false;
	for (auto &relation : relations_) {
		any_derived = any_derived || relation.derived;
	}
	// A subquery in an expression may name this node's columns, and its own
	// tables may be this node's: every name qualified by an alias of its own.
	if (relations_.size() > 1 || any_derived || outer_ || HasSubqueryExpression(node)) {
		// A join, or a derived table (which T-SQL requires an alias for): every
		// relation under its own alias, every column qualified.
		for (idx_t r = 0; r < relations_.size(); r++) {
			relations_[r].sql = "[r" + std::to_string(alias_base_ + r + 1) + "]";
		}
	}
	for (idx_t i = 0; i < columns_.size(); i++) {
		columns_[i].is_nullable = columns_[i].is_nullable || relations_[relation_of_[i]].nullable;
	}
	// The FROM first: a USING column is known before the select list names it.
	visible_ = 0;
	if (!WriteFrom(*node.from_table, from_sql)) {
		return false;
	}
	visible_ = DConstants::INVALID_INDEX;
	return true;
}

bool NodeWriter::CollectModifiers(const SelectNode &node, SelectModifiers &modifiers) {
	// Inside a QUALIFY wrapper the modifiers are the wrapper's.
	for (idx_t i = 0; i < (qualifying_ ? 0 : node.modifiers.size()); i++) {
		auto &modifier = node.modifiers[i];
		// The parser puts DISTINCT first; DISTINCT ON is DuckDB's own.
		if (modifier->type == ResultModifierType::DISTINCT_MODIFIER && i == 0 &&
			modifier->Cast<DistinctModifier>().distinct_on_targets.empty()) {
			distinct_ = true;
		} else if (modifier->type == ResultModifierType::ORDER_MODIFIER && !modifiers.order && !modifiers.limit) {
			modifiers.order = &modifier->Cast<OrderModifier>();
		} else if (modifier->type == ResultModifierType::LIMIT_MODIFIER && !modifiers.limit) {
			modifiers.limit = &modifier->Cast<LimitModifier>();
		} else {
			return Veto("a DISTINCT ON, a LIMIT % or a second ORDER BY / LIMIT");
		}
	}
	return true;
}

bool NodeWriter::ReadModifiers(const SelectNode &node, SelectModifiers &modifiers) {
	if (!CollectModifiers(node, modifiers)) {
		return false;
	}
	if (modifiers.limit) {
		auto &limit = *modifiers.limit;
		if (limit.limit_type != LimitValueType::ROW_COUNT) {
			// LIMIT 10%: no T-SQL form is proven to round as DuckDB does.
			return Veto("LIMIT as a percentage");
		}
		if (limit.limit && !ConstantCount(*limit.limit, modifiers.limit_value)) {
			return false;
		}
		if (limit.offset && !ConstantCount(*limit.offset, modifiers.offset_value)) {
			return false;
		}
	}
	limit_ = modifiers.limit_value;
	out_.picks_rows = out_.picks_rows || modifiers.Limited() || modifiers.offset_value > 0;
	if (distinct_ && modifiers.offset_value > 0 && !modifiers.order) {
		// OFFSET's `ORDER BY (SELECT NULL)` is not a result column: 145.
		return Veto("DISTINCT with OFFSET and no ORDER BY");
	}
	return true;
}

void NodeWriter::NoteAggregates(const SelectNode &node, const OrderModifier *order) {
	aggregated_ = aggregated_ || node.having || (qualifying_ && ContainsAggregate(*node.qualify));
	for (auto &item : node.select_list) {
		aggregated_ = aggregated_ || ContainsAggregate(*item);
	}
	if (order) {
		for (auto &key : order->orders) {
			aggregated_ = aggregated_ || ContainsAggregate(*key.expression);
		}
	}
}

bool NodeWriter::CheckDistinctOutputs() {
	for (auto &output : outputs_) {
		if (output.column_index != DConstants::INVALID_INDEX) {
			if (!IsGroupable(columns_[output.column_index])) {
				return Veto("DISTINCT over " + columns_[output.column_index].sql_type_name);
			}
		} else if (output.value.kind == ComparableKind::None || output.value.approximate || output.value.division) {
			return Veto("DISTINCT over the computed column " + output.name);
		}
	}
	return true;
}

bool NodeWriter::WriteWhere(const SelectNode &node) {
	std::vector<std::string> filters;
	if (node.where_clause) {
		std::string where;
		if (!WritePredicate(*node.where_clause, where)) {
			return false;
		}
		filters.push_back(where);
	}
	filters.insert(filters.end(), semi_filters_.begin(), semi_filters_.end());
	if (!filters.empty()) {
		out_.statement += " WHERE " + ExpressionVocabulary::Conjunction(filters, true);
	}
	return true;
}

bool NodeWriter::WriteGroupByHaving(const SelectNode &node, const std::string &group_sql) {
	if (!group_sql.empty()) {
		out_.statement += " GROUP BY " + group_sql;
	}
	if (node.having) {
		std::string having;
		if (!WritePredicate(*node.having, having)) {
			return false;
		}
		out_.statement += " HAVING " + having;
	}
	return true;
}

bool NodeWriter::WriteOrderAndPaging(const SelectModifiers &modifiers) {
	std::string order_sql;
	if (modifiers.order && !WriteOrder(*modifiers.order, modifiers.Limited(), order_sql)) {
		return false;
	}
	if (modifiers.offset_value > 0) {
		// OFFSET needs an ORDER BY in T-SQL; without one DuckDB promises no order
		// either, and `(SELECT NULL)` asks for none.
		out_.statement += " ORDER BY " + (order_sql.empty() ? std::string("(SELECT NULL)") : order_sql);
		out_.statement += " OFFSET " + std::to_string(modifiers.offset_value) + " ROWS";
		if (modifiers.Limited()) {
			out_.statement += " FETCH NEXT " + std::to_string(modifiers.limit_value) + " ROWS ONLY";
		}
	} else if (!order_sql.empty()) {
		out_.statement += " ORDER BY " + order_sql;
	}
	return true;
}

void NodeWriter::CountUnboundedLinks(const SelectNode &node) {
	// A CROSS link (a comma join) is bounded by WHERE's equalities as an ON
	// link is by its own.
	if (!cross_links_.empty() && node.where_clause) {
		link_equalities_.clear();
		CollectEqualities(*node.where_clause);
	}
	for (auto &link : cross_links_) {
		const auto left_first = link.first;
		const auto right_relation = link.second;
		if (!node.where_clause ||
			(!KeyCovered(right_relation, left_first, right_relation) &&
			 !(right_relation - left_first == 1 && KeyCovered(left_first, right_relation, right_relation + 1)))) {
			unbounded_links_++;
		}
	}
}

void NodeWriter::NoteGain(bool limited) {
	// A LIMIT bounds what crosses the wire whatever the joins multiply; an
	// aggregate or a DISTINCT does unless it keeps a key of every table (then
	// it has a row per joined row: GROUP BY x.id, y.id over a many-to-many
	// join -- review of PR E1).
	bool reduces = limited;
	if (aggregated_) {
		reduces = reduces || !CoversEveryKey(group_keys_);
	}
	if (distinct_ && !aggregated_) {
		std::vector<idx_t> selected;
		for (auto &output : outputs_) {
			if (output.column_index != DConstants::INVALID_INDEX) {
				selected.push_back(output.column_index);
			}
		}
		reduces = reduces || !CoversEveryKey(selected);
	}
	// A derived table's uncertain join is reduced only by a LIMIT or a
	// GROUP BY-less aggregate: its keys are not this node's to cover.
	const bool reduces_derived = limited || (aggregated_ && group_keys_.empty());
	out_.gain_uncertain = (unbounded_links_ > 0 && !reduces) || (derived_uncertain_ && !reduces_derived);
}

void NodeWriter::NoteResultFacts() {
	for (auto &output : outputs_) {
		// Computed here, or a derived table's (or inlined CTE's) column carried
		// out as it is (review of #399: one level of nesting hid the flag, and
		// a division's NULL reached DuckDB's filter above the vehicle).
		out_.value_divergence = out_.value_divergence || (output.column_index == DConstants::INVALID_INDEX
															  ? output.value.division || output.value.approximate
															  : divergent_of_[output.column_index]);
	}
	for (auto &relation : relations_) {
		out_.total_input_rows += relation.derived ? relation.total_rows : relation.table.approx_rows;
		out_.total_size_unknown =
			out_.total_size_unknown || (relation.derived ? relation.total_unknown : !relation.table.size_known);
		if (relation.semi) {
			continue;
		}
		out_.largest_input_rows = MaxValue(out_.largest_input_rows, relation.table.approx_rows);
		out_.input_size_unknown = out_.input_size_unknown || !relation.table.size_known;
	}
}

bool NodeWriter::Write(const SelectNode &node) {
	if (node.qualify && !qualifying_) {
		// Before the CTEs: the node's own writer registers them, once (review
		// of E2: registered on the wrapper too, a body saw itself).
		return WriteQualified(node);
	}
	std::string from_sql;
	SelectModifiers modifiers;
	if (!BeginSelect(node) || !WriteRelations(node, from_sql) || !ReadModifiers(node, modifiers)) {
		return false;
	}
	const bool limited = modifiers.Limited();
	const bool top = limited && modifiers.offset_value == 0;

	// An aggregate query: grouped, filtered on groups, or aggregating --
	// anywhere after grouping, ORDER BY included, as DuckDB's binder decides.
	std::string group_sql;
	if (!WriteGroups(node, group_sql)) {
		return false;
	}
	NoteAggregates(node, modifiers.order);

	out_.statement = std::string("SELECT ") + (distinct_ ? "DISTINCT " : "") +
					 (top ? "TOP (" + std::to_string(modifiers.limit_value) + ") " : "");
	after_grouping_ = true;
	if (!WriteSelectList(node) || (qualifying_ && !WriteQualifyColumn(node)) ||
		(distinct_ && !CheckDistinctOutputs())) {
		return false;
	}
	after_grouping_ = false;
	out_.statement += " FROM " + from_sql;
	if (!WriteWhere(node)) {
		return false;
	}
	after_grouping_ = true;
	if (!WriteGroupByHaving(node, group_sql) || !WriteOrderAndPaging(modifiers)) {
		return false;
	}
	CountUnboundedLinks(node);
	NoteGain(limited);
	NoteResultFacts();
	return true;
}

bool NodeWriter::WriteQueryNode(const QueryNode &node) {
	if (node.type == QueryNodeType::SELECT_NODE) {
		return Write(node.Cast<SelectNode>());
	}
	if (node.type != QueryNodeType::SET_OPERATION_NODE) {
		return Veto("a recursive CTE or another query node");
	}
	if (!node.cte_map.map.empty()) {
		return Veto("a WITH clause");
	}
	if (node.modifiers.empty()) {
		return WriteSetOperation(node.Cast<SetOperationNode>());
	}
	// ORDER BY / LIMIT over a set operation: `SELECT * FROM (<set operation>)`
	// with them -- the names are the first child's either way, and the NULL
	// placement a nullable key needs is an expression, which T-SQL refuses in
	// a set operation's own ORDER BY (104).
	auto select = make_uniq<SelectNode>();
	for (auto &modifier : node.modifiers) {
		if (modifier->type == ResultModifierType::ORDER_MODIFIER) {
			for (auto &order : modifier->Cast<OrderModifier>().orders) {
				// A set operation is ordered by its result columns only (DuckDB's
				// binder refuses an expression there; the wrapper would take it).
				auto &key = *order.expression;
				const bool column = key.GetExpressionClass() == ExpressionClass::COLUMN_REF &&
									key.Cast<ColumnRefExpression>().ColumnNames().size() == 1;
				const bool position = key.GetExpressionClass() == ExpressionClass::CONSTANT;
				if (!column && !position) {
					return Veto("ORDER BY " + key.ToString() + " over a set operation");
				}
			}
		}
		select->modifiers.push_back(modifier->Copy());
	}
	select->select_list.push_back(make_uniq<StarExpression>());
	// The set operation itself is written from `node` (see CollectDerived);
	// the placeholder only gives the subquery a node.
	auto statement = make_uniq<SelectStatement>();
	statement->node = make_uniq<SetOperationNode>();
	select->from_table = make_uniq<SubqueryRef>(std::move(statement));
	synthetic_ref_ = &select->from_table->Cast<SubqueryRef>();
	synthetic_setop_ = &node.Cast<SetOperationNode>();
	synthetic_node_ = std::move(select);
	return Write(*synthetic_node_);
}

}  // namespace node_writer
}  // namespace mssql
}  // namespace duckdb
