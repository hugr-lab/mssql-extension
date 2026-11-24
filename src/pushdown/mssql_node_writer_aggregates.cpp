#include "pushdown/mssql_node_writer.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "pushdown/mssql_expression_vocabulary.hpp"

namespace duckdb {
namespace mssql {
namespace node_writer {

bool NodeWriter::CheckGrouped(idx_t index) {
	if (!aggregated_ || !after_grouping_ || in_aggregate_) {
		return true;
	}
	for (auto key : group_keys_) {
		if (key == index) {
			return true;
		}
	}
	return Veto("column " + columns_[index].name + " is neither grouped nor aggregated");
}

bool NodeWriter::GroupKeyColumn(const SelectNode &node, const ParsedExpression &key, idx_t &out) {
	if (key.GetExpressionClass() == ExpressionClass::CONSTANT) {
		// GROUP BY 1: a position in the select list (the rewriter does not fold
		// group keys), which must itself be a column.
		int64_t position;
		auto &literal = key.Cast<ConstantExpression>().GetLiteral();
		if (literal.kind != LiteralKind::INTEGER || !literal.TryGetInt64(position) || position < 1 ||
			position > int64_t(node.select_list.size()) ||
			node.select_list[position - 1]->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
			return Veto("GROUP BY " + key.ToString());
		}
		return ResolveColumn(node.select_list[position - 1]->Cast<ColumnRefExpression>(), out);
	}
	if (key.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return Veto("GROUP BY an expression " + key.ToString());
	}
	auto &ref = key.Cast<ColumnRefExpression>();
	// DuckDB's binder: a column of the FROM wins over a select-list alias.
	const int found = FindColumn(ref, out);
	if (found != 0 || ref.IsQualified()) {
		return found == 1 ? true : ResolveColumn(ref, out);
	}
	for (auto &item : node.select_list) {
		if (StringUtil::CIEquals(item->GetAlias().GetIdentifierName(), ref.GetColumnName().GetIdentifierName())) {
			if (item->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
				return Veto("GROUP BY the alias of an expression " + key.ToString());
			}
			return ResolveColumn(item->Cast<ColumnRefExpression>(), out);
		}
	}
	return ResolveColumn(ref, out);
}

bool NodeWriter::WriteGroups(const SelectNode &node, std::string &sql) {
	auto &groups = node.groups;
	if (groups.group_expressions.empty()) {
		if (groups.grouping_sets.empty()) {
			return true;
		}
		if (groups.grouping_sets.size() == 1 && groups.grouping_sets[0].empty()) {
			// GROUP BY (): one row even over no rows in DuckDB, NONE on the server
			// (measured) -- an aggregate without GROUP BY is the one row there.
			aggregated_ = true;
			return true;
		}
		return Veto("GROUPING SETS / ROLLUP / CUBE");
	}
	// A plain GROUP BY a, b is ONE grouping set holding every key.
	if (groups.grouping_sets.size() != 1 || groups.grouping_sets[0].size() != groups.group_expressions.size()) {
		return Veto("GROUPING SETS / ROLLUP / CUBE");
	}
	aggregated_ = true;
	for (auto &key : groups.group_expressions) {
		idx_t index;
		if (!GroupKeyColumn(node, *key, index)) {
			return false;
		}
		if (!IsGroupable(columns_[index])) {
			return Veto("GROUP BY " + columns_[index].name + ": the server does not group " +
						columns_[index].sql_type_name + " as DuckDB does");
		}
		group_keys_.push_back(index);
		sql += (sql.empty() ? "" : ", ") + ColumnSql(index);
	}
	return true;
}

// `agg(x) FILTER (WHERE c)` is `agg(CASE WHEN c THEN x END)`: a row the
// condition does not take (false or NULL) gives the aggregate a NULL, which
// every aggregate here skips -- measured equal for COUNT(*), COUNT(DISTINCT),
// SUM and MAX (spec 079 PR E2). No filter: the value as it is.
static std::string FilteredValue(const std::string &filter, const std::string &value) {
	return filter.empty() ? value : "CASE WHEN " + filter + " THEN " + value + " END";
}

//! A count as a result: BIGINT, never NULL, ordered as DuckDB orders it.
static void CountResult(Operand &out) {
	out.type = out.result_type = LogicalType::BIGINT;
	out.orderable = out.not_null = true;
}

//! The rows (COUNT(*), count(1)), those the filter takes when there is one.
static void RowCount(const AggregateCall &call, const std::string &filter, Operand &out) {
	out.sql = (filter.empty() ? "COUNT_BIG(*)" : "COUNT_BIG(" + FilteredValue(filter, "1") + ")") + call.over;
	CountResult(out);
}

bool NodeWriter::CheckAggregateCall(const AggregateCall &call, AggregateKind kind) {
	if (!after_grouping_) {
		return Veto("an aggregate in WHERE / ON");
	}
	if (in_aggregate_) {
		return Veto("an aggregate inside an aggregate");
	}
	const bool ordered = call.orders && !call.orders->empty();
	if ((ordered && kind != AggregateKind::StringAgg) || call.foreign) {
		return Veto("aggregate " + call.text);
	}
	if (!call.over.empty() && (call.distinct || kind == AggregateKind::StringAgg)) {
		// Neither takes an OVER clause there (10759, 4113).
		return Veto("aggregate " + call.text + " over a window");
	}
	return true;
}

bool NodeWriter::WriteAggregateFilter(const AggregateCall &call, std::string &filter) {
	if (!call.filter) {
		return true;
	}
	// The condition is a row's, like the argument.
	in_aggregate_ = call.over.empty();
	const bool written = WritePredicate(*call.filter, filter);
	in_aggregate_ = false;
	return written;
}

bool NodeWriter::WriteAggregate(const AggregateCall &call, AggregateKind kind, Operand &out) {
	// A window's aggregate (PR E2) reads the values of its node's rows -- the
	// groups' in an aggregate query -- so its argument is written as they are,
	// and every aggregate call it renders gets the OVER clause.
	std::string filter;
	if (!CheckAggregateCall(call, kind) || !WriteAggregateFilter(call, filter)) {
		return false;
	}
	auto &args = call.args;
	out = Operand();
	out.kind = ComparableKind::ExactNumeric;
	if (kind == AggregateKind::CountStar) {
		if (!args.empty()) {
			return Veto("aggregate " + call.text);
		}
		RowCount(call, filter, out);
		return true;
	}
	const bool separated = kind == AggregateKind::StringAgg && args.size() == 2;
	if ((args.size() != 1 && !separated) || args[0].HasName() || (separated && args[1].HasName())) {
		return Veto("aggregate " + call.text);
	}
	Operand arg;
	in_aggregate_ = call.over.empty();
	const bool written = WriteValue(args[0].GetExpression(), arg);
	in_aggregate_ = false;
	if (!written) {
		return false;
	}
	if (kind == AggregateKind::StringAgg) {
		return WriteStringAgg(call, arg, filter, out);
	}
	if (arg.constant) {
		return WriteCountOfConstant(call, kind, *arg.constant, filter, out);
	}
	arg.sql = FilteredValue(filter, arg.sql);
	if (arg.division) {
		// A zero divisor is inf / NaN in DuckDB's sum and a NULL the server's
		// skips: not the recorded divergence of a value any more.
		return Veto("a division under an aggregate");
	}
	if (arg.cast_result) {
		// A value whose DuckDB type is not the wire's (a derived table's integer
		// SUM: HUGEINT here, decimal(38,0) there) would aggregate into the
		// server's type.
		return Veto("an aggregate over a value cast back after the read");
	}
	if (kind == AggregateKind::Count) {
		return WriteCount(call, arg, out);
	}
	if (kind == AggregateKind::Min || kind == AggregateKind::Max) {
		return WriteMinMax(call, kind, arg, out);
	}
	return WriteNumericAggregate(call, kind, arg, out);
}

bool NodeWriter::WriteCountOfConstant(const AggregateCall &call, AggregateKind kind, const ParsedExpression &constant,
									  const std::string &filter, Operand &out) {
	// count(1): the rows. Any other aggregate of a constant stays DuckDB's.
	if (kind != AggregateKind::Count || call.distinct || constant.GetExpressionClass() != ExpressionClass::CONSTANT ||
		constant.Cast<ConstantExpression>().GetLiteral().IsNull()) {
		return Veto("aggregate of a constant " + call.text);
	}
	RowCount(call, filter, out);
	return true;
}

bool NodeWriter::WriteCount(const AggregateCall &call, const Operand &arg, Operand &out) {
	if (call.distinct && (arg.column ? !IsGroupable(*arg.column) : arg.kind == ComparableKind::None)) {
		return Veto("count(DISTINCT) over " + arg.type.ToString());
	}
	const auto type = arg.column ? StringUtil::Lower(arg.column->sql_type_name) : std::string();
	if (type == "text" || type == "ntext" || type == "image") {
		// The server's COUNT refuses them (8117, measured in PR E2).
		return Veto("count over " + type);
	}
	// A geometry's COUNT is the server's (owner, 2026-11-20): it counts a curve, NULL here (spec 093 T5).
	out.sql = "COUNT_BIG(" + std::string(call.distinct ? "DISTINCT " : "") + arg.sql + ")" + call.over;
	CountResult(out);
	return true;
}

bool NodeWriter::WriteMinMax(const AggregateCall &call, AggregateKind kind, const Operand &arg, Operand &out) {
	// The order's extremes: only a column the server orders as DuckDB does
	// (#362) -- no string, whose order is the collation's. DISTINCT changes
	// nothing.
	if (!arg.column || !arg.column->OrdersLikeDuckDB()) {
		return Veto("min / max over " + (arg.column ? arg.column->sql_type_name : arg.type.ToString()) +
					": the server does not order it as DuckDB does");
	}
	const std::string name = kind == AggregateKind::Min ? "MIN" : "MAX";
	const idx_t index = idx_t(arg.column - columns_.data());
	out.sql = arg.kind == ComparableKind::Boolean
				  // MIN / MAX of bit is error 8117; through tinyint and back.
				  ? "CAST(" + name + "(CAST(" + arg.sql + " AS tinyint))" + call.over + " AS bit)"
				  : name + "(" + arg.sql + ")" + call.over;
	out.type = arg.type;
	out.kind = arg.kind;
	// A constant compared with it is declared from the column -- for typing
	// only: the column's nullability is not the result's.
	out.column = arg.column;
	out.result_type = types_[index];
	out.orderable = true;
	return true;
}

//! STDEV / STDEVP / VAR / VARP, by their T-SQL names.
static const char *DispersionName(AggregateKind kind) {
	switch (kind) {
	case AggregateKind::Stdev:
		return "STDEV";
	case AggregateKind::StdevP:
		return "STDEVP";
	case AggregateKind::Var:
		return "VAR";
	default:
		return "VARP";
	}
}

bool NodeWriter::WriteNumericAggregate(const AggregateCall &call, AggregateKind kind, const Operand &arg,
									   Operand &out) {
	// SUM / AVG / STDEV / VAR: an exact numeric, or a float column (or a
	// widening cast to DOUBLE).
	const bool integer = arg.kind == ComparableKind::ExactNumeric && IntegerRank(arg.type.id()) > 0;
	const bool decimal = arg.kind == ComparableKind::ExactNumeric && arg.type.id() == LogicalTypeId::DECIMAL;
	const bool floating = arg.type.id() == LogicalTypeId::DOUBLE && arg.kind == ComparableKind::Float;
	if (!integer && !decimal && !floating) {
		return Veto(call.name + " over " + arg.type.ToString());
	}
	const std::string distinct = call.distinct ? "DISTINCT " : "";
	// An integer sum as decimal(38,0): the server's own SUM of an int is an
	// int and overflows where DuckDB's HUGEINT does not, and a bigint cast
	// overflows too (measured).
	const std::string exact_sum = "SUM(" + distinct +
								  (integer ? ExpressionVocabulary::Cast(arg.sql, "decimal(38,0)") : arg.sql) + ")" +
								  call.over;
	out.type = LogicalType::DOUBLE;
	out.result_type = LogicalType::DOUBLE;
	switch (kind) {
	case AggregateKind::Sum:
		if (floating) {
			out.sql = "SUM(" + distinct + arg.sql + ")" + call.over;
			out.approximate = true;
		} else {
			out.sql = exact_sum;
			// DECIMAL(38, s) on both sides for a decimal; an integer sum is
			// HUGEINT in DuckDB, read as decimal(38,0) and cast back.
			out.type = LogicalType::DECIMAL(38, decimal ? DecimalType::GetScale(arg.type) : 0);
			out.result_type = integer ? LogicalType::HUGEINT : out.type;
			out.cast_result = integer;
			out.orderable = true;
		}
		break;
	case AggregateKind::Avg:
		// An exact sum divided as float -- bit-exact against DuckDB's average of
		// a decimal where AVG(CAST(… AS float)) is not (measured). Still a double
		// division: recorded, like a float SUM, as approximate.
		out.sql = floating ? "AVG(" + distinct + arg.sql + ")" + call.over
						   : ExpressionVocabulary::Cast(exact_sum, "float") + " / " + "COUNT_BIG(" + distinct +
								 arg.sql + ")" + call.over;
		out.approximate = true;
		break;
	default:
		out.sql = std::string(DispersionName(kind)) + "(" + distinct + arg.sql + ")" + call.over;
		out.approximate = true;
		break;
	}
	if (out.approximate && predicate_depth_ > 0) {
		// Last bits apart: which rows pass `avg(x) > 2.5` could differ.
		return Veto("a floating-point aggregate in a condition");
	}
	return true;
}

// `string_agg(x [, sep] [ORDER BY k])` as `STRING_AGG(CAST(x AS nvarchar(max)),
// sep) [WITHIN GROUP (ORDER BY k)]`. nvarchar(max): over a bounded argument the
// server's result is bounded too and fails past 8000 bytes; it also turns a
// code-page varchar into Unicode. NULLs are skipped and a group of none is
// NULL on both sides; with no ORDER BY the order is arbitrary on both sides,
// as a LIMIT without one is (owner's call, PR E2). Measured equal, PR E2.
bool NodeWriter::WriteStringAgg(const AggregateCall &call, const Operand &arg, const std::string &filter,
								Operand &out) {
	if (call.distinct) {
		return Veto("string_agg(DISTINCT): the server's STRING_AGG has none");
	}
	// A varchar / nvarchar column: char(n) is read trimmed but concatenated
	// padded there, and a computed string's text is not proven the same.
	const auto type = arg.column ? StringUtil::Lower(arg.column->sql_type_name) : std::string();
	if (arg.constant || (type != "varchar" && type != "nvarchar")) {
		return Veto("string_agg over " + (arg.column ? arg.column->sql_type_name : arg.type.ToString()));
	}
	auto &args = call.args;
	std::string separator = ",";
	if (args.size() == 2) {
		const auto &expr = args[1].GetExpression();
		if (expr.GetExpressionClass() != ExpressionClass::CONSTANT ||
			expr.Cast<ConstantExpression>().GetLiteral().kind != LiteralKind::STRING) {
			return Veto("string_agg with a separator that is not a string constant");
		}
		separator = expr.Cast<ConstantExpression>().GetLiteral().text;
	}
	std::string order;
	if (call.orders && !call.orders->empty()) {
		in_aggregate_ = true;
		const bool written = WriteSortKeys(*call.orders, order);
		in_aggregate_ = false;
		if (!written) {
			return false;
		}
		// One WITHIN GROUP order per scope (8711); an unordered one beside it
		// is accepted.
		if (string_agg_ordered_ && string_agg_order_ != order) {
			return Veto("two string_agg orders in one node: the server takes one");
		}
		string_agg_ordered_ = true;
		string_agg_order_ = order;
	}
	const std::string value = FilteredValue(filter, ExpressionVocabulary::Cast(arg.sql, "nvarchar(max)"));
	out = Operand();
	out.sql = "STRING_AGG(" + value + ", " + Parameter(nullptr, Value(separator)) + ")" +
			  (order.empty() ? "" : " WITHIN GROUP (ORDER BY " + order + ")");
	// Text that neither compares nor orders as DuckDB's would (D4): a value
	// to select, nothing more.
	out.type = out.result_type = LogicalType::VARCHAR;
	out.kind = ComparableKind::None;
	return true;
}

}  // namespace node_writer
}  // namespace mssql
}  // namespace duckdb
