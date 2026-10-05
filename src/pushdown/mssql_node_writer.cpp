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

ComparableKind KindOf(const MSSQLColumnInfo &column) {
	if (column.is_cast_required || column.is_geometry) {
		return ComparableKind::None;
	}
	const auto type = StringUtil::Lower(column.sql_type_name);
	if (type == "bit") {
		return ComparableKind::Boolean;
	}
	if (type == "tinyint" || type == "smallint" || type == "int" || type == "bigint" || type == "decimal" ||
		type == "numeric") {
		return ComparableKind::ExactNumeric;
	}
	if (type == "float" && column.duckdb_type.id() == LogicalTypeId::DOUBLE) {
		// float(53): IEEE binary64 on both sides, so +, -, * and a comparison
		// round alike (real stays out: DuckDB promotes FLOAT beside a DOUBLE).
		return ComparableKind::Float;
	}
	if (type == "date") {
		return ComparableKind::Date;
	}
	if (type == "char" || type == "varchar" || type == "nchar" || type == "nvarchar") {
		return ComparableKind::String;
	}
	return ComparableKind::None;
}

bool BuiltinTypeOf(const TypeExpression &type, LogicalTypeId &out) {
	if (!type.GetChildren().empty() || !type.GetSchema().empty() || !type.GetCatalog().empty()) {
		return false;
	}
	out = TransformStringToLogicalTypeId(type.GetTypeName().GetIdentifierName());
	return out != LogicalTypeId::INVALID && out != LogicalTypeId::UNBOUND;
}

bool AggregateFor(const std::string &function_name, AggregateKind &out) {
	static const std::pair<const char *, AggregateKind> AGGREGATES[] = {{"count_star", AggregateKind::CountStar},
																		{"count", AggregateKind::Count},
																		{"sum", AggregateKind::Sum},
																		{"avg", AggregateKind::Avg},
																		{"mean", AggregateKind::Avg},
																		{"min", AggregateKind::Min},
																		{"max", AggregateKind::Max},
																		{"stddev", AggregateKind::Stdev},
																		{"stddev_samp", AggregateKind::Stdev},
																		{"stddev_pop", AggregateKind::StdevP},
																		{"variance", AggregateKind::Var},
																		{"var_samp", AggregateKind::Var},
																		{"var_pop", AggregateKind::VarP}};
	for (auto &aggregate : AGGREGATES) {
		if (StringUtil::CIEquals(function_name, aggregate.first)) {
			out = aggregate.second;
			return true;
		}
	}
	return false;
}

bool ContainsAggregate(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		AggregateKind kind;
		if (AggregateFor(expr.Cast<FunctionExpression>().FunctionName().GetIdentifierName(), kind)) {
			return true;
		}
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(
		expr, [&](const ParsedExpression &child) { found = found || ContainsAggregate(child); });
	return found;
}

bool HasSubqueryExpression(const SelectNode &node) {
	for (auto &item : node.select_list) {
		if (item->HasSubquery()) {
			return true;
		}
	}
	for (auto &key : node.groups.group_expressions) {
		if (key->HasSubquery()) {
			return true;
		}
	}
	if ((node.where_clause && node.where_clause->HasSubquery()) || (node.having && node.having->HasSubquery())) {
		return true;
	}
	for (auto &modifier : node.modifiers) {
		if (modifier->type == ResultModifierType::ORDER_MODIFIER) {
			for (auto &order : modifier->Cast<OrderModifier>().orders) {
				if (order.expression->HasSubquery()) {
					return true;
				}
			}
		}
	}
	std::function<bool(const TableRef &)> in_conditions = [&](const TableRef &ref) -> bool {
		if (ref.type != TableReferenceType::JOIN) {
			return false;
		}
		auto &join = ref.Cast<JoinRef>();
		return (join.condition && join.condition->HasSubquery()) || in_conditions(*join.left) ||
			   in_conditions(*join.right);
	};
	return node.from_table && in_conditions(*node.from_table);
}

bool IsGroupable(const MSSQLColumnInfo &column) {
	const auto type = StringUtil::Lower(column.sql_type_name);
	if ((type == "time" || type == "datetimeoffset") && column.scale >= 7) {
		// Read as microseconds: two values 100 ns apart are two groups there and
		// one value here -- harmless for an order, not for a set.
		return false;
	}
	return KindOf(column) != ComparableKind::None || column.OrdersLikeDuckDB();
}

bool IsArithmeticType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::DECIMAL:
		return true;
	default:
		return false;
	}
}

int IntegerRank(LogicalTypeId id) {
	switch (id) {
	case LogicalTypeId::UTINYINT:
		return 1;
	case LogicalTypeId::SMALLINT:
		return 2;
	case LogicalTypeId::INTEGER:
		return 3;
	case LogicalTypeId::BIGINT:
		return 4;
	default:
		return -1;
	}
}

MSSQLColumnInfo DerivedColumn(const std::string &name, const LogicalType &type) {
	string sql_type = "derived";  // unknown to IsKnownSQLServerType: not comparable
	int16_t length = 0;
	uint8_t precision = 0;
	uint8_t scale = 0;
	switch (type.id()) {
	case LogicalTypeId::BIGINT:
		sql_type = "bigint", length = 8, precision = 19;
		break;
	case LogicalTypeId::INTEGER:
		sql_type = "int", length = 4, precision = 10;
		break;
	case LogicalTypeId::SMALLINT:
		sql_type = "smallint", length = 2, precision = 5;
		break;
	case LogicalTypeId::UTINYINT:
		sql_type = "tinyint", length = 1, precision = 3;
		break;
	case LogicalTypeId::BOOLEAN:
		sql_type = "bit", length = 1, precision = 1;
		break;
	case LogicalTypeId::DECIMAL:
		sql_type = "decimal", length = 17, precision = DecimalType::GetWidth(type), scale = DecimalType::GetScale(type);
		break;
	case LogicalTypeId::DOUBLE:
		sql_type = "float", length = 8, precision = 53;
		break;
	case LogicalTypeId::DATE:
		sql_type = "date", length = 3, precision = 10;
		break;
	default:
		break;
	}
	return MSSQLColumnInfo(name, 0, sql_type, length, precision, scale, true, "", "");
}

}  // namespace node_writer
}  // namespace mssql
}  // namespace duckdb
