#include "pushdown/mssql_sql_writer.hpp"

#include "catalog/mssql_column_info.hpp"
#include "codec/target_string_type.hpp"
#include "connection/mssql_settings.hpp"
#include "mssql_counters.hpp"
#include "pushdown/mssql_expression_vocabulary.hpp"
#include "pushdown/mssql_order_term.hpp"
#include "query/mssql_identifier.hpp"
#include "query/mssql_sql_params.hpp"

#include <atomic>
#include <cstdio>

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/type_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"

namespace duckdb {
namespace mssql {

std::string WrittenQuery::Declarations() const {
	std::string out;
	for (auto &param : params) {
		if (!out.empty()) {
			out += ", ";
		}
		out += "@" + param.name + " " + param.declaration;
	}
	return out;
}

SQLWriterOptions SQLWriterOptions::FromContext(ClientContext &context) {
	SQLWriterOptions options;
	options.convert_varchar_max = LoadConvertVarcharMax(context);
	options.parameterize = LoadScanParameterizeFilters(context);
	auto &config = DBConfig::GetConfig(context);
	options.default_order = config.ResolveOrder(context, OrderType::ORDER_DEFAULT);
	options.default_null_order_asc =
		config.ResolveNullOrder(context, OrderType::ASCENDING, OrderByNullType::ORDER_DEFAULT);
	options.default_null_order_desc =
		config.ResolveNullOrder(context, OrderType::DESCENDING, OrderByNullType::ORDER_DEFAULT);
	return options;
}

SQLWriter::SQLWriter(SQLWriterOptions options, TableResolver resolver)
	: options_(options), resolver_(std::move(resolver)) {}

namespace {

//! A SQL Server type whose values `=`, `<` … compare as DuckDB compares the
//! column's DuckDB type, for a constant cast to that type. PR B's list: exact
//! numerics, bit, date and the character types (D4: their sets are the
//! server's, under the column's collation). Time-of-day and the datetime
//! family wait for PR C: a constant finer than the column's precision is
//! ROUNDED into the parameter on the server and compared exactly by DuckDB
//! (the #358 lesson), which needs its own rule. text / ntext / xml / the
//! cast-required types cannot be compared with `=` at all.
enum class ComparableKind { None, Boolean, ExactNumeric, Date, String };

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
	if (type == "date") {
		return ComparableKind::Date;
	}
	if (type == "char" || type == "varchar" || type == "nchar" || type == "nvarchar") {
		return ComparableKind::String;
	}
	return ComparableKind::None;
}

//! A builtin DuckDB type named by a cast target with no modifiers: what the
//! rewriter's constant folding leaves for `DATE '2024-01-01'`.
bool BuiltinTypeOf(const TypeExpression &type, LogicalTypeId &out) {
	if (!type.GetChildren().empty() || !type.GetSchema().empty() || !type.GetCatalog().empty()) {
		return false;
	}
	out = TransformStringToLogicalTypeId(type.GetTypeName().GetIdentifierName());
	return out != LogicalTypeId::INVALID && out != LogicalTypeId::UNBOUND;
}

struct OutputColumn {
	std::string name;	 // the name DuckDB gives the result column
	idx_t column_index;	 // into the table's columns
};

class NodeWriter {
public:
	NodeWriter(const SQLWriterOptions &options, const WriterTable &table, WrittenQuery &out, std::string &why)
		: options_(options), table_(table), columns_(*table.columns), out_(out), why_(why) {}

	bool Write(const SelectNode &node);

private:
	bool Veto(const std::string &reason) {
		why_ = reason;
		return false;
	}

	//! The table column a (possibly qualified) reference names; false for a
	//! name that is not one of them -- rowid, a column of another relation --
	//! or matches two (a case-sensitive database may hold `a` and `A`).
	bool ResolveColumn(const ColumnRefExpression &ref, idx_t &out);
	bool WriteSelectList(const SelectNode &node);
	bool WritePredicate(const ParsedExpression &expr, std::string &sql);
	bool WriteComparison(const ComparisonExpression &cmp, std::string &sql);
	bool ConstantFor(const MSSQLColumnInfo &column, const ParsedExpression &expr, Value &out);
	std::string Parameter(const MSSQLColumnInfo &column, const Value &value);
	//! A column in WHERE / ORDER BY. One table: its bare name, whose meaning
	//! is the column's (collation, type). A join will qualify it here.
	std::string ColumnSql(idx_t index) const {
		return QuoteIdentifier(columns_[index].name);
	}
	bool WriteOrder(const OrderModifier &order, bool limited, std::string &sql);
	bool ConstantCount(const ParsedExpression &expr, int64_t &out);

