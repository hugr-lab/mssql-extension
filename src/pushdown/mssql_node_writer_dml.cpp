//===----------------------------------------------------------------------===//
// Spec 080 D1 / PR 3c: an UPDATE / DELETE node as ONE T-SQL statement.
//
//   SQL Server, Azure SQL   UPDATE [r1] SET [c] = e FROM [s].[t] AS [r1]
//                             [CROSS JOIN [s].[u] AS [r2]] WHERE p
//                           DELETE [r1] FROM [s].[t] AS [r1] [CROSS JOIN …] WHERE p
//   Fabric (no FROM form)   UPDATE [s].[t] SET [c] = e WHERE p
//                           DELETE FROM [s].[t] WHERE p
//
// The WHERE is the SELECT writer's predicate, whole or not at all: a DML's
// condition is never pushed in a relaxed form -- extra rows written are not
// harmless (D2). A SET value is computed and typed as DuckDB writes it into
// the column (DuckDB casts to the column's type, T-SQL converts implicitly and
// truncates where DuckDB rounds), so it must already have the column's type, or
// be a constant DuckDB would type the same way. A SET of a key column is
// pushed: T-SQL updates key columns, only the fallback needs the key fixed.
//===----------------------------------------------------------------------===//

#include "pushdown/mssql_node_writer.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "pushdown/mssql_expression_vocabulary.hpp"

