#include "pushdown/mssql_node_writer.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "pushdown/mssql_order_term.hpp"

namespace duckdb {
namespace mssql {
namespace node_writer {

void NodeWriter::OrderDirection(const OrderByNode &node, OrderType &type, OrderByNullType &null_order) const {
	type = node.type == OrderType::ORDER_DEFAULT ? options_.default_order : node.type;
	null_order = node.null_order;
	if (null_order == OrderByNullType::ORDER_DEFAULT) {
		null_order = type == OrderType::DESCENDING ? options_.default_null_order_desc : options_.default_null_order_asc;
	}
}

//! Whether `key` is among `keys` already; it joins them when it is not. A key already sorted by cannot
//! reorder anything, and the server refuses a repeated ORDER BY item (169) where DuckDB takes it.
static bool SeenBefore(std::vector<std::string> &keys, const std::string &key) {
	for (auto &seen : keys) {
		if (seen == key) {
			return true;
		}
	}
	keys.push_back(key);
	return false;
}

bool NodeWriter::WriteSortKeyTerm(const OrderByNode &node, const Operand &key, bool is_last_key, std::string &term) {
	OrderType type;
	OrderByNullType null_order;
	OrderDirection(node, type, null_order);
	// A column key only when the key IS the column: MIN / MAX carry their
	// column for typing constants, and their result is nullable whatever
	// the column is (an empty group, a FILTER).
	if (key.column && node.expression->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		std::string fragment;
		if (!OrderKeyFragment(*key.column, key.sql, true, true, is_last_key, fragment)) {
			return Veto("an ordering by " + key.column->name + ": the server does not order " +
						key.column->sql_type_name + " as DuckDB does");
		}
		if (!OrderTerm(fragment, key.sql, type, null_order, key.column->is_nullable, true, term)) {
			return Veto("an ordering by " + key.column->name + ": NULL placement");
		}
		return true;
	}
	if (!key.orderable) {
		return Veto(key.approximate ? "an ordering by a floating-point aggregate" : "an ordering by a computed value");
	}
	if (!OrderTerm(key.sql, key.sql, type, null_order, !key.not_null, true, term)) {
		return Veto("an ordering by " + node.expression->ToString() + ": NULL placement");
	}
	return true;
}

bool NodeWriter::WriteSortKeys(const std::vector<OrderByNode> &orders, std::string &sql) {
	std::vector<std::string> keys;
	for (idx_t key_index = 0; key_index < orders.size(); key_index++) {
		auto &node = orders[key_index];
		// #556: a truncated-read key only as the last of the order.
		const bool is_last_key = key_index + 1 == orders.size();
		Operand key;
		if (!WriteValue(*node.expression, key)) {
			return false;
		}
		if (key.constant) {
			return Veto("an ordering by a constant");
		}
		if (SeenBefore(keys, key.sql)) {
			continue;
		}
		std::string term;
		if (!WriteSortKeyTerm(node, key, is_last_key, term)) {
			return false;
		}
		sql += (sql.empty() ? "" : ", ") + term;
	}
	return true;
}

bool NodeWriter::ResolveOrderKey(const ParsedExpression &key, idx_t &index, Operand &computed) {
	// DuckDB resolves an ORDER BY name against the select list first, then
	// the FROM; a bare integer is a position in the select list.
	const OutputColumn *output = nullptr;
	index = DConstants::INVALID_INDEX;
	if (key.GetExpressionClass() == ExpressionClass::CONSTANT) {
		int64_t position;
		auto &literal = key.Cast<ConstantExpression>().GetLiteral();
		if (literal.kind != LiteralKind::INTEGER || !literal.TryGetInt64(position) || position < 1 ||
			position > int64_t(outputs_.size())) {
			return Veto("ORDER BY " + key.ToString());
		}
		output = &outputs_[position - 1];
	} else if (key.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &ref = key.Cast<ColumnRefExpression>();
		if (!ref.IsQualified()) {
			for (auto &candidate : outputs_) {
				if (StringUtil::CIEquals(candidate.name, ref.GetColumnName().GetIdentifierName())) {
					output = &candidate;
					break;
				}
			}
		}
		if (!output && (!ResolveColumn(ref, index) || !CheckGrouped(index))) {
			return false;
		}
	} else {
		// An expression: an aggregate, the top-N shape (`ORDER BY count(*)
		// DESC LIMIT 10`), rendered as a value.
		if (!WriteValue(key, computed)) {
			return false;
		}
		if (computed.constant) {
			return Veto("ORDER BY " + key.ToString());
		}
	}
	if (output) {
		if (output->column_index != DConstants::INVALID_INDEX) {
			index = output->column_index;
		} else {
			computed = output->value;
		}
	}
	return true;
}

bool NodeWriter::IsNamedResultColumn(idx_t index, const Operand &computed) const {
	bool listed = false;
	for (auto &candidate : outputs_) {
		listed = listed || (index != DConstants::INVALID_INDEX ? candidate.column_index == index && candidate.plain_read
															   : candidate.column_index == DConstants::INVALID_INDEX &&
																	 candidate.value.sql == computed.sql);
	}
	return listed;
}

bool NodeWriter::WriteOrderTerm(const OrderByNode &node, idx_t index, const Operand &computed, bool limited,
								bool is_last_key, std::string &term) {
	const auto &key = *node.expression;
	OrderType type;
	OrderByNullType null_order;
	OrderDirection(node, type, null_order);
	if (index == DConstants::INVALID_INDEX) {
		if (!computed.orderable) {
			return Veto(computed.approximate ? "ORDER BY a floating-point aggregate" : "ORDER BY a computed column");
		}
		// Under DISTINCT the term must be the key itself: a CASE placing NULLs
		// is not a result column (145).
		if (!OrderTerm(computed.sql, computed.sql, type, null_order, !computed.not_null, limited && !distinct_, term)) {
			return Veto("ORDER BY " + key.ToString() + ": NULL placement needs a LIMIT to be emulated");
		}
		return true;
	}
	const auto &column = columns_[index];
	// Qualified (OrderColumnSql): DuckDB's resolution above already chose
	// the table column, which a bare T-SQL name would not.
	const std::string column_ref = OrderColumnSql(index);
	std::string fragment;
	// Under DISTINCT neither the bytes key nor the NULL-placing CASE is a
	// result column (145): the key must be sent as it is.
	if (!OrderKeyFragment(column, column_ref, true, limited && !distinct_, is_last_key, fragment)) {
		return Veto("ORDER BY " + column.name + ": the server does not order " + column.sql_type_name +
					" as DuckDB does");
	}
	if (!OrderTerm(fragment, column_ref, type, null_order, column.is_nullable, limited && !distinct_, term)) {
		return Veto("ORDER BY " + column.name + ": NULL placement needs a LIMIT to be emulated");
	}
	return true;
}

bool NodeWriter::WriteOrder(const OrderModifier &order, bool limited, std::string &sql) {
	// A key already sorted by is dropped (SeenBefore).
	std::vector<std::string> keys;
	for (idx_t key_index = 0; key_index < order.orders.size(); key_index++) {
		auto &node = order.orders[key_index];
		// #556: a truncated-read key only as the last of the order.
		const bool is_last_key = key_index + 1 == order.orders.size();
		const auto &key = *node.expression;
		idx_t index;
		Operand computed;
		if (!ResolveOrderKey(key, index, computed)) {
			return false;
		}
		// Under DISTINCT the key must be a result column as the server spells
		// it (145) -- a column read through a CAST is not.
		if (distinct_ && !IsNamedResultColumn(index, computed)) {
			return Veto("ORDER BY " + key.ToString() + " under DISTINCT: not a result column the server can name");
		}
		if (SeenBefore(keys, index != DConstants::INVALID_INDEX ? OrderColumnSql(index) : computed.sql)) {
			continue;
		}
		std::string term;
		if (!WriteOrderTerm(node, index, computed, limited, is_last_key, term)) {
			return false;
		}
		sql += (sql.empty() ? "" : ", ") + term;
	}
	return true;
}

}  // namespace node_writer
}  // namespace mssql
}  // namespace duckdb