	const SQLWriterOptions &options_;
	const WriterTable &table_;
	const std::vector<MSSQLColumnInfo> &columns_;
	std::string relation_;	// the name the FROM gives the table (alias, else its name)
	WrittenQuery &out_;
	std::string &why_;
	std::vector<OutputColumn> outputs_;
	SqlParamSet params_;
	std::vector<Value> param_values_;

public:
	//! The statement's parameters, once written.
	void TakeParams(std::vector<WrittenParam> &out) {
		for (size_t i = 0; i < params_.params.size(); i++) {
			WrittenParam param;
			param.name = params_.params[i].name;
			param.declaration = params_.params[i].declaration;
			param.value = param_values_[i];
			out.push_back(std::move(param));
		}
	}
};

bool NodeWriter::ResolveColumn(const ColumnRefExpression &ref, idx_t &out) {
	auto &names = ref.ColumnNames();
	if (names.size() > 2) {
		return Veto("column reference " + ref.ToString() + " has more than two parts");
	}
	if (names.size() == 2 && !StringUtil::CIEquals(names[0].GetIdentifierName(), relation_)) {
		return Veto("column reference " + ref.ToString() + " names another relation");
	}
	const auto &name = names.back().GetIdentifierName();
	bool found = false;
	for (idx_t i = 0; i < columns_.size(); i++) {
		if (!StringUtil::CIEquals(columns_[i].name, name)) {
			continue;
		}
		if (found) {
			return Veto("column name " + name + " matches more than one column");
		}
		found = true;
		out = i;
	}
	if (!found) {
		return Veto("column " + name + " is not a column of " + table_.name);
	}
	return true;
}

bool NodeWriter::WriteSelectList(const SelectNode &node) {
	std::string list;
	auto add = [&](idx_t index, const std::string &alias) {
		const auto &column = columns_[index];
		const std::string output = alias.empty() ? column.name : alias;
		for (auto &existing : outputs_) {
			// The vehicle's result names are the server's: two equal names would
			// come back deduplicated differently than DuckDB names them.
			if (StringUtil::CIEquals(existing.name, output)) {
				return Veto("two result columns are named " + output);
			}
		}
		if (!list.empty()) {
			list += ", ";
		}
		list += MSSQLColumnInfo::BuildReadExpression(column.name, column.sql_type_name, column.max_length,
													 column.collation_name, options_.convert_varchar_max, "", alias);
		outputs_.push_back(OutputColumn{output, index});
		out_.column_types.push_back(table_.types ? (*table_.types)[index] : column.NativeDuckDBType());
		return true;
	};
	for (auto &item : node.select_list) {
		const auto &alias = item->GetAlias().GetIdentifierName();
		switch (item->GetExpressionClass()) {
		case ExpressionClass::STAR: {
			auto &star = item->Cast<StarExpression>();
			if (star.IsColumns() || !star.ExcludeList().empty() || !star.ReplaceList().empty() ||
				!star.RenameList().empty() || star.Expression()) {
				return Veto("a * with COLUMNS / EXCLUDE / REPLACE / RENAME");
			}
			if (!star.RelationName().empty() &&
				!StringUtil::CIEquals(star.RelationName().GetIdentifierName(), relation_)) {
				return Veto("a * of another relation");
			}
			for (idx_t i = 0; i < columns_.size(); i++) {
				if (!add(i, "")) {
					return false;
				}
			}
			break;
		}
		case ExpressionClass::COLUMN_REF: {
			idx_t index;
			if (!ResolveColumn(item->Cast<ColumnRefExpression>(), index)) {
				return false;
			}
			// An alias equal to the column's own name renders as the bare column.
			const bool renamed = !alias.empty() && alias != columns_[index].name;
			if (!add(index, renamed ? alias : "")) {
				return false;
			}
			break;
		}
		default:
			return Veto("select-list expression " + item->ToString());
		}
	}
	out_.statement += list;
	return true;
}

bool NodeWriter::ConstantFor(const MSSQLColumnInfo &column, const ParsedExpression &expr, Value &out) {
	const auto kind = KindOf(column);
	if (kind == ComparableKind::None) {
		return Veto("column " + column.name + " (" + column.sql_type_name + ") is not compared on the server");
	}
	const auto &target = column.duckdb_type;
	if (expr.GetExpressionClass() == ExpressionClass::CAST) {
		// A typed literal: DuckDB compares in the cast's type, which must be the
		// column's own for the server's comparison to be the same one.
		auto &cast = expr.Cast<CastExpression>();
		LogicalTypeId id;
		if (cast.IsTryCast() || !BuiltinTypeOf(cast.TargetType(), id) || id != target.id() ||
			target.id() == LogicalTypeId::DECIMAL || cast.Child().GetExpressionClass() != ExpressionClass::CONSTANT) {
			return Veto("constant " + expr.ToString() + " against " + column.name);
		}
		auto &literal = cast.Child().Cast<ConstantExpression>().GetLiteral();
		if (literal.kind != LiteralKind::STRING) {
			return Veto("constant " + expr.ToString() + " against " + column.name);
		}
		auto cast_value = Value(literal.text).DefaultTryCastAs(target, nullptr, true);
		if (!cast_value) {
			return Veto("constant " + expr.ToString() + " does not cast to " + target.ToString());
		}
		out = std::move(*cast_value);
		return true;
	}
	if (expr.GetExpressionClass() != ExpressionClass::CONSTANT) {
		return Veto("comparison operand " + expr.ToString());
	}
	auto &literal = expr.Cast<ConstantExpression>().GetLiteral();
	switch (literal.kind) {
	case LiteralKind::STRING: {
		if (kind == ComparableKind::String) {
			out = Value(literal.text);
			return true;
		}
		// DuckDB casts a string literal to the column's type; a failed cast is
		// DuckDB's error to raise, so the statement stays local.
		auto cast_value = Value(literal.text).DefaultTryCastAs(target, nullptr, true);
		if (!cast_value) {
			return Veto("constant '" + literal.text + "' does not cast to " + target.ToString());
		}
		out = std::move(*cast_value);
		return true;
	}
	case LiteralKind::INTEGER:
	case LiteralKind::NUMERIC: {
		if (kind != ComparableKind::ExactNumeric) {
			return Veto("numeric constant against " + column.name + " (" + column.sql_type_name + ")");
		}
		// DuckDB compares in the wider of the two types; the server compares in
		// the column's. They agree exactly when the constant survives the trip
		// into the column's type and back: 2.0 against an int is 2 both ways,
		// 2.5 or 300 against a tinyint is not, and stays local.
		const Value original = literal.ToValue();
		auto cast_value = original.DefaultTryCastAs(target, nullptr, true);
		if (!cast_value) {
			return Veto("constant " + literal.text + " does not fit " + column.name);
		}
		auto back = cast_value->DefaultTryCastAs(original.type(), nullptr, true);
		if (!back || !Value::NotDistinctFrom(*back, original)) {
			return Veto("constant " + literal.text + " is not exact in " + column.name);
		}
		out = std::move(*cast_value);
		return true;
	}
	case LiteralKind::BOOLEAN:
		if (kind != ComparableKind::Boolean) {
			return Veto("boolean constant against " + column.name);
		}
		out = literal.ToValue();
		return true;
	default:
		return Veto("constant " + expr.ToString() + " against " + column.name);
	}
}

std::string NodeWriter::Parameter(const MSSQLColumnInfo &column, const Value &value) {
	// The vocabulary's constant: @pN declared from the column it is compared
	// with (076 / #361), or a literal. The vehicle carries each parameter's
	// VALUE (mssql_scan_params renders its own literal), kept beside the set.
	const size_t before = params_.params.size();
	std::string sql =
		ExpressionVocabulary::Constant(value, value.type(), options_.parameterize ? &params_ : nullptr, &column);
	if (params_.params.size() > before) {
		param_values_.push_back(value);
	}
	return sql;
}

bool NodeWriter::WriteComparison(const ComparisonExpression &cmp, std::string &sql) {
	std::string op;
	if (!ExpressionVocabulary::ComparisonOperator(cmp.GetExpressionType(), op)) {
		return Veto("comparison " + cmp.ToString());
	}
	const ParsedExpression *column_side = &cmp.Left();
	const ParsedExpression *constant_side = &cmp.Right();
	bool flipped = false;
	if (column_side->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		std::swap(column_side, constant_side);
		flipped = true;
	}
	if (column_side->GetExpressionClass() != ExpressionClass::COLUMN_REF ||
		constant_side->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		// Column against column waits for the typed operands of a later step:
		// two string columns under different collations are a server error
		// (468), and the type mixes need the constant rules' care.
		return Veto("comparison " + cmp.ToString());
	}
	idx_t index;
	if (!ResolveColumn(column_side->Cast<ColumnRefExpression>(), index)) {
		return false;
	}
	const auto &column = columns_[index];
	Value value;
	if (!ConstantFor(column, *constant_side, value)) {
		return false;
	}
	if (value.IsNull()) {
		return Veto("comparison with NULL");
	}
	const std::string column_sql = ColumnSql(index);
	const std::string constant_sql = Parameter(column, value);
	sql = flipped ? ExpressionVocabulary::Comparison(op, constant_sql, column_sql)
				  : ExpressionVocabulary::Comparison(op, column_sql, constant_sql);
	return true;
}

bool NodeWriter::WritePredicate(const ParsedExpression &expr, std::string &sql) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::CONJUNCTION: {
		auto &conj = expr.Cast<ConjunctionExpression>();
		const bool is_and = expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND;
		if (!is_and && expr.GetExpressionType() != ExpressionType::CONJUNCTION_OR) {
			return Veto("conjunction " + expr.ToString());
		}
		// All or nothing: RemoteExecute cannot leave a conjunct to DuckDB, so a
		// refused child vetoes the node (the scan path would push the AND's
		// supported part and keep the rest in its client-side net).
		std::vector<std::string> parts;
		for (auto &child : conj.GetChildren()) {
			std::string part;
			if (!WritePredicate(*child, part)) {
				return false;
			}
			parts.push_back(part);
		}
		if (parts.empty()) {
			return Veto("an empty conjunction");
		}
		sql = ExpressionVocabulary::Conjunction(parts, is_and);
		return true;
	}
	case ExpressionClass::COMPARISON:
		return WriteComparison(expr.Cast<ComparisonExpression>(), sql);
	case ExpressionClass::OPERATOR: {
		auto &op = expr.Cast<OperatorExpression>();
		auto &children = op.GetChildren();
		if (expr.GetExpressionType() == ExpressionType::OPERATOR_NOT && children.size() == 1) {
			std::string inner;
			if (!WritePredicate(*children[0], inner)) {
				return false;
			}
			// Three-valued NOT is the same on both sides: NOT UNKNOWN is UNKNOWN.
			sql = ExpressionVocabulary::Not(inner);
			return true;
		}
		if ((expr.GetExpressionType() == ExpressionType::OPERATOR_IS_NULL ||
			 expr.GetExpressionType() == ExpressionType::OPERATOR_IS_NOT_NULL) &&
			children.size() == 1 && children[0]->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			idx_t index;
			if (!ResolveColumn(children[0]->Cast<ColumnRefExpression>(), index)) {
				return false;
			}
			sql = ExpressionVocabulary::IsNull(ColumnSql(index),
											   expr.GetExpressionType() == ExpressionType::OPERATOR_IS_NOT_NULL);
			return true;
		}
		return Veto("operator " + expr.ToString());
	}
	default:
		return Veto("predicate " + expr.ToString());
	}
}