namespace duckdb {
namespace mssql {
namespace node_writer {

namespace {

bool IsNullConstant(const ParsedExpression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::CONSTANT &&
		   expr.Cast<ConstantExpression>().GetLiteral().kind == LiteralKind::NULL_LITERAL;
}

//! Two columns the server declares alike: a value of one is written into the
//! other with nothing truncated, padded, rounded or recoded. Several SQL
//! Server types share one DuckDB type (datetime and datetime2(6) are both
//! TIMESTAMP, time(6) and time(7) both TIME...), and the server converts
//! between them its own way -- datetime2(6) <- datetime rounds .007 to
//! .006667 where DuckDB reads .006666 (review of spec 080 PR 3c).
bool SameDeclaration(const MSSQLColumnInfo &a, const MSSQLColumnInfo &b) {
	return StringUtil::CIEquals(a.sql_type_name, b.sql_type_name) && a.max_length == b.max_length &&
		   a.precision == b.precision && a.scale == b.scale && StringUtil::CIEquals(a.collation_name, b.collation_name);
}

//! A value of type `from` written into a column of type `to` as DuckDB
//! writes it: the same type, or an integer widened (exact on both sides --
//! tinyint is UTINYINT, 0..255, so it widens into smallint too).
bool WritesAsIs(const LogicalType &from, const LogicalType &to) {
	if (from == to) {
		return true;
	}
	const int from_rank = IntegerRank(from.id());
	const int to_rank = IntegerRank(to.id());
	return from_rank > 0 && to_rank > 0 && from_rank <= to_rank;
}

//! An aggregate or a window function anywhere in `expr`: DuckDB's binder
//! refuses one in SET or WHERE, and the server's error (157, 4108) must not
//! replace that.
bool HoldsAggregateOrWindow(const ParsedExpression &expr) {
	if (expr.IsWindow() || ContainsAggregate(expr)) {
		return true;
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		found = found || (child.GetExpressionClass() != ExpressionClass::SUBQUERY && HoldsAggregateOrWindow(child));
	});
	return found;
}

}  // namespace

bool NodeWriter::TargetColumn(const std::vector<MSSQLColumnInfo> &columns, idx_t count, const std::string &name,
							  const char *role, std::vector<idx_t> &taken, idx_t &index) {
	index = DConstants::INVALID_INDEX;
	for (idx_t c = 0; c < count; c++) {
		if (!StringUtil::CIEquals(columns[c].name, name)) {
			continue;
		}
		if (index != DConstants::INVALID_INDEX) {
			// A case-sensitive database holding `a` and `A`.
			return Veto(std::string(role) + " column " + name + " matches more than one column");
		}
		index = c;
	}
	if (index == DConstants::INVALID_INDEX) {
		return Veto(std::string(role) + " column " + name + " is not a column of the target");
	}
	for (auto done : taken) {
		if (done == index) {
			return Veto(std::string(role) + " column " + name +
						(std::string(role) == "SET" ? " assigned twice" : " named twice"));
		}
	}
	taken.push_back(index);
	return true;
}

bool NodeWriter::CollectDmlRelations(const TableRef &target, const std::vector<const TableRef *> &sources) {
	if (target.type != TableReferenceType::BASE_TABLE) {
		return Veto("the target is not a table");
	}
	std::vector<idx_t> members;
	if (!CollectRelations(target, members)) {
		return false;
	}
	if (relations_.size() != 1 || relations_[0].derived) {
		return Veto("the target is a CTE");
	}
	if (relations_[0].table.is_view) {
		// The shipped path refuses a view (no rowid); a pushed statement through
		// an updatable view would succeed where it fails.
		return Veto("the target is a view");
	}
	if (!sources.empty() && !options_.dml_from_join) {
		return Veto("FROM / USING on a platform without UPDATE / DELETE ... FROM (Fabric)");
	}
	for (auto *source : sources) {
		if (source->type != TableReferenceType::BASE_TABLE) {
			return Veto("a FROM / USING that is not a list of tables");
		}
		if (!CollectRelations(*source, members)) {
			return false;
		}
		if (relations_.back().derived) {
			return Veto("a CTE in FROM / USING");
		}
	}
	if (options_.dml_from_join) {
		// The SQL Server form names the target by its alias: every relation
		// aliased, every column qualified -- a subquery's tables are aliased
		// past these, so none can shadow them.
		for (idx_t r = 0; r < relations_.size(); r++) {
			relations_[r].sql = "[r" + std::to_string(alias_base_ + r + 1) + "]";
		}
	}
	return true;
}

bool NodeWriter::WriteDmlFrom(const TableRef &target, const std::vector<const TableRef *> &sources, std::string &sql) {
	visible_ = 0;
	if (!WriteFrom(target, sql)) {
		return false;
	}
	for (auto *source : sources) {
		std::string source_sql;
		if (!WriteFrom(*source, source_sql)) {
			return false;
		}
		sql += " CROSS JOIN " + source_sql;
	}
	visible_ = DConstants::INVALID_INDEX;
	return true;
}

bool NodeWriter::WriteSetValue(const ParsedExpression &expr, idx_t index, std::string &sql) {
	auto &column = columns_[index];
	if (expr.GetExpressionClass() == ExpressionClass::DEFAULT) {
		// DuckDB writes its bound default -- NULL for a column it knows none of
		// -- where the server would apply its own.
		return Veto("DEFAULT in SET");
	}
	if (IsNullConstant(expr)) {
		sql = "NULL";
		return true;
	}
	if (HoldsAggregateOrWindow(expr)) {
		return Veto("an aggregate or a window function in SET");
	}
	Operand value;
	if (!WriteValue(expr, value)) {
		return false;
	}
	Operand peer;
	peer.sql = ColumnSql(index);
	peer.type = column.duckdb_type;
	peer.kind = KindOf(column);
	peer.column = &column;
	if (value.constant) {
		if (peer.kind == ComparableKind::None) {
			return Veto("a constant SET into " + column.sql_type_name + " column " + column.name);
		}
		// Typed as the binder types it against the column, exact or vetoed; a
		// string must fit the column's declaration (the shipped path's overflow
		// guard and the server's truncation error agree on a longer one).
		if (!BindConstant(peer, value, true)) {
			return false;
		}
		sql = value.sql;
		return true;
	}
	if (value.division || value.approximate) {
		return Veto("SET " + column.name + " = a division or a floating-point aggregate");
	}
	if (value.type_uncertain || value.cast_result || value.type.id() == LogicalTypeId::INVALID) {
		return Veto("SET " + column.name + " = a value whose type DuckDB and the server may not agree on");
	}
	if (!WritesAsIs(value.type, column.duckdb_type)) {
		// DuckDB casts the value to the column's type (DOUBLE -> INT rounds),
		// the server converts it implicitly (and truncates).
		return Veto("SET " + column.name + " = a " + value.type.ToString() + " value into a " +
					column.duckdb_type.ToString() + " column");
	}
	// A numeric, bit or date column's DuckDB type names its server type (int,
	// bigint, decimal(p,s), float(53), bit, date), so a value of that type is
	// written as DuckDB writes it. Anywhere else -- strings, the datetime and
	// time family, binary, geometry, xml -- the server converts between
	// declarations its own way: the value must be a column declared alike.
	const bool typed_by_duckdb = peer.kind == ComparableKind::ExactNumeric || peer.kind == ComparableKind::Float ||
								 peer.kind == ComparableKind::Boolean || peer.kind == ComparableKind::Date;
	if (!typed_by_duckdb && (!value.column || !SameDeclaration(*value.column, column))) {
		return Veto("SET " + column.name + " = a value other than a constant or a column declared alike");
	}
	sql = value.sql;
	return true;
}

bool NodeWriter::WriteDmlWhere(const ParsedExpression *condition, std::string &sql) {
	sql.clear();
	if (!condition) {
		return true;
	}
	if (HoldsAggregateOrWindow(*condition)) {
		return Veto("an aggregate or a window function in WHERE");
	}
	if (!options_.dml_from_join && condition->HasSubquery()) {
		// The single-table form names the target's columns bare, which a
		// subquery's own tables could shadow.
		return Veto("a subquery in the WHERE of the single-table form (Fabric)");
	}
	return WritePredicate(*condition, sql);
}

bool NodeWriter::WriteUpdate(const UpdateQueryNode &node) {
	if (!node.returning_list.empty()) {
		// Spec 080 D1: a DML with RETURNING is not pushed -- which also keeps a
		// RETURNING CTE body's DML off the vehicle.
		return Veto("RETURNING");
	}
	if (!node.cte_map.map.empty()) {
		return Veto("a WITH clause on an UPDATE");
	}
	if (!node.modifiers.empty()) {
		return Veto("a modifier on an UPDATE");
	}
	if (node.prioritize_table_when_binding) {
		return Veto("an UPDATE bound as ALTER TABLE expressions");
	}
	if (!node.table || !node.set_info || node.set_info->columns.empty()) {
		return Veto("an UPDATE without a table or a SET");
	}
	auto &set = *node.set_info;
	std::vector<const TableRef *> sources;
	if (node.from_table) {
		sources.push_back(node.from_table.get());
	}
	if (!CollectDmlRelations(*node.table, sources)) {
		return false;
	}
	std::string from_sql;
	if (options_.dml_from_join && !WriteDmlFrom(*node.table, sources, from_sql)) {
		return false;
	}

	const idx_t target_columns = relations_[0].table.columns->size();
	std::vector<idx_t> assigned;
	std::string set_sql;
	for (idx_t i = 0; i < set.columns.size(); i++) {
		const auto &name = set.columns[i].GetIdentifierName();
		idx_t index;
		if (!TargetColumn(columns_, target_columns, name, "SET", assigned, index)) {
			return false;
		}
		if (!options_.dml_from_join && set.expressions[i]->HasSubquery()) {
			return Veto("a subquery in SET in the single-table form (Fabric)");
		}
		std::string value;
		if (!WriteSetValue(*set.expressions[i], index, value)) {
			return false;
		}
		set_sql += (set_sql.empty() ? "" : ", ") + QuoteIdentifier(columns_[index].name) + " = " + value;
	}

	std::string where;
	if (!WriteDmlWhere(set.condition.get(), where)) {
		return false;
	}
	if (!sources.empty()) {
		// T-SQL updates a target row from an ARBITRARY one of several source rows
		// that match it; pushed only when each source's unique key is equated
		// with the target's columns, so at most one matches.
		link_equalities_.clear();
		if (set.condition) {
			CollectEqualities(*set.condition);
		}
		for (idx_t r = 1; r < relations_.size(); r++) {
			if (!KeyCovered(r, 0, 1)) {
				return Veto("UPDATE ... FROM " + relations_[r].name +
							" without its unique key equated with the target's columns");
			}
		}
	}

	auto &target = relations_[0].table;
	if (options_.dml_from_join) {
		out_.statement = "UPDATE " + relations_[0].sql + " SET " + set_sql + " FROM " + from_sql;
	} else {
		out_.statement =
			"UPDATE " + QuoteIdentifier(target.schema) + "." + QuoteIdentifier(target.name) + " SET " + set_sql;
	}
	if (!where.empty()) {
		out_.statement += " WHERE " + where;
	}
	out_.dml_kind = "update";
	out_.target_schema = target.schema;
	out_.target_table = target.name;
	return true;
}

bool NodeWriter::WriteDelete(const DeleteQueryNode &node) {
	if (!node.returning_list.empty()) {
		return Veto("RETURNING");
	}
	if (!node.cte_map.map.empty()) {
		return Veto("a WITH clause on a DELETE");
	}
	if (!node.modifiers.empty()) {
		return Veto("a modifier on a DELETE");
	}
	if (!node.table) {
		return Veto("a DELETE without a table");
	}
	std::vector<const TableRef *> sources;
	for (auto &using_clause : node.using_clauses) {
		sources.push_back(using_clause.get());
	}
	if (!CollectDmlRelations(*node.table, sources)) {
		return false;
	}
	std::string from_sql;
	if (options_.dml_from_join && !WriteDmlFrom(*node.table, sources, from_sql)) {
		return false;
	}
	// A target row several USING rows match is deleted once, by DuckDB and by
	// the server alike.
	std::string where;
	if (!WriteDmlWhere(node.condition.get(), where)) {
		return false;
	}
	auto &target = relations_[0].table;
	if (options_.dml_from_join) {
		out_.statement = "DELETE " + relations_[0].sql + " FROM " + from_sql;
	} else {
		out_.statement = "DELETE FROM " + QuoteIdentifier(target.schema) + "." + QuoteIdentifier(target.name);
	}
	if (!where.empty()) {
		out_.statement += " WHERE " + where;
	}
	out_.dml_kind = "delete";
	out_.target_schema = target.schema;
	out_.target_table = target.name;
	return true;
}

//===----------------------------------------------------------------------===//
// Spec 080 PR 3d: INSERT INTO [s].[t] ([c], ...) <SELECT>
//
// Both sides on the server, so no row crosses the wire. The SELECT is the
// SELECT writer's, with its columns written as themselves (raw_columns_): the
// value never leaves the server, so the read expression a client needs (a
// code-page varchar's NVARCHAR cast, geometry's STAsBinary) is not applied.
// Each value must reach its column as DuckDB's INSERT would write it -- the
// SET rule of PR 3c -- and an omitted column gets the server's DEFAULT on
// both paths (the shipped INSERT names only the named columns, measured).
//===----------------------------------------------------------------------===//

bool NodeWriter::WriteInsert(const InsertQueryNode &node, const BaseTableRef &target) {
	if (!node.returning_list.empty()) {
		return Veto("RETURNING");
	}
	if (node.on_conflict_info) {
		return Veto("ON CONFLICT");
	}
	if (node.default_values) {
		return Veto("DEFAULT VALUES");
	}
	if (node.column_order != InsertColumnOrder::INSERT_BY_POSITION) {
		return Veto("INSERT BY NAME");
	}
	if (!node.cte_map.map.empty() || !node.modifiers.empty()) {
		return Veto("a WITH clause or a modifier on an INSERT");
	}
	if (!node.select_statement || !node.select_statement->node) {
		return Veto("an INSERT without a SELECT");
	}
	if (node.GetValuesList()) {
		// The rows come from DuckDB: the shipped path's business.
		return Veto("INSERT ... VALUES");
	}
	const auto &select = *node.select_statement->node;
	if (select.type != QueryNodeType::SELECT_NODE) {
		return Veto("an INSERT from a set operation");
	}

	WriterTable table;
	if (!resolver_(target, table) || !table.columns) {
		return Veto("the target does not resolve in this catalog");
	}
	const auto &target_columns = *table.columns;
	std::vector<idx_t> into;
	if (node.columns.empty()) {
		for (idx_t c = 0; c < target_columns.size(); c++) {
			into.push_back(c);
		}
	} else {
		for (auto &name : node.columns) {
			idx_t index;
			if (!TargetColumn(target_columns, target_columns.size(), name.GetIdentifierName(), "INSERT", into, index)) {
				return false;
			}
		}
	}

	WrittenQuery inner_out;
	NodeWriter inner(*this, inner_out);
	inner.raw_columns_ = true;
	if (!inner.WriteQueryNode(select)) {
		return false;
	}
	const auto &outputs = inner.Outputs();
	if (outputs.size() != into.size()) {
		return Veto("the SELECT gives " + std::to_string(outputs.size()) + " columns for " +
					std::to_string(into.size()));
	}
	for (idx_t i = 0; i < into.size(); i++) {
		const auto &column = target_columns[into[i]];
		const auto &output = outputs[i];
		const auto kind = KindOf(column);
		const bool typed_by_duckdb = kind == ComparableKind::ExactNumeric || kind == ComparableKind::Float ||
									 kind == ComparableKind::Boolean || kind == ComparableKind::Date;
		if (i < inner_out.cast_types.size() && inner_out.cast_types[i].id() != LogicalTypeId::INVALID) {
			return Veto("INSERT column " + column.name + ": a value DuckDB casts after the read");
		}
		if (output.column_index != DConstants::INVALID_INDEX) {
			const auto &source = inner.FlatColumn(output.column_index);
			if (typed_by_duckdb ? !WritesAsIs(source.duckdb_type, column.duckdb_type)
								: (inner.already_read_[output.column_index] || !SameDeclaration(source, column))) {
				// Beyond numerics, bit and date, only a column declared alike
				// is written as DuckDB writes it (see WriteSetValue).
				return Veto("INSERT column " + column.name + " from " + source.sql_type_name + " column " +
							source.name);
			}
			continue;
		}
		const auto &value = output.value;
		const auto type = value.result_type.id() != LogicalTypeId::INVALID ? value.result_type : value.type;
		if (!typed_by_duckdb || value.division || value.approximate || value.type_uncertain || value.cast_result ||
			!WritesAsIs(type, column.duckdb_type)) {
			return Veto("INSERT column " + column.name + " from a computed value");
		}
	}

	std::string column_list;
	for (auto index : into) {
		column_list += (column_list.empty() ? "" : ", ") + QuoteIdentifier(target_columns[index].name);
		out_.identity_insert = out_.identity_insert || (target_columns[index].is_identity && !table.is_view);
	}
	out_.statement = "INSERT INTO " + QuoteIdentifier(table.schema) + "." + QuoteIdentifier(table.name) + " (" +
					 column_list + ") " + inner_out.statement;
	out_.dml_kind = "insert";
	out_.target_schema = table.schema;
	out_.target_table = table.name;
	return true;
}

//===----------------------------------------------------------------------===//
// Spec 080 PR 4a (D4): a MERGE whose source is in the same catalog, as ONE
// T-SQL statement -- the rewriter hands the node over whole when every table
// it names is in this catalog.
//
//   MERGE [s].[t] AS [r1] USING <source> AS [r2] ON c
//   WHEN MATCHED [AND c] THEN UPDATE SET [col] = v, … | DELETE
//   WHEN NOT MATCHED [BY TARGET] [AND c] THEN INSERT ([col], …) VALUES (v, …)
//   WHEN NOT MATCHED BY SOURCE [AND c] THEN UPDATE SET … | DELETE;
//
// DuckDB takes any number of WHEN clauses of each kind and applies the first
// whose condition holds. T-SQL takes at most two WHEN MATCHED (one UPDATE, one
// DELETE, the first conditional), one WHEN NOT MATCHED BY TARGET (an INSERT)
// and two WHEN NOT MATCHED BY SOURCE -- the same first-match rule within that
// shape. Anything beyond it is refused by name, nothing sent: the statement
// keeps the native path (until PR 4b). A trailing DO NOTHING is dropped (no
// later clause it could take rows from); one before another clause is
// refused. A target row matched by several source rows is an error on both
// sides (8672; DuckDB's own), and the count is the MERGE's DONE count.
//===----------------------------------------------------------------------===//

namespace {

const char *WhenSql(MergeActionCondition when) {
	switch (when) {
	case MergeActionCondition::WHEN_MATCHED:
		return "WHEN MATCHED";
	case MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET:
		return "WHEN NOT MATCHED BY TARGET";
	default:
		return "WHEN NOT MATCHED BY SOURCE";
	}
}

bool HoldsSubquery(const MergeIntoAction &action) {
	if (action.condition && action.condition->HasSubquery()) {
		return true;
	}
	if (action.update_info) {
		for (auto &value : action.update_info->expressions) {
			if (value->HasSubquery()) {
				return true;
			}
		}
	}
	for (auto &value : action.expressions) {
		if (value->HasSubquery()) {
			return true;
		}
	}
	return false;
}

}  // namespace

bool NodeWriter::WriteMergeInsert(const MergeIntoAction &action, std::string &sql) {
	if (action.default_values) {
		// The server's defaults, as the shipped INSERT naming no column gets them.
		sql = "INSERT DEFAULT VALUES";
		return true;
	}
	if (action.column_order != InsertColumnOrder::INSERT_BY_POSITION) {
		return Veto("INSERT BY NAME in a MERGE");
	}
	if (!action.exclude_columns.empty()) {
		return Veto("INSERT * EXCLUDE in a MERGE");
	}
	if (action.expressions.empty()) {
		return Veto("INSERT * in a MERGE");
	}
	const idx_t target_columns = relations_[0].table.columns->size();
	std::vector<idx_t> into;
	if (action.insert_columns.empty()) {
		for (idx_t c = 0; c < target_columns; c++) {
			into.push_back(c);
		}
	} else {
		for (auto &name : action.insert_columns) {
			idx_t index;
			if (!TargetColumn(columns_, target_columns, name.GetIdentifierName(), "INSERT", into, index)) {
				return false;
			}
		}
	}
	if (into.size() != action.expressions.size()) {
		return Veto("a MERGE INSERT with " + std::to_string(action.expressions.size()) + " values for " +
					std::to_string(into.size()) + " columns");
	}
	std::string columns_sql;
	std::string values_sql;
	for (idx_t i = 0; i < into.size(); i++) {
		const auto &column = columns_[into[i]];
		if (column.is_identity) {
			// No IDENTITY_INSERT bracket inside a MERGE (D4).
			return Veto("a MERGE INSERT naming the identity column " + column.name);
		}
		std::string value;
		if (!WriteSetValue(*action.expressions[i], into[i], value)) {
			return false;
		}
		columns_sql += (i ? ", " : "") + QuoteIdentifier(column.name);
		values_sql += (i ? ", " : "") + value;
	}
	sql = "INSERT (" + columns_sql + ") VALUES (" + values_sql + ")";
	return true;
}

bool NodeWriter::WriteMergeAction(const MergeIntoAction &action, MergeActionCondition when, std::string &sql) {
	// Which side an action's rows have: a NOT MATCHED BY TARGET row has no
	// target row, a NOT MATCHED BY SOURCE row no source row.
	if (when == MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET) {
		merge_forbidden_first_ = 0;
		merge_forbidden_last_ = 1;
	} else if (when == MergeActionCondition::WHEN_NOT_MATCHED_BY_SOURCE) {
		merge_forbidden_first_ = 1;
		merge_forbidden_last_ = relations_.size();
	}
	// USING (c): an unqualified c is the merged column DuckDB's binder makes.
	// Its join puts the source on the left and the target on the right
	// (bind_merge_into.cpp), and the merged column is the left one for an INNER
	// or LEFT join, the right one for a RIGHT join, and has no single side for
	// a FULL one (SetPrimaryBinding, bind_joinref.cpp). So: the source in WHEN
	// NOT MATCHED BY TARGET and in WHEN MATCHED without a BY SOURCE clause, the
	// target in BY SOURCE and in WHEN MATCHED beside a BY SOURCE clause alone;
	// beside both (FULL) it is not mapped, and an unqualified c is vetoed as
	// ambiguous. The two sides are only collation-equal, so the side matters
	// (review of #473).
	const bool by_source = merge_has_by_source_;
	const bool by_target = merge_has_by_target_;
	for (auto &merged : merge_using_) {
		if (when == MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET) {
			using_columns_.emplace_back(merged.name, merged.source);
		} else if (when == MergeActionCondition::WHEN_NOT_MATCHED_BY_SOURCE) {
			using_columns_.emplace_back(merged.name, merged.target);
		} else if (!by_source) {
			using_columns_.emplace_back(merged.name, merged.source);
		} else if (!by_target) {
			using_columns_.emplace_back(merged.name, merged.target);
		}
	}
	struct Reset {
		NodeWriter &writer;
		~Reset() {
			writer.merge_forbidden_first_ = DConstants::INVALID_INDEX;
			writer.merge_forbidden_last_ = DConstants::INVALID_INDEX;
			writer.using_columns_.clear();
		}
	} reset{*this};

	if (HoldsSubquery(action)) {
		return Veto("a subquery in a MERGE action");
	}
	sql = WhenSql(when);
	if (action.condition) {
		if (HoldsAggregateOrWindow(*action.condition)) {
			return Veto("an aggregate or a window function in a MERGE condition");
		}
		std::string condition;
		if (!WritePredicate(*action.condition, condition)) {
			return false;
		}
		sql += " AND " + condition;
	}
	sql += " THEN ";
	switch (action.action_type) {
	case MergeActionType::MERGE_DELETE:
		sql += "DELETE";
		return true;
	case MergeActionType::MERGE_INSERT: {
		std::string insert;
		if (!WriteMergeInsert(action, insert)) {
			return false;
		}
		sql += insert;
		return true;
	}
	case MergeActionType::MERGE_UPDATE: {
		if (!action.update_info || action.update_info->columns.empty()) {
			return Veto("UPDATE SET * in a MERGE");
		}
		auto &set = *action.update_info;
		if (set.condition) {
			return Veto("a WHERE on a MERGE UPDATE");
		}
		const idx_t target_columns = relations_[0].table.columns->size();
		std::vector<idx_t> assigned;
		std::string set_sql;
		for (idx_t i = 0; i < set.columns.size(); i++) {
			const auto &name = set.columns[i].GetIdentifierName();
			idx_t index;
			if (!TargetColumn(columns_, target_columns, name, "SET", assigned, index)) {
				return false;
			}
			std::string value;
			if (!WriteSetValue(*set.expressions[i], index, value)) {
				return false;
			}
			set_sql += (set_sql.empty() ? "" : ", ") + QuoteIdentifier(columns_[index].name) + " = " + value;
		}
		sql += "UPDATE SET " + set_sql;
		return true;
	}
	case MergeActionType::MERGE_ERROR:
		return Veto("an ERROR action in a MERGE");
	default:
		return Veto("a MERGE action T-SQL has no form for");
	}
}

bool NodeWriter::WriteMerge(const MergeQueryNode &node) {
	if (!options_.merge) {
		return Veto("MERGE on a platform without it");
	}
	if (!node.returning_list.empty()) {
		return Veto("RETURNING");
	}
	if (!node.cte_map.map.empty()) {
		return Veto("a WITH clause on a MERGE");
	}
	if (!node.modifiers.empty()) {
		return Veto("a modifier on a MERGE");
	}
	if (!node.target || !node.source) {
		return Veto("a MERGE without a target or a source");
	}
	if (node.target->type != TableReferenceType::BASE_TABLE) {
		return Veto("the target is not a table");
	}
	std::vector<idx_t> members;
	if (!CollectRelations(*node.target, members)) {
		return false;
	}
	if (relations_.size() != 1 || relations_[0].derived) {
		return Veto("the target is a CTE");
	}
	if (relations_[0].table.is_view) {
		return Veto("the target is a view");
	}
	if (!CollectRelations(*node.source, members)) {
		return false;
	}
	// Every relation aliased, every column qualified: the source's columns are
	// named next to the target's in every clause.
	for (idx_t r = 0; r < relations_.size(); r++) {
		relations_[r].sql = "[r" + std::to_string(alias_base_ + r + 1) + "]";
	}
	std::string target_sql;
	std::string source_sql;
	visible_ = 0;
	if (!WriteFrom(*node.target, target_sql) || !WriteFrom(*node.source, source_sql)) {
		return false;
	}
	visible_ = DConstants::INVALID_INDEX;
	if (node.source->type == TableReferenceType::JOIN) {
		// T-SQL takes a joined table source too; not in this PR.
		return Veto("a join as the MERGE source");
	}

	std::string on;
	if (node.join_condition) {
		if (node.join_condition->HasSubquery()) {
			return Veto("a subquery in the MERGE ON condition");
		}
		if (HoldsAggregateOrWindow(*node.join_condition)) {
			return Veto("an aggregate or a window function in the MERGE ON condition");
		}
		if (!WritePredicate(*node.join_condition, on)) {
			return false;
		}
	} else if (!node.using_columns.empty()) {
		// USING (c, …): the target's c equals the source's c, compared by the
		// SELECT writer's comparison rule.
		if (relations_.size() != 2) {
			return Veto("MERGE … USING (columns) over a source of several tables");
		}
		for (auto &name : node.using_columns) {
			ColumnRefExpression target_ref(name, Identifier(relations_[0].name));
			ColumnRefExpression source_ref(name, Identifier(relations_[1].name));
			MergeUsing merged;
			merged.name = name.GetIdentifierName();
			if (FindColumn(target_ref, merged.target) != 1 || FindColumn(source_ref, merged.source) != 1) {
				return Veto("USING column " + merged.name + " is not one column on both sides");
			}
			ComparisonExpression equal(ExpressionType::COMPARE_EQUAL, target_ref.Copy(), source_ref.Copy());
			std::string term;
			if (!WritePredicate(equal, term)) {
				return false;
			}
			on += (on.empty() ? "" : " AND ") + term;
			merge_using_.push_back(std::move(merged));
		}
	} else {
		return Veto("a MERGE without an ON condition");
	}

	merge_has_by_source_ = node.actions.count(MergeActionCondition::WHEN_NOT_MATCHED_BY_SOURCE) > 0;
	merge_has_by_target_ = node.actions.count(MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET) > 0;

	// T-SQL's shape, after a trailing DO NOTHING is dropped.
	std::string clauses;
	const MergeActionCondition order[] = {MergeActionCondition::WHEN_MATCHED,
										  MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET,
										  MergeActionCondition::WHEN_NOT_MATCHED_BY_SOURCE};
	for (auto when : order) {
		auto entry = node.actions.find(when);
		if (entry == node.actions.end()) {
			continue;
		}
		std::vector<const MergeIntoAction *> actions;
		for (idx_t i = 0; i < entry->second.size(); i++) {
			const auto &action = *entry->second[i];
			if (action.action_type == MergeActionType::MERGE_DO_NOTHING) {
				if (i + 1 != entry->second.size()) {
					// Rows it holds back would reach a later clause on the server.
					return Veto(std::string("DO NOTHING before another ") + WhenSql(when) + " clause");
				}
				continue;
			}
			actions.push_back(&action);
		}
		if (when == MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET) {
			if (actions.size() > 1) {
				return Veto("more than one WHEN NOT MATCHED BY TARGET clause");
			}
			if (!actions.empty() && actions[0]->action_type != MergeActionType::MERGE_INSERT) {
				return Veto("WHEN NOT MATCHED BY TARGET without INSERT");
			}
		} else {
			if (actions.size() > 2) {
				return Veto(std::string("more than two ") + WhenSql(when) + " clauses");
			}
			// T-SQL wants the first of two clauses conditional. DuckDB's parser
			// already refuses a clause after an unconditional one of its kind
			// ("would be unreachable"), DO NOTHING included.
			if (actions.size() == 2) {
				if (actions[0]->action_type == actions[1]->action_type) {
					return Veto(std::string("two ") + WhenSql(when) + " clauses of one action");
				}
			}
			for (auto *action : actions) {
				if (action->action_type == MergeActionType::MERGE_INSERT) {
					return Veto(std::string("INSERT in ") + WhenSql(when));
				}
			}
		}
		for (auto *action : actions) {
			std::string clause;
			if (!WriteMergeAction(*action, when, clause)) {
				return false;
			}
			clauses += " " + clause;
		}
	}
	if (clauses.empty()) {
		return Veto("a MERGE with no action");
	}

	auto &target = relations_[0].table;
	out_.statement = "MERGE " + target_sql + " USING " + source_sql + " ON " + on + clauses + ";";
	out_.dml_kind = "merge";
	out_.target_schema = target.schema;
	out_.target_table = target.name;
	return true;
}

}  // namespace node_writer
}  // namespace mssql
}  // namespace duckdb
