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
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
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
	if (value.type != column.duckdb_type) {
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
		idx_t index = DConstants::INVALID_INDEX;
		for (idx_t c = 0; c < target_columns; c++) {
			if (!StringUtil::CIEquals(columns_[c].name, name)) {
				continue;
			}
			if (index != DConstants::INVALID_INDEX) {
				// A case-sensitive database holding `a` and `A`.
				return Veto("SET column " + name + " matches more than one column");
			}
			index = c;
		}
		if (index == DConstants::INVALID_INDEX) {
			return Veto("SET column " + name + " is not a column of the target");
		}
		for (auto done : assigned) {
			if (done == index) {
				return Veto("SET column " + name + " assigned twice");
			}
		}
		assigned.push_back(index);
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

}  // namespace node_writer
}  // namespace mssql
}  // namespace duckdb