bool NodeWriter::ConstantCount(const ParsedExpression &expr, int64_t &out) {
	if (expr.GetExpressionClass() != ExpressionClass::CONSTANT) {
		return Veto("LIMIT / OFFSET " + expr.ToString() + " is not a constant");
	}
	auto &literal = expr.Cast<ConstantExpression>().GetLiteral();
	if (literal.kind != LiteralKind::INTEGER || !literal.TryGetInt64(out) || out < 0) {
		return Veto("LIMIT / OFFSET " + expr.ToString());
	}
	return true;
}

bool NodeWriter::WriteOrder(const OrderModifier &order, bool limited, std::string &sql) {
	for (auto &node : order.orders) {
		// DuckDB resolves an ORDER BY name against the select list first, then
		// the FROM; a bare integer is a position in the select list.
		idx_t index = DConstants::INVALID_INDEX;
		const auto &key = *node.expression;
		if (key.GetExpressionClass() == ExpressionClass::CONSTANT) {
			int64_t position;
			auto &literal = key.Cast<ConstantExpression>().GetLiteral();
			if (literal.kind != LiteralKind::INTEGER || !literal.TryGetInt64(position) || position < 1 ||
				position > int64_t(outputs_.size())) {
				return Veto("ORDER BY " + key.ToString());
			}
			index = outputs_[position - 1].column_index;
		} else if (key.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto &ref = key.Cast<ColumnRefExpression>();
			if (!ref.IsQualified()) {
				for (auto &output : outputs_) {
					if (StringUtil::CIEquals(output.name, ref.GetColumnName().GetIdentifierName())) {
						index = output.column_index;
						break;
					}
				}
			}
			if (index == DConstants::INVALID_INDEX && !ResolveColumn(ref, index)) {
				return false;
			}
		} else {
			return Veto("ORDER BY " + key.ToString());
		}
		const auto &column = columns_[index];
		// Qualified: a bare ORDER BY name in T-SQL is a select-list alias first
		// (measured: `SELECT [id] AS [value] ... ORDER BY [value]` sorts by id),
		// while DuckDB's resolution above already chose the table column.
		const std::string column_ref = QuoteIdentifier(table_.name) + "." + QuoteIdentifier(column.name);
		std::string fragment;
		if (!OrderKeyFragment(column, column_ref, true, limited, fragment)) {
			return Veto("ORDER BY " + column.name + ": the server does not order " + column.sql_type_name +
						" as DuckDB does");
		}
		const auto type = node.type == OrderType::ORDER_DEFAULT ? options_.default_order : node.type;
		auto null_order = node.null_order;
		if (null_order == OrderByNullType::ORDER_DEFAULT) {
			null_order =
				type == OrderType::DESCENDING ? options_.default_null_order_desc : options_.default_null_order_asc;
		}
		std::string term;
		if (!OrderTerm(fragment, column_ref, type, null_order, column.is_nullable, limited, term)) {
			return Veto("ORDER BY " + column.name + ": NULL placement needs a LIMIT to be emulated");
		}
		sql += (sql.empty() ? "" : ", ") + term;
	}
	return true;
}

