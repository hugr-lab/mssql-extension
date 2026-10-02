#include "pushdown/mssql_node_writer.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "query/mssql_identifier.hpp"

namespace duckdb {
namespace mssql {
namespace node_writer {

//! The QUALIFY wrapper's hidden column.
static constexpr const char *QUALIFY_COLUMN = "mssql_qualify";

// A window function (spec 079 PR E2), measured equal against DuckDB on SQL
// Server 2025: ROW_NUMBER / RANK / DENSE_RANK / NTILE, LAG / LEAD with an
// offset and a default, FIRST_VALUE / LAST_VALUE, the aggregates OVER (with
// their FILTER), the default frame (RANGE to the current row's peers, the
// whole partition without an ORDER BY -- the same on both sides), ROWS frames,
// NULL placement emulated in the window's ORDER BY. Vetoed: DISTINCT (10759),
// RANGE / GROUPS with an offset (4194), EXCLUDE, IGNORE NULLS (SQL Server 2022
// on; the server's version is not known here), nth_value, fill.
bool NodeWriter::WriteWindow(const WindowExpression &window, Operand &out) {
	if (in_aggregate_) {
		return Veto("a window inside an aggregate");
	}
	if (!after_grouping_) {
		return Veto("a window in WHERE / ON");
	}
	if (!window.GetQualifiedName().Schema().empty() || !window.GetQualifiedName().Catalog().empty() ||
		window.IgnoreNulls() || window.WindowExclude() != WindowExcludeMode::NO_OTHER || !window.ArgOrders().empty()) {
		return Veto("window " + window.ToString());
	}
	const auto type = window.GetExpressionType();
	const bool aggregate = type == ExpressionType::WINDOW_AGGREGATE;
	if (window.Distinct() && !aggregate) {
		return Veto("window " + window.ToString());
	}
	if (window.Filter() && !aggregate) {
		return Veto("a FILTER on " + window.ToString());
	}

	// PARTITION BY: a set of groups, the server's under D4 like GROUP BY's --
	// a groupable column.
	std::string partition;
	for (auto &key : window.Partitions()) {
		Operand operand;
		if (!WriteValue(*key, operand)) {
			return false;
		}
		if (key->GetExpressionClass() != ExpressionClass::COLUMN_REF || !operand.column ||
			!IsGroupable(*operand.column)) {
			return Veto("PARTITION BY " + key->ToString());
		}
		partition += (partition.empty() ? "" : ", ") + operand.sql;
	}
	std::string order;
	if (!WriteSortKeys(window.OrderBy(), order)) {
		return false;
	}
	std::string frame;
	if (!WriteWindowFrame(window, frame)) {
		return false;
	}
	const bool framed = !frame.empty();
	// Which row a tie in the window's ORDER BY gives is the evaluation's: two
	// evaluations of one inlined CTE could number or shift it differently,
	// where DuckDB evaluates it once -- as a LIMIT picks rows (E1). RANK,
	// DENSE_RANK and the aggregates over a RANGE frame are defined by peers
	// and give the same answer whatever the order of ties.
	const bool by_rows = StringUtil::StartsWith(frame, "ROWS") || type == ExpressionType::WINDOW_ROW_NUMBER ||
						 type == ExpressionType::WINDOW_NTILE || type == ExpressionType::WINDOW_LAG ||
						 type == ExpressionType::WINDOW_LEAD || type == ExpressionType::WINDOW_FIRST_VALUE ||
						 type == ExpressionType::WINDOW_LAST_VALUE;
	out_.picks_rows = out_.picks_rows || by_rows;
	tie_windows_ += by_rows ? 1 : 0;
	if (framed && order.empty()) {
		// A ROWS / RANGE frame needs an ORDER BY there.
		return Veto("a window frame without an ORDER BY");
	}
	std::string over = " OVER (";
	if (!partition.empty()) {
		over += "PARTITION BY " + partition;
	}
	if (!order.empty()) {
		over += std::string(partition.empty() ? "" : " ") + "ORDER BY " + order;
	}
	if (framed) {
		over += " " + frame;
	}
	over += ")";

	auto &args = window.GetArguments();
	const auto &name = window.FunctionName().GetIdentifierName();
	if (aggregate) {
		AggregateKind kind;
		if (!AggregateFor(name, kind)) {
			return Veto("window " + window.ToString());
		}
		if (kind == AggregateKind::Count && args.empty()) {
			kind = AggregateKind::CountStar;  // count(*) OVER arrives as count()
		}
		const AggregateCall call{
			args, window.Distinct(), window.Filter().get(), nullptr, false, name, window.ToString(), over};
		return WriteAggregate(call, kind, out);
	}
	// The functions below need an ORDER BY there (4112) and take no frame
	// (10752) -- but FIRST_VALUE / LAST_VALUE, which take one.
	const bool takes_frame = type == ExpressionType::WINDOW_FIRST_VALUE || type == ExpressionType::WINDOW_LAST_VALUE;
	if (order.empty() || (framed && !takes_frame)) {
		return Veto("window " + window.ToString());
	}
	out = Operand();
	switch (type) {
	case ExpressionType::WINDOW_ROW_NUMBER:
	case ExpressionType::WINDOW_RANK:
	case ExpressionType::WINDOW_RANK_DENSE:
	case ExpressionType::WINDOW_NTILE: {
		std::string function = type == ExpressionType::WINDOW_ROW_NUMBER   ? "ROW_NUMBER()"
							   : type == ExpressionType::WINDOW_RANK	   ? "RANK()"
							   : type == ExpressionType::WINDOW_RANK_DENSE ? "DENSE_RANK()"
																		   : "";
		if (type == ExpressionType::WINDOW_NTILE) {
			int64_t buckets;
			if (args.size() != 1 || args[0].HasName() || !ConstantCount(args[0].GetExpression(), buckets, "NTILE") ||
				buckets < 1) {
				return Veto("window " + window.ToString());
			}
			function = "NTILE(" + std::to_string(buckets) + ")";
		} else if (!args.empty()) {
			return Veto("window " + window.ToString());
		}
		out.sql = function + over;
		out.type = out.result_type = LogicalType::BIGINT;
		out.kind = ComparableKind::ExactNumeric;
		out.orderable = out.not_null = true;
		return true;
	}
	case ExpressionType::WINDOW_LAG:
	case ExpressionType::WINDOW_LEAD:
	case ExpressionType::WINDOW_FIRST_VALUE:
	case ExpressionType::WINDOW_LAST_VALUE: {
		const bool shifts = type == ExpressionType::WINDOW_LAG || type == ExpressionType::WINDOW_LEAD;
		if (args.empty() || args.size() > (shifts ? 3u : 1u)) {
			return Veto("window " + window.ToString());
		}
		for (auto &arg : args) {
			if (arg.HasName()) {
				return Veto("window " + window.ToString());
			}
		}
		// The value, and LAG / LEAD's default typed from it as a CASE branch
		// would be (one type, a string a column of one collation, no
		// code-page varchar: it is read as it comes).
		std::vector<Operand> operands(1);
		if (!WriteValue(args[0].GetExpression(), operands[0])) {
			return false;
		}
		if (operands[0].constant) {
			return Veto("window " + window.ToString() + " over a constant");
		}
		std::string offset;
		if (args.size() >= 2) {
			int64_t rows;
			if (!ConstantCount(args[1].GetExpression(), rows, "a LAG / LEAD offset")) {
				return false;
			}
			offset = ", " + std::to_string(rows);
		}
		if (args.size() == 3) {
			operands.emplace_back();
			if (!WriteValue(args[2].GetExpression(), operands[1])) {
				return false;
			}
		}
		Operand value;
		if (!Unify(operands, value)) {
			return false;
		}
		const char *function = type == ExpressionType::WINDOW_LAG			? "LAG("
							   : type == ExpressionType::WINDOW_LEAD		? "LEAD("
							   : type == ExpressionType::WINDOW_FIRST_VALUE ? "FIRST_VALUE("
																			: "LAST_VALUE(";
		out = value;
		out.sql =
			function + operands[0].sql + offset + (operands.size() == 2 ? ", " + operands[1].sql : "") + ")" + over;
		// Another row's value, or none: nullable whatever the column is, and
		// ordered as its type orders.
		out.column = nullptr;
		out.constant = nullptr;
		out.orderable = false;
		out.not_null = false;
		return true;
	}
	default:
		return Veto("window " + window.ToString());
	}
}

// The frame as T-SQL spells it, or empty for DuckDB's default (RANGE from the
// partition's start to the current row's last peer), which is the server's
// default too. ROWS with constant offsets (a literal there: 102 on a
// parameter), RANGE only between UNBOUNDED and CURRENT ROW (4194), no GROUPS.
bool NodeWriter::WriteWindowFrame(const WindowExpression &window, std::string &sql) {
	const auto start = window.WindowStart();
	const auto end = window.WindowEnd();
	if (start == WindowBoundary::UNBOUNDED_PRECEDING && end == WindowBoundary::CURRENT_ROW_RANGE) {
		return true;
	}
	bool rows = false;
	auto bound = [&](WindowBoundary boundary, const unique_ptr<ParsedExpression> &expr, std::string &text) {
		switch (boundary) {
		case WindowBoundary::UNBOUNDED_PRECEDING:
			text = "UNBOUNDED PRECEDING";
			return true;
		case WindowBoundary::UNBOUNDED_FOLLOWING:
			text = "UNBOUNDED FOLLOWING";
			return true;
		case WindowBoundary::CURRENT_ROW_RANGE:
			text = "CURRENT ROW";
			return true;
		case WindowBoundary::CURRENT_ROW_ROWS:
			rows = true;
			text = "CURRENT ROW";
			return true;
		case WindowBoundary::EXPR_PRECEDING_ROWS:
		case WindowBoundary::EXPR_FOLLOWING_ROWS: {
			int64_t offset;
			if (!expr || !ConstantCount(*expr, offset, "a window frame offset")) {
				return false;
			}
			if (offset > NumericLimits<int32_t>::Maximum()) {
				// An int there: a larger literal is a syntax error (102, measured).
				return Veto("a window frame offset past int");
			}
			rows = true;
			text = std::to_string(offset) +
				   (boundary == WindowBoundary::EXPR_PRECEDING_ROWS ? " PRECEDING" : " FOLLOWING");
			return true;
		}
		default:
			return Veto("window frame of " + window.ToString());
		}
	};
	std::string from;
	std::string to;
	if (!bound(start, window.StartExpr(), from) || !bound(end, window.EndExpr(), to)) {
		return false;
	}
	// A frame that starts after it ends is empty in DuckDB and a compile error
	// there (measured: `1 FOLLOWING AND 1 PRECEDING`).
	auto position = [](WindowBoundary boundary, const unique_ptr<ParsedExpression> &expr) -> int64_t {
		int64_t offset = 0;
		if (expr && expr->GetExpressionClass() == ExpressionClass::CONSTANT) {
			expr->Cast<ConstantExpression>().GetLiteral().TryGetInt64(offset);
		}
		switch (boundary) {
		case WindowBoundary::UNBOUNDED_PRECEDING:
			return NumericLimits<int64_t>::Minimum();
		case WindowBoundary::UNBOUNDED_FOLLOWING:
			return NumericLimits<int64_t>::Maximum();
		case WindowBoundary::EXPR_PRECEDING_ROWS:
			return -offset;
		case WindowBoundary::EXPR_FOLLOWING_ROWS:
			return offset;
		default:
			return 0;
		}
	};
	if (position(start, window.StartExpr()) > position(end, window.EndExpr())) {
		return Veto("a window frame that starts after it ends");
	}
	sql = std::string(rows ? "ROWS" : "RANGE") + " BETWEEN " + from + " AND " + to;
	return true;
}

// QUALIFY (PR E2): T-SQL has none, so the node is written as a derived table
// and a wrapper keeps the rows QUALIFY keeps, then applies the node's DISTINCT /
// ORDER BY / LIMIT, which DuckDB applies after QUALIFY. A condition on
// select-list aliases only (`QUALIFY rn = 1`) is the wrapper's WHERE on the
// derived table's columns, the window evaluated once; any other is the node's
// last result column -- `CASE WHEN <qualify> THEN 1 ELSE 0 END`, a bit (a NULL
// condition drops the row, as QUALIFY does) -- and the wrapper keeps the 1s.
// The wrapper's ORDER BY takes a result column's name or position only: its
// rows are QUALIFY's, where DuckDB evaluates a window in ORDER BY before it and
// binds a name inside an expression to the FROM first (review of E2).
// Measured equal against DuckDB, PR E2.
bool NodeWriter::WriteQualified(const SelectNode &node) {
	auto select = make_uniq<SelectNode>();
	for (auto &modifier : node.modifiers) {
		if (modifier->type == ResultModifierType::ORDER_MODIFIER) {
			for (auto &order : modifier->Cast<OrderModifier>().orders) {
				auto &key = *order.expression;
				const bool column = key.GetExpressionClass() == ExpressionClass::COLUMN_REF &&
									key.Cast<ColumnRefExpression>().ColumnNames().size() == 1;
				const bool position = key.GetExpressionClass() == ExpressionClass::CONSTANT;
				if (!column && !position) {
					return Veto("ORDER BY " + key.ToString() + " under QUALIFY");
				}
			}
		}
		select->modifiers.push_back(modifier->Copy());
	}
	select->select_list.push_back(make_uniq<StarExpression>());
	// On aliases only: every name in the condition is one, and nothing in it
	// is evaluated per row of the node (a window, a subquery).
	std::vector<std::string> names;
	bool on_aliases = !node.qualify->IsWindow() && !node.qualify->HasSubquery();
	std::function<void(const ParsedExpression &)> collect = [&](const ParsedExpression &expr) {
		if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto &ref = expr.Cast<ColumnRefExpression>();
			bool alias = false;
			for (auto &item : node.select_list) {
				alias = alias || (ref.ColumnNames().size() == 1 &&
								  StringUtil::CIEquals(item->GetAlias().GetIdentifierName(),
													   ref.GetColumnName().GetIdentifierName()));
			}
			on_aliases = on_aliases && alias;
			names.push_back(ref.GetColumnName().GetIdentifierName());
			return;
		}
		ParsedExpressionIterator::EnumerateChildren(expr, collect);
	};
	collect(*node.qualify);
	on_aliases = on_aliases && !names.empty();
	select->where_clause =
		on_aliases ? node.qualify->Copy() : make_uniq<ColumnRefExpression>(Identifier(QUALIFY_COLUMN));
	// The node itself is written from `node` (CollectDerivedNode); the
	// placeholder only gives the subquery a node.
	auto statement = make_uniq<SelectStatement>();
	statement->node = make_uniq<SelectNode>();
	select->from_table = make_uniq<SubqueryRef>(std::move(statement));
	synthetic_ref_ = &select->from_table->Cast<SubqueryRef>();
	synthetic_select_ = &node;
	if (on_aliases) {
		qualify_names_ = std::move(names);
	} else {
		hidden_column_ = QUALIFY_COLUMN;
	}
	synthetic_node_ = std::move(select);
	return Write(*synthetic_node_);
}

bool NodeWriter::WriteQualifyColumn(const SelectNode &node) {
	if (!qualify_names_.empty()) {
		// The wrapper filters on the aliases -- which DuckDB reads as FROM
		// columns when one has the name.
		for (auto &name : qualify_names_) {
			idx_t index;
			if (FindColumn(ColumnRefExpression(Identifier(name)), index) != 0) {
				return Veto("QUALIFY on " + name + ", a FROM column and an alias");
			}
		}
		return true;
	}
	for (auto &output : outputs_) {
		if (StringUtil::CIEquals(output.name, QUALIFY_COLUMN)) {
			return Veto("a result column named " + std::string(QUALIFY_COLUMN));
		}
	}
	const idx_t select_ties = tie_windows_;
	std::string condition;
	qualify_aliases_ = true;
	const bool written = WritePredicate(*node.qualify, condition);
	qualify_aliases_ = false;
	if (!written) {
		return false;
	}
	if (select_ties > 0 && (tie_windows_ > select_ties || qualify_named_alias_)) {
		// The select list's window and the condition's are evaluated apart
		// there, once here: ties could be broken two ways (review of E2).
		return Veto("a tie-dependent window evaluated twice under QUALIFY");
	}
	out_.statement +=
		", CAST(CASE WHEN " + condition + " THEN 1 ELSE 0 END AS bit) AS " + QuoteIdentifier(QUALIFY_COLUMN);
	out_.column_types.push_back(LogicalType::BOOLEAN);
	out_.cast_types.push_back(LogicalType::INVALID);
	out_.column_names.push_back(QUALIFY_COLUMN);
	OutputColumn entry;
	entry.name = QUALIFY_COLUMN;
	entry.column_index = DConstants::INVALID_INDEX;
	entry.value.type = entry.value.result_type = LogicalType::BOOLEAN;
	entry.value.kind = ComparableKind::Boolean;
	entry.value.not_null = true;
	outputs_.push_back(std::move(entry));
	return true;
}

}  // namespace node_writer
}  // namespace mssql
}  // namespace duckdb
