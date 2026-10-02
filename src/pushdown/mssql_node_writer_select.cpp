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

bool NodeWriter::WriteAggregate(const FunctionExpression &fn, AggregateKind kind, Operand &out) {
	if (!after_grouping_) {
		return Veto("an aggregate in WHERE / ON");
	}
	if (in_aggregate_) {
		return Veto("an aggregate inside an aggregate");
	}
	if (fn.Filter() || (fn.OrderBy() && !fn.OrderBy()->orders.empty()) || fn.ExportState() ||
		!fn.GetQualifiedName().Schema().empty() || !fn.GetQualifiedName().Catalog().empty()) {
		return Veto("aggregate " + fn.ToString());
	}
	auto &args = fn.GetArguments();
	out = Operand();
	out.kind = ComparableKind::ExactNumeric;
	if (kind == AggregateKind::CountStar) {
		if (!args.empty()) {
			return Veto("aggregate " + fn.ToString());
		}
		out.sql = "COUNT_BIG(*)";
		out.type = out.result_type = LogicalType::BIGINT;
		out.orderable = out.not_null = true;
		return true;
	}
	if (args.size() != 1 || args[0].HasName()) {
		return Veto("aggregate " + fn.ToString());
	}
	Operand arg;
	in_aggregate_ = true;
	const bool written = WriteValue(args[0].GetExpression(), arg);
	in_aggregate_ = false;
	if (!written) {
		return false;
	}
	if (arg.constant) {
		// count(1): the rows. Any other aggregate of a constant stays DuckDB's.
		auto &constant = *arg.constant;
		if (kind != AggregateKind::Count || fn.Distinct() ||
			constant.GetExpressionClass() != ExpressionClass::CONSTANT ||
			constant.Cast<ConstantExpression>().GetLiteral().IsNull()) {
			return Veto("aggregate of a constant " + fn.ToString());
		}
		out.sql = "COUNT_BIG(*)";
		out.type = out.result_type = LogicalType::BIGINT;
		out.orderable = out.not_null = true;
		return true;
	}
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
	const std::string distinct = fn.Distinct() ? "DISTINCT " : "";
	if (kind == AggregateKind::Count) {
		if (fn.Distinct() && (arg.column ? !IsGroupable(*arg.column) : arg.kind == ComparableKind::None)) {
			return Veto("count(DISTINCT) over " + arg.type.ToString());
		}
		out.sql = "COUNT_BIG(" + distinct + arg.sql + ")";
		out.type = out.result_type = LogicalType::BIGINT;
		out.orderable = out.not_null = true;
		return true;
	}
	if (kind == AggregateKind::Min || kind == AggregateKind::Max) {
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
					  ? "CAST(" + name + "(CAST(" + arg.sql + " AS tinyint)) AS bit)"
					  : name + "(" + arg.sql + ")";
		out.type = arg.type;
		out.kind = arg.kind;
		out.column = arg.column;  // a constant compared with it is declared from the column
		out.result_type = types_[index];
		out.orderable = true;
		return true;
	}
	// SUM / AVG / STDEV / VAR: an exact numeric, or a float column (or a
	// widening cast to DOUBLE).
	const bool integer = arg.kind == ComparableKind::ExactNumeric && IntegerRank(arg.type.id()) > 0;
	const bool decimal = arg.kind == ComparableKind::ExactNumeric && arg.type.id() == LogicalTypeId::DECIMAL;
	const bool floating = arg.type.id() == LogicalTypeId::DOUBLE && arg.kind == ComparableKind::Float;
	if (!integer && !decimal && !floating) {
		return Veto(fn.FunctionName().GetIdentifierName() + " over " + arg.type.ToString());
	}
	// An integer sum as decimal(38,0): the server's own SUM of an int is an
	// int and overflows where DuckDB's HUGEINT does not, and a bigint cast
	// overflows too (measured).
	const std::string exact_sum =
		"SUM(" + distinct + (integer ? ExpressionVocabulary::Cast(arg.sql, "decimal(38,0)") : arg.sql) + ")";
	out.type = LogicalType::DOUBLE;
	out.result_type = LogicalType::DOUBLE;
	switch (kind) {
	case AggregateKind::Sum:
		if (floating) {
			out.sql = "SUM(" + distinct + arg.sql + ")";
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
		out.sql = floating
					  ? "AVG(" + distinct + arg.sql + ")"
					  : ExpressionVocabulary::Cast(exact_sum, "float") + " / COUNT_BIG(" + distinct + arg.sql + ")";
		out.approximate = true;
		break;
	default: {
		const char *name = kind == AggregateKind::Stdev	   ? "STDEV"
						   : kind == AggregateKind::StdevP ? "STDEVP"
						   : kind == AggregateKind::Var	   ? "VAR"
														   : "VARP";
		out.sql = std::string(name) + "(" + distinct + arg.sql + ")";
		out.approximate = true;
		break;
	}
	}
	if (out.approximate && predicate_depth_ > 0) {
		// Last bits apart: which rows pass `avg(x) > 2.5` could differ.
		return Veto("a floating-point aggregate in a condition");
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
		if (!CheckGrouped(index)) {
			return false;
		}
		if (!list.empty()) {
			list += ", ";
		}
		const auto &relation = relations_[relation_of_[index]];
		OutputColumn entry;
		entry.name = output;
		entry.column_index = index;
		if (already_read_[index]) {
			// A derived table's column: its node read it already.
			list += ColumnSql(index) + " AS " + QuoteIdentifier(output);
			entry.plain_read = true;
		} else {
			const auto read =
				MSSQLColumnInfo::BuildReadExpression(column.name, column.sql_type_name, column.max_length,
													 column.collation_name, options_.convert_varchar_max, "", "");
			list += MSSQLColumnInfo::BuildReadExpression(column.name, column.sql_type_name, column.max_length,
														 column.collation_name, options_.convert_varchar_max,
														 relation.sql.empty() ? "" : relation.sql + ".", alias);
			entry.plain_read = read == QuoteIdentifier(column.name);
		}
		outputs_.push_back(std::move(entry));
		const bool cast_back = cast_of_[index].id() != LogicalTypeId::INVALID;
		out_.column_types.push_back(cast_back ? LogicalType::INVALID : types_[index]);
		out_.cast_types.push_back(cast_of_[index]);
		out_.column_names.push_back(output);
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
			if (aggregated_) {
				return Veto("a * in an aggregate query");
			}
			// `*` lists every relation's columns in FROM order, a USING column
			// once; `r.*` one relation's, all of them.
			idx_t only = DConstants::INVALID_INDEX;
			if (!star.RelationName().empty()) {
				for (idx_t r = 0; r < relations_.size(); r++) {
					if (StringUtil::CIEquals(star.RelationName().GetIdentifierName(), relations_[r].name)) {
						only = r;
					}
				}
				if (only == DConstants::INVALID_INDEX) {
					return Veto("a * of another relation");
				}
			}
			if (only != DConstants::INVALID_INDEX && relations_[only].semi) {
				return Veto("a * of a SEMI / ANTI join's table");
			}
			for (idx_t i = 0; i < columns_.size(); i++) {
				if (only != DConstants::INVALID_INDEX ? relation_of_[i] != only
													  : IsHiddenUsingColumn(i) || relations_[relation_of_[i]].semi) {
					continue;
				}
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
		case ExpressionClass::CONSTANT: {
			// An integer constant (`EXISTS (SELECT 1 ...)`): INTEGER to DuckDB's
			// binder as an int literal is to the server. Any other literal is
			// typed by rules of its own on each side.
			auto &literal = item->Cast<ConstantExpression>().GetLiteral();
			int64_t number;
			if (aggregated_ || literal.kind != LiteralKind::INTEGER || !literal.TryGetInt64(number) ||
				number > NumericLimits<int32_t>::Maximum() || number < NumericLimits<int32_t>::Minimum()) {
				return Veto("a constant in the select list");
			}
			const std::string output = item->GetName().GetIdentifierName();
			if (output.size() > 128) {
				return Veto("a result column name longer than 128 characters");
			}
			for (auto &existing : outputs_) {
				if (StringUtil::CIEquals(existing.name, output)) {
					return Veto("two result columns are named " + output);
				}
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
			break;
		}
		case ExpressionClass::FUNCTION:
		case ExpressionClass::CASE:
		case ExpressionClass::CAST:
		case ExpressionClass::OPERATOR:
		case ExpressionClass::SUBQUERY: {
			// A computed column: rendered by the same rules as WHERE, named as
			// DuckDB names it (the alias, else the expression's text), typed by
			// the server's describe -- the spec's "types of a pushed query are the
			// server's" (column_types '' for it) -- or, for an aggregate, as DuckDB
			// types it.
			Operand value;
			if (!WriteValue(*item, value)) {
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
			if (alias.empty() && item->HasSubquery()) {
				// Named after its text, which holds the table names the rewriter
				// stripped of their catalog (review of E1).
				return Veto("a subquery in a result column without an alias");
			}
			const std::string output = item->GetName().GetIdentifierName();
			if (output.size() > 128) {
				// SQL Server refuses a longer identifier (103); DuckDB does not.
				return Veto("a result column name longer than 128 characters");
			}
			for (auto &existing : outputs_) {
				if (StringUtil::CIEquals(existing.name, output)) {
					return Veto("two result columns are named " + output);
				}
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
			break;
		}
		default:
			return Veto("select-list expression " + item->ToString());
		}
	}
	out_.statement += list;
	return true;
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
	// A key already sorted by is dropped: it cannot reorder anything, and the
	// server refuses a repeated ORDER BY item (169) where DuckDB takes it.
	std::vector<std::string> keys;
	auto repeated = [&](const std::string &key) {
		for (auto &seen : keys) {
			if (seen == key) {
				return true;
			}
		}
		keys.push_back(key);
		return false;
	};
	for (auto &node : order.orders) {
		// DuckDB resolves an ORDER BY name against the select list first, then
		// the FROM; a bare integer is a position in the select list.
		const OutputColumn *output = nullptr;
		idx_t index = DConstants::INVALID_INDEX;
		Operand computed;
		const auto &key = *node.expression;
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
		if (distinct_) {
			// Under DISTINCT the key must be a result column as the server spells
			// it (145) -- a column read through a CAST is not.
			bool listed = false;
			for (auto &candidate : outputs_) {
				listed = listed ||
						 (index != DConstants::INVALID_INDEX ? candidate.column_index == index && candidate.plain_read
															 : candidate.column_index == DConstants::INVALID_INDEX &&
																   candidate.value.sql == computed.sql);
			}
			if (!listed) {
				return Veto("ORDER BY " + key.ToString() + " under DISTINCT: not a result column the server can name");
			}
		}
		const auto type = node.type == OrderType::ORDER_DEFAULT ? options_.default_order : node.type;
		auto null_order = node.null_order;
		if (null_order == OrderByNullType::ORDER_DEFAULT) {
			null_order =
				type == OrderType::DESCENDING ? options_.default_null_order_desc : options_.default_null_order_asc;
		}
		std::string term;
		if (repeated(index != DConstants::INVALID_INDEX ? OrderColumnSql(index) : computed.sql)) {
			continue;
		}
		if (index == DConstants::INVALID_INDEX) {
			if (!computed.orderable) {
				return Veto(computed.approximate ? "ORDER BY a floating-point aggregate"
												 : "ORDER BY a computed column");
			}
			// Under DISTINCT the term must be the key itself: a CASE placing NULLs
			// is not a result column (145).
			if (!OrderTerm(computed.sql, computed.sql, type, null_order, !computed.not_null, limited && !distinct_,
						   term)) {
				return Veto("ORDER BY " + key.ToString() + ": NULL placement needs a LIMIT to be emulated");
			}
			sql += (sql.empty() ? "" : ", ") + term;
			continue;
		}
		const auto &column = columns_[index];
		// Qualified (OrderColumnSql): DuckDB's resolution above already chose
		// the table column, which a bare T-SQL name would not.
		const std::string column_ref = OrderColumnSql(index);
		std::string fragment;
		// Under DISTINCT neither the bytes key nor the NULL-placing CASE is a
		// result column (145): the key must be sent as it is.
		if (!OrderKeyFragment(column, column_ref, true, limited && !distinct_, fragment)) {
			return Veto("ORDER BY " + column.name + ": the server does not order " + column.sql_type_name +
						" as DuckDB does");
		}
		if (!OrderTerm(fragment, column_ref, type, null_order, column.is_nullable, limited && !distinct_, term)) {
			return Veto("ORDER BY " + column.name + ": NULL placement needs a LIMIT to be emulated");
		}
		sql += (sql.empty() ? "" : ", ") + term;
	}
	return true;
}

bool NodeWriter::Write(const SelectNode &node) {
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
	if (node.qualify || node.sample) {
		return Veto("QUALIFY / USING SAMPLE");
	}
	if (!node.from_table) {
		return Veto("no FROM");
	}
	for (auto &item : node.select_list) {
		if (!item->GetAlias().empty()) {
			select_aliases_.push_back(item->GetAlias().GetIdentifierName());
		}
	}
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
	std::string from_sql;
	visible_ = 0;
	if (!WriteFrom(*node.from_table, from_sql)) {
		return false;
	}
	visible_ = DConstants::INVALID_INDEX;

	const OrderModifier *order = nullptr;
	const LimitModifier *limit = nullptr;
	for (idx_t i = 0; i < node.modifiers.size(); i++) {
		auto &modifier = node.modifiers[i];
		// The parser puts DISTINCT first; DISTINCT ON is DuckDB's own.
		if (modifier->type == ResultModifierType::DISTINCT_MODIFIER && i == 0 &&
			modifier->Cast<DistinctModifier>().distinct_on_targets.empty()) {
			distinct_ = true;
		} else if (modifier->type == ResultModifierType::ORDER_MODIFIER && !order && !limit) {
			order = &modifier->Cast<OrderModifier>();
		} else if (modifier->type == ResultModifierType::LIMIT_MODIFIER && !limit) {
			limit = &modifier->Cast<LimitModifier>();
		} else {
			return Veto("a DISTINCT ON, a LIMIT % or a second ORDER BY / LIMIT");
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
	limit_ = limit_value;
	out_.picks_rows = out_.picks_rows || limited || offset_value > 0;
	const bool top = limited && offset_value == 0;
	if (distinct_ && offset_value > 0 && !order) {
		// OFFSET's `ORDER BY (SELECT NULL)` is not a result column: 145.
		return Veto("DISTINCT with OFFSET and no ORDER BY");
	}

	// An aggregate query: grouped, filtered on groups, or aggregating --
	// anywhere after grouping, ORDER BY included, as DuckDB's binder decides.
	std::string group_sql;
	if (!WriteGroups(node, group_sql)) {
		return false;
	}
	aggregated_ = aggregated_ || node.having;
	for (auto &item : node.select_list) {
		aggregated_ = aggregated_ || ContainsAggregate(*item);
	}
	if (order) {
		for (auto &key : order->orders) {
			aggregated_ = aggregated_ || ContainsAggregate(*key.expression);
		}
	}

	out_.statement = std::string("SELECT ") + (distinct_ ? "DISTINCT " : "") +
					 (top ? "TOP (" + std::to_string(limit_value) + ") " : "");
	after_grouping_ = true;
	if (!WriteSelectList(node)) {
		return false;
	}
	if (distinct_) {
		for (auto &output : outputs_) {
			if (output.column_index != DConstants::INVALID_INDEX) {
				if (!IsGroupable(columns_[output.column_index])) {
					return Veto("DISTINCT over " + columns_[output.column_index].sql_type_name);
				}
			} else if (output.value.kind == ComparableKind::None || output.value.approximate || output.value.division) {
				return Veto("DISTINCT over the computed column " + output.name);
			}
		}
	}
	after_grouping_ = false;
	out_.statement += " FROM " + from_sql;
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
	after_grouping_ = true;
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
	for (auto &output : outputs_) {
		// Computed here, or a derived table's (or inlined CTE's) column carried
		// out as it is (review of #399: one level of nesting hid the flag, and
		// a division's NULL reached DuckDB's filter above the vehicle).
		out_.value_divergence = out_.value_divergence || (output.column_index == DConstants::INVALID_INDEX
															  ? output.value.division || output.value.approximate
															  : divergent_of_[output.column_index]);
	}
	for (auto &relation : relations_) {
		if (relation.semi) {
			continue;
		}
		out_.largest_input_rows = MaxValue(out_.largest_input_rows, relation.table.approx_rows);
		out_.input_size_unknown = out_.input_size_unknown || !relation.table.size_known;
	}
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

bool NodeWriter::WriteSetOperation(const SetOperationNode &node) {
	const char *keyword;
	switch (node.setop_type) {
	case SetOperationType::UNION:
		keyword = node.setop_all ? " UNION ALL " : " UNION ";
		break;
	case SetOperationType::EXCEPT:
		keyword = " EXCEPT ";
		break;
	case SetOperationType::INTERSECT:
		keyword = " INTERSECT ";
		break;
	default:
		return Veto("UNION BY NAME");
	}
	if (node.setop_all && node.setop_type != SetOperationType::UNION) {
		// T-SQL has no EXCEPT ALL / INTERSECT ALL.
		return Veto("EXCEPT ALL / INTERSECT ALL");
	}
	// UNION, EXCEPT and INTERSECT compare rows: a set of values, as DISTINCT's.
	const bool compares = !(node.setop_type == SetOperationType::UNION && node.setop_all);
	if (node.children.size() < 2) {
		return Veto("a set operation of one child");
	}
	std::vector<WrittenQuery> child_outs(node.children.size());
	std::vector<unique_ptr<NodeWriter>> children;
	std::string sql;
	for (idx_t c = 0; c < node.children.size(); c++) {
		auto &child_node = *node.children[c];
		bool ordered = false;
		bool limited = false;
		for (auto &modifier : child_node.modifiers) {
			ordered = ordered || modifier->type == ResultModifierType::ORDER_MODIFIER;
			limited = limited || modifier->type == ResultModifierType::LIMIT_MODIFIER;
		}
		if (ordered && !limited) {
			// 1033 there, as in a derived table; DuckDB promises no order.
			return Veto("an ORDER BY without LIMIT in a set operation's member");
		}
		// Its children are aliased past nothing of this node: a correlated
		// column is the enclosing node's.
		children.push_back(make_uniq<NodeWriter>(*this, child_outs[c], Correlated()));
		if (!children.back()->WriteQueryNode(child_node)) {
			out_.refers_outside = out_.refers_outside || child_outs[c].refers_outside;
			return false;
		}
		std::string part = child_outs[c].statement;
		if (ordered || limited) {
			// TOP / ORDER BY / OFFSET inside a set operation's member: in a
			// derived table of its own, which T-SQL takes.
			part = "SELECT * FROM (" + part + ") AS [q" + std::to_string(alias_base_ + c + 1) + "]";
		} else if (child_node.type == QueryNodeType::SET_OPERATION_NODE) {
			part = "(" + part + ")";
		}
		sql += (c == 0 ? "" : keyword) + part;
	}
	auto &first = *children[0];
	const auto width = first.Outputs().size();
	for (idx_t i = 0; i < width; i++) {
		bool plain = true;
		bool nullable = false;
		bool divergent = false;
		for (idx_t c = 0; c < children.size(); c++) {
			auto &child = *children[c];
			if (child.Outputs().size() != width) {
				return Veto("set operation children of different widths");
			}
			auto &output = child.Outputs()[i];
			auto &child_out = child_outs[c];
			// One DuckDB type, known, on every side: the server's promotion
			// (varchar(10) with varchar(20), int with bigint) is not DuckDB's.
			if (child_out.column_types[i] != child_outs[0].column_types[i] ||
				child_out.cast_types[i] != child_outs[0].cast_types[i] ||
				(child_out.column_types[i].id() == LogicalTypeId::INVALID &&
				 child_out.cast_types[i].id() == LogicalTypeId::INVALID)) {
				return Veto("set operation columns of different types");
			}
			const bool column = output.column_index != DConstants::INVALID_INDEX;
			plain = plain && column && output.plain_read;
			if (plain) {
				// One server shape too: time(3) and time(7) are both TIME here,
				// a varchar(5) and a varchar(50) both VARCHAR with native types
				// off -- the first member's metadata would misdescribe the rest.
				auto &mine = child.FlatColumn(output.column_index);
				auto &theirs = first.FlatColumn(first.Outputs()[i].column_index);
				plain = StringUtil::CIEquals(mine.sql_type_name, theirs.sql_type_name) &&
						mine.max_length == theirs.max_length && mine.precision == theirs.precision &&
						mine.scale == theirs.scale;
			}
			const auto kind = column ? KindOf(child.FlatColumn(output.column_index)) : output.value.kind;
			if (kind == ComparableKind::String) {
				// One collation, or the server's 468 / a different comparison.
				auto &reference = first.Outputs()[i];
				if (!column || !output.plain_read || reference.column_index == DConstants::INVALID_INDEX ||
					!reference.plain_read ||
					child.FlatColumn(output.column_index).collation_name !=
						first.FlatColumn(reference.column_index).collation_name ||
					!StringUtil::CIEquals(child.FlatColumn(output.column_index).sql_type_name,
										  first.FlatColumn(reference.column_index).sql_type_name)) {
					return Veto("set operation string columns that are not columns of one type and collation");
				}
			}
			if (compares) {
				if (column ? !IsGroupable(child.FlatColumn(output.column_index))
						   : output.value.kind == ComparableKind::None || output.value.approximate ||
								 output.value.division) {
					return Veto("a set operation compares " + output.name);
				}
			}
			if (column) {
				nullable = nullable || child.FlatColumn(output.column_index).is_nullable ||
						   child.divergent_of_[output.column_index];
				divergent = divergent || child.divergent_of_[output.column_index];
			} else {
				nullable = true;
				divergent = divergent || output.value.division || output.value.approximate;
			}
		}
		auto &reference = first.Outputs()[i];
		OutputColumn entry;
		entry.name = reference.name;
		if (plain) {
			auto column = first.FlatColumn(reference.column_index);
			column.name = reference.name;
			column.is_nullable = nullable;
			entry.column_index = columns_.size();
			entry.plain_read = true;
			columns_.push_back(std::move(column));
			// Parallel to columns_ as elsewhere; relation_of_ stays empty, as a
			// set operation has no relation (FindColumn answers 0 for it).
			types_.push_back(child_outs[0].column_types[i]);
			cast_of_.push_back(child_outs[0].cast_types[i]);
			already_read_.push_back(true);
			divergent_of_.push_back(divergent);
		} else {
			entry.column_index = DConstants::INVALID_INDEX;
			if (reference.column_index != DConstants::INVALID_INDEX) {
				// Columns of different server shapes: a value, opaque outside.
				auto &column = first.FlatColumn(reference.column_index);
				entry.value.type = column.duckdb_type;
				entry.value.kind = ComparableKind::None;
			} else {
				entry.value = reference.value;
			}
			entry.value.column = nullptr;
			entry.value.constant = nullptr;
			entry.value.orderable = false;
			entry.value.not_null = false;
			entry.value.division = divergent;
			entry.value.approximate = divergent;
			entry.value.result_type = child_outs[0].cast_types[i].id() != LogicalTypeId::INVALID
										  ? child_outs[0].cast_types[i]
										  : child_outs[0].column_types[i];
			entry.value.cast_result = child_outs[0].cast_types[i].id() != LogicalTypeId::INVALID;
		}
		outputs_.push_back(std::move(entry));
		out_.column_types.push_back(child_outs[0].column_types[i]);
		out_.cast_types.push_back(child_outs[0].cast_types[i]);
		out_.column_names.push_back(child_outs[0].column_names[i]);
	}
	for (auto &child_out : child_outs) {
		out_.picks_rows = out_.picks_rows || child_out.picks_rows;
		out_.gain_uncertain = out_.gain_uncertain || child_out.gain_uncertain;
		out_.value_divergence = out_.value_divergence || child_out.value_divergence;
		out_.largest_input_rows = MaxValue(out_.largest_input_rows, child_out.largest_input_rows);
		out_.input_size_unknown = out_.input_size_unknown || child_out.input_size_unknown;
	}
	out_.statement = sql;
	return true;
}

}  // namespace node_writer
}  // namespace mssql
}  // namespace duckdb