bool NodeWriter::Write(const SelectNode &node) {
	if (!node.cte_map.map.empty()) {
		return Veto("a WITH clause");
	}
	if (!node.groups.group_expressions.empty() || !node.groups.grouping_sets.empty() || node.having ||
		node.aggregate_handling != AggregateHandling::STANDARD_HANDLING) {
		return Veto("GROUP BY / HAVING");
	}
	if (node.qualify || node.sample) {
		return Veto("QUALIFY / USING SAMPLE");
	}
	if (!node.from_table || node.from_table->type != TableReferenceType::BASE_TABLE) {
		return Veto("FROM is not one base table");
	}
	auto &from = node.from_table->Cast<BaseTableRef>();
	if (from.sample || !from.column_name_alias.empty() || from.at_clause) {
		return Veto("TABLESAMPLE / column aliases / AT on the table");
	}
	relation_ = from.alias.empty() ? table_.name : from.alias.GetIdentifierName();

	const OrderModifier *order = nullptr;
	const LimitModifier *limit = nullptr;
	for (auto &modifier : node.modifiers) {
		if (modifier->type == ResultModifierType::ORDER_MODIFIER && !order && !limit) {
			order = &modifier->Cast<OrderModifier>();
		} else if (modifier->type == ResultModifierType::LIMIT_MODIFIER && !limit) {
			limit = &modifier->Cast<LimitModifier>();
		} else {
			return Veto("a DISTINCT, a LIMIT % or a second ORDER BY / LIMIT");
		}
	}
	int64_t limit_value = -1;
	int64_t offset_value = 0;
	if (limit) {
		if (limit->limit_type != LimitValueType::ROW_COUNT) {
			// LIMIT 10%: no T-SQL form is proven to round as DuckDB does.
			return Veto("LIMIT as a percentage");
		}
		if (limit->limit && !ConstantCount(*limit->limit, limit_value)) {
			return false;
		}
		if (limit->offset && !ConstantCount(*limit->offset, offset_value)) {
			return false;
		}
	}
	const bool limited = limit_value >= 0;
	const bool top = limited && offset_value == 0;

	out_.statement = top ? "SELECT TOP (" + std::to_string(limit_value) + ") " : "SELECT ";
	if (!WriteSelectList(node)) {
		return false;
	}
	out_.statement += " FROM " + QuoteIdentifier(table_.schema) + "." + QuoteIdentifier(table_.name);
	if (node.where_clause) {
		std::string where;
		if (!WritePredicate(*node.where_clause, where)) {
			return false;
		}
		out_.statement += " WHERE " + where;
	}
	std::string order_sql;
	if (order && !WriteOrder(*order, limited, order_sql)) {
		return false;
	}
	if (offset_value > 0) {
		// OFFSET needs an ORDER BY in T-SQL; without one DuckDB promises no order
		// either, and `(SELECT NULL)` asks for none.
		out_.statement += " ORDER BY " + (order_sql.empty() ? std::string("(SELECT NULL)") : order_sql);
		out_.statement += " OFFSET " + std::to_string(offset_value) + " ROWS";
		if (limited) {
			out_.statement += " FETCH NEXT " + std::to_string(limit_value) + " ROWS ONLY";
		}
	} else if (!order_sql.empty()) {
		out_.statement += " ORDER BY " + order_sql;
	}
	return true;
}

}  // namespace

bool SQLWriter::Write(const QueryNode &node, WrittenQuery &out, std::string &why) {
	out = WrittenQuery();
	if (node.type != QueryNodeType::SELECT_NODE) {
		why = "not a SELECT";
		return false;
	}
	auto &select = node.Cast<SelectNode>();
	if (!select.from_table || select.from_table->type != TableReferenceType::BASE_TABLE) {
		why = "FROM is not one base table";
		return false;
	}
	WriterTable table;
	if (!resolver_(select.from_table->Cast<BaseTableRef>(), table) || !table.columns) {
		why = "the table does not resolve in this catalog";
		return false;
	}
	NodeWriter writer(options_, table, out, why);
	if (!writer.Write(select)) {
		return false;
	}
	writer.TakeParams(out.params);
	return true;
}

std::string ColumnTypeName(const LogicalType &type) {
	codec::TargetStringType spec;
	if (!codec::TryGetTargetStringType(type, spec)) {
		return type.ToString();
	}
	std::string name = spec.unicode ? "MSSQL_NVARCHAR(" : "MSSQL_VARCHAR(";
	name += codec::IsMaxLength(spec.length) ? std::string("'MAX'") : std::to_string(spec.length);
	if (!spec.collation.empty()) {
		name += ", '" + StringUtil::Replace(spec.collation, "'", "''") + "'";
	}
	return name + ")";
}

void CountRemotePushdown() {
	static std::atomic<uint64_t> statements{0};
	const auto total = ++statements;
	if (CountersEnabled()) {
		fprintf(stderr, "[MSSQL COUNTERS] remote_pushdown: statements=%llu\n", (unsigned long long)total);
	}
}

bool SQLWriter::PushesMoreThanScan(const QueryNode &node) {
	for (auto &modifier : node.modifiers) {
		if (modifier->type == ResultModifierType::ORDER_MODIFIER ||
			modifier->type == ResultModifierType::LIMIT_MODIFIER) {
			return true;
		}
	}
	return false;
}

}  // namespace mssql
}  // namespace duckdb
