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

bool NodeWriter::ConstantFor(ComparableKind kind, const LogicalType &target, const std::string &peer_name,
							 const ParsedExpression &expr, Value &out) {
	if (kind == ComparableKind::None || target.id() == LogicalTypeId::INVALID) {
		return Veto(peer_name + " is not compared with a constant on the server");
	}
	if (expr.GetExpressionClass() == ExpressionClass::CAST) {
		// A typed literal: DuckDB compares in the cast's type, which must be the
		// column's own for the server's comparison to be the same one.
		auto &cast = expr.Cast<CastExpression>();
		LogicalTypeId id;
		if (cast.IsTryCast() || !BuiltinTypeOf(cast.TargetType(), id) || id != target.id() ||
			target.id() == LogicalTypeId::DECIMAL || cast.Child().GetExpressionClass() != ExpressionClass::CONSTANT) {
			return Veto("constant " + expr.ToString() + " against " + peer_name);
		}
		auto &literal = cast.Child().Cast<ConstantExpression>().GetLiteral();
		if (literal.kind != LiteralKind::STRING) {
			return Veto("constant " + expr.ToString() + " against " + peer_name);
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
		if (kind != ComparableKind::ExactNumeric && kind != ComparableKind::Float) {
			return Veto("numeric constant against " + peer_name);
		}
		// DuckDB compares in the wider of the two types; the server compares in
		// the column's. They agree exactly when the constant survives the trip
		// into the column's type and back: 2.0 against an int is 2 both ways,
		// 2.5 or 300 against a tinyint is not, and stays local.
		const Value original = literal.ToValue();
		auto cast_value = original.DefaultTryCastAs(target, nullptr, true);
		if (!cast_value) {
			return Veto("constant " + literal.text + " does not fit " + peer_name);
		}
		auto back = cast_value->DefaultTryCastAs(original.type(), nullptr, true);
		if (!back || !Value::NotDistinctFrom(*back, original)) {
			return Veto("constant " + literal.text + " is not exact in " + peer_name);
		}
		out = std::move(*cast_value);
		return true;
	}
	case LiteralKind::BOOLEAN:
		if (kind != ComparableKind::Boolean) {
			return Veto("boolean constant against " + peer_name);
		}
		out = literal.ToValue();
		return true;
	default:
		return Veto("constant " + expr.ToString() + " against " + peer_name);
	}
}

std::string NodeWriter::Parameter(const MSSQLColumnInfo *column, const Value &value) {
	// The vocabulary's constant: @pN declared from the column it is compared
	// with (076 / #361) -- or, against an expression, from its own value,
	// already cast to the expression's type -- or a literal. The vehicle
	// carries each parameter's VALUE (mssql_scan_params renders its own
	// literal), kept beside the set.
	const size_t before = params_->params.size();
	std::string sql =
		ExpressionVocabulary::Constant(value, value.type(), options_.parameterize ? params_ : nullptr, column);
	if (params_->params.size() > before) {
		param_values_->push_back(value);
	}
	return sql;
}

bool NodeWriter::BindConstant(const Operand &peer, Operand &constant, bool arithmetic) {
	const std::string peer_name = peer.column ? peer.column->name : peer.sql;
	if (arithmetic && constant.constant->GetExpressionClass() == ExpressionClass::CONSTANT &&
		constant.constant->Cast<ConstantExpression>().GetLiteral().kind == LiteralKind::NUMERIC &&
		IntegerRank(peer.type.id()) > 0) {
		// In a VALUE, a decimal literal widens DuckDB's result (`tiny + 2.0` is
		// DECIMAL, 257.0 for 255) where the exact-trip typing would keep the
		// server's tinyint and overflow. A comparison only needs the trip.
		return Veto("a decimal constant beside " + peer_name);
	}
	Value value;
	if (!ConstantFor(peer.kind, peer.type, peer_name, *constant.constant, value)) {
		return false;
	}
	if (value.IsNull()) {
		return Veto("a NULL constant");
	}
	const size_t before = params_->params.size();
	constant.sql = Parameter(peer.column, value);
	if (arithmetic && peer.kind == ComparableKind::String) {
		// A string constant in a VALUE (a CASE / COALESCE branch) types the
		// result on the server: a longer one widens it (varchar(30) for a
		// varchar(20) column), a literal is nvarchar(4000). DuckDB keeps the
		// column's type. So it must fit the column's own declaration, and a
		// literal says it (measured, both).
		const std::string own =
			ExpressionVocabulary::DeclarationForColumn(*peer.column, Value(""), LogicalType::VARCHAR);
		if (own.empty() || ExpressionVocabulary::DeclarationForColumn(*peer.column, value, value.type()) != own) {
			return Veto("a string constant that does not fit " + peer_name);
		}
		if (params_->params.size() == before) {
			constant.sql = ExpressionVocabulary::Cast(constant.sql, own);
		}
	} else if (arithmetic && params_->params.size() == before) {
		// A literal's type is the server's own reading of it -- `1` an int,
		// `3000000000` a numeric(10,0) -- so `tiny + 1` would widen there where
		// DuckDB keeps UTINYINT and overflows at 255, and `big + 3000000000`
		// turn numeric. A parameter is declared from the column; a literal must
		// say its type itself.
		std::string declaration;
		try {
			declaration = DeclarationForValue("p", value.type(), value);
		} catch (const std::exception &) {
			return Veto("constant " + value.ToString() + " has no T-SQL type");
		}
		constant.sql = "CAST(" + constant.sql + " AS " + declaration + ")";
	}
	constant.type = value.type();
	constant.kind = peer.kind;
	constant.constant = nullptr;
	return true;
}

bool NodeWriter::WriteValue(const ParsedExpression &expr, Operand &out) {
	out = Operand();
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF: {
		idx_t index;
		auto &ref = expr.Cast<ColumnRefExpression>();
		if (outer_ && MayBeOuter(ref, FindColumn(ref, index))) {
			// Not a column here: a correlated one of a node around it.
			bool found = false;
			if (!WriteOuterColumn(ref, out, found)) {
				return false;
			}
			if (found) {
				return true;
			}
		}
		if (!ResolveColumn(ref, index)) {
			return false;
		}
		if (!CheckGrouped(index)) {
			return false;
		}
		if (divergent_of_[index] && predicate_depth_ > 0) {
			// NULL there where DuckDB says inf (or the last bits differ): a
			// condition on it would pick other rows (review of PR E1).
			return Veto("a derived division / floating-point aggregate in a condition");
		}
		const auto &column = columns_[index];
		out.sql = ColumnSql(index);
		out.type = column.duckdb_type;
		out.kind = KindOf(column);
		out.column = &column;
		// A derived table's integer SUM: decimal(38,0) on the wire, HUGEINT here.
		out.result_type = cast_of_[index].id() != LogicalTypeId::INVALID ? cast_of_[index] : LogicalType::INVALID;
		out.cast_result = cast_of_[index].id() != LogicalTypeId::INVALID;
		return true;
	}
	case ExpressionClass::CONSTANT:
		// A constant: typed by its peer.
		out.constant = &expr;
		return true;
	case ExpressionClass::CAST: {
		auto &cast = expr.Cast<CastExpression>();
		if (cast.Child().GetExpressionClass() == ExpressionClass::CONSTANT) {
			// The rewriter's folded typed literal (`DATE '2024-01-01'`).
			out.constant = &expr;
			return true;
		}
		return WriteCast(cast, out);
	}
	case ExpressionClass::CASE:
		return WriteCase(expr.Cast<CaseExpression>(), out);
	case ExpressionClass::SUBQUERY:
		return WriteScalarSubquery(expr.Cast<SubqueryExpression>(), out);
	case ExpressionClass::OPERATOR: {
		auto &op = expr.Cast<OperatorExpression>();
		if (expr.GetExpressionType() != ExpressionType::OPERATOR_COALESCE &&
			expr.GetExpressionType() != ExpressionType::OPERATOR_NULLIF) {
			return Veto("value " + expr.ToString());
		}
		std::vector<Operand> operands(op.GetChildren().size());
		for (idx_t i = 0; i < operands.size(); i++) {
			if (!WriteValue(*op.GetChildren()[i], operands[i])) {
				return false;
			}
		}
		if (expr.GetExpressionType() == ExpressionType::OPERATOR_NULLIF &&
			(operands.size() != 2 || operands[0].constant)) {
			// NULLIF's first operand types its result; a constant there has none
			// to give (and NULLIF(NULL, x) is T-SQL error 4151).
			return Veto("value " + expr.ToString());
		}
		if (operands.empty() || !Unify(operands, out, true)) {
			return operands.empty() ? Veto("value " + expr.ToString()) : false;
		}
		std::vector<std::string> args;
		for (auto &operand : operands) {
			args.push_back(operand.sql);
		}
		out.sql = expr.GetExpressionType() == ExpressionType::OPERATOR_NULLIF
					  ? ExpressionVocabulary::NullIf(args[0], args[1])
					  : ExpressionVocabulary::Coalesce(args);
		return true;
	}
	case ExpressionClass::WINDOW:
		return WriteWindow(expr.Cast<WindowExpression>(), out);
	case ExpressionClass::FUNCTION: {
		auto &fn = expr.Cast<FunctionExpression>();
		const auto &name = fn.FunctionName().GetIdentifierName();
		AggregateKind aggregate;
		if (AggregateFor(name, aggregate)) {
			const AggregateCall call{
				fn.GetArguments(),
				fn.Distinct(),
				fn.Filter().get(),
				fn.OrderBy() ? &fn.OrderBy()->orders : nullptr,
				fn.ExportState() || !fn.GetQualifiedName().Schema().empty() || !fn.GetQualifiedName().Catalog().empty(),
				name,
				fn.ToString(),
				""};
			return WriteAggregate(call, aggregate, out);
		}
		if (name == "/" && fn.GetArguments().size() == 2) {
			return WriteDivide(fn, out);
		}
		// Unreachable on the 2.0 pin: DuckDB's nullif is a system macro its
		// rewriter keeps local. Kept for a DuckDB that hands it over.
		if (StringUtil::CIEquals(name, "nullif") && fn.GetArguments().size() == 2) {
			std::vector<Operand> operands(2);
			for (idx_t i = 0; i < 2; i++) {
				if (!WriteValue(fn.GetArguments()[i].GetExpression(), operands[i])) {
					return false;
				}
			}
			if (operands[0].constant) {
				return Veto("nullif of a constant");
			}
			if (!Unify(operands, out, true)) {
				return false;
			}
			out.sql = ExpressionVocabulary::NullIf(operands[0].sql, operands[1].sql);
			return true;
		}
		return WriteArithmetic(fn, out);
	}
	default:
		return Veto("value " + expr.ToString());
	}
}

bool NodeWriter::Unify(std::vector<Operand> &operands, Operand &result, bool consumes_null) {
	for (auto &operand : operands) {
		if (consumes_null && operand.division) {
			// COALESCE(x / 0, -1) is -1 there, inf here: the recorded NULL would
			// become a wrong value.
			return Veto("a division under COALESCE / NULLIF");
		}
	}
	const Operand *peer = nullptr;
	for (auto &operand : operands) {
		if (!operand.constant) {
			peer = &operand;
			break;
		}
	}
	if (!peer) {
		return Veto("an expression of constants only");
	}
	if (peer->kind == ComparableKind::None || peer->type.id() == LogicalTypeId::INVALID) {
		return Veto("an expression over " + peer->sql);
	}
	for (auto &operand : operands) {
		if (operand.constant) {
			continue;
		}
		// One type for every branch: the promotions differ otherwise. A string
		// branch must be a column under the peer's collation -- two collations
		// meeting in one expression are the server's error (468), not a result.
		if (operand.kind != peer->kind || operand.type != peer->type) {
			return Veto("branches of " + peer->type.ToString() + " and " + operand.type.ToString());
		}
		// An integer SUM is HUGEINT in DuckDB and decimal(38,0) on the wire: a
		// COALESCE / CASE over it is cast back as it is, so every branch that is
		// not a constant (typed from it) must be one too.
		if (operand.cast_result != peer->cast_result) {
			return Veto("an integer SUM beside another decimal(38,0)");
		}
		if (peer->kind == ComparableKind::String &&
			(!operand.column || !peer->column || operand.column->collation_name != peer->column->collation_name)) {
			return Veto("string branches that are not columns of one collation");
		}
		// A value, not a comparison: it reaches DuckDB as bytes, and a varchar
		// under a code-page collation arrives untranscoded (#224) -- the catalog
		// scan reads it through its NVARCHAR cast (BuildReadExpression), a branch
		// has none. Unicode and UTF-8 columns arrive as DuckDB's UTF-8.
		if (operand.kind == ComparableKind::String && !operand.column->is_unicode && !operand.column->is_utf8) {
			return Veto("a code-page varchar in a computed value");
		}
	}
	const Operand typed_peer = *peer;
	bool typed_constant = false;
	for (auto &operand : operands) {
		if (!operand.constant) {
			continue;
		}
		if (operand.constant->GetExpressionClass() == ExpressionClass::CONSTANT &&
			operand.constant->Cast<ConstantExpression>().GetLiteral().IsNull()) {
			operand.sql = "NULL";
			operand.constant = nullptr;
			continue;
		}
		if (!BindConstant(typed_peer, operand, true)) {
			return false;
		}
		typed_constant = true;
	}
	result = Operand();
	result.type = typed_peer.type;
	result.kind = typed_peer.kind;
	result.result_type = typed_peer.result_type;
	result.cast_result = typed_peer.cast_result;
	// A constant branch beside a decimal: DuckDB's type is its literal rules'
	// (`COALESCE(v, 700)` is DECIMAL(12,2) there, decimal(10,2) here).
	// An integer SUM is HUGEINT in DuckDB whatever the constant: known.
	// A DECIMAL(38, s) stays DECIMAL(38, s): the width is capped.
	result.type_uncertain = typed_constant && typed_peer.type.id() == LogicalTypeId::DECIMAL &&
							DecimalType::GetWidth(typed_peer.type) < 38 &&
							!(typed_peer.cast_result && typed_peer.result_type.id() != LogicalTypeId::DECIMAL);
	for (auto &operand : operands) {
		result.approximate = result.approximate || operand.approximate;
		result.type_uncertain = result.type_uncertain || operand.type_uncertain;
		// A branch's NULL where DuckDB says inf is the result's (a window's
		// value over a division, review of E2).
		result.division = result.division || operand.division;
	}
	return true;
}

bool NodeWriter::WriteCase(const CaseExpression &expr, Operand &out) {
	std::vector<std::string> whens;
	std::vector<Operand> branches;
	for (auto &check : expr.CaseChecks()) {
		std::string when;
		if (!WritePredicate(*check.when_expr, when)) {
			return false;
		}
		whens.push_back(when);
		branches.emplace_back();
		if (!WriteValue(*check.then_expr, branches.back())) {
			return false;
		}
	}
	branches.emplace_back();
	if (!WriteValue(expr.Else(), branches.back())) {
		return false;
	}
	if (!Unify(branches, out)) {
		return false;
	}
	for (auto &branch : branches) {
		out.division = out.division || branch.division;
		out.approximate = out.approximate || branch.approximate;
	}
	std::vector<std::string> thens;
	for (idx_t i = 0; i + 1 < branches.size(); i++) {
		thens.push_back(branches[i].sql);
	}
	out.sql = ExpressionVocabulary::Case(whens, thens, branches.back().sql);
	return true;
}

bool NodeWriter::WriteDivide(const FunctionExpression &fn, Operand &out) {
	std::vector<Operand> operands(2);
	for (idx_t i = 0; i < 2; i++) {
		if (!WriteValue(fn.GetArguments()[i].GetExpression(), operands[i])) {
			return false;
		}
	}
	if (operands[0].constant && operands[1].constant) {
		return Veto("division of two constants " + fn.ToString());
	}
	if (predicate_depth_ > 0) {
		// A zero divisor gives inf / NaN here and NULL there: recorded for a
		// selected VALUE (spec 079 D2), but in a condition it changes WHICH rows
		// come back -- `x / 0 IS NULL`, `NOT (x / 0 < 1)` -- so not in one.
		return Veto("a division in a condition");
	}
	if (!options_.ieee_floating_point_ops) {
		// Without it DuckDB raises (or, error_on_division_by_zero off, gives
		// NULL) on a zero divisor: the one case the pushed NULL would hide.
		return Veto("/ with ieee_floating_point_ops = false");
	}
	// DuckDB's `/` divides as DOUBLE whatever the numeric operands, so a
	// constant is a DOUBLE here (`n / 2.5` included), and each side is cast to
	// float on the server.
	Operand as_double;
	as_double.kind = ComparableKind::Float;
	as_double.type = LogicalType::DOUBLE;
	as_double.sql = "a DOUBLE operand";
	for (auto &operand : operands) {
		if (operand.constant) {
			if (!BindConstant(as_double, operand, true)) {
				return false;
			}
			continue;
		}
		if ((operand.kind != ComparableKind::ExactNumeric && operand.kind != ComparableKind::Float) ||
			(!IsArithmeticType(operand.type) && operand.type.id() != LogicalTypeId::DOUBLE)) {
			return Veto("division over " + operand.type.ToString());
		}
	}
	out.sql = ExpressionVocabulary::Divide(operands[0].sql, operands[1].sql);
	out.type = LogicalType::DOUBLE;
	out.kind = ComparableKind::Float;
	out.division = true;
	out.approximate = operands[0].approximate || operands[1].approximate;
	return true;
}

bool NodeWriter::WriteCast(const CastExpression &cast, Operand &out) {
	LogicalTypeId target;
	if (cast.IsTryCast() || !BuiltinTypeOf(cast.TargetType(), target)) {
		return Veto("cast " + cast.ToString());
	}
	Operand child;
	if (!WriteValue(cast.Child(), child)) {
		return false;
	}
	if (child.constant || (child.kind != ComparableKind::ExactNumeric && child.kind != ComparableKind::Float)) {
		return Veto("cast " + cast.ToString());
	}
	// Widening only, where both sides convert exactly the same: a wider
	// integer, or a double (IEEE nearest on both). Anything that can fail,
	// round or format -- to a string, a date, a narrower type -- stays DuckDB's.
	const int from = IntegerRank(child.type.id());
	const int to = IntegerRank(target);
	std::string tsql;
	if (to > 0 && from > 0 && to >= from) {
		// UTINYINT is rank 1 -- SQL Server's tinyint is 0-255, which is what
		// DuckDB spells UTINYINT -- so it reaches here whenever the child is one
		// too. Without its own arm it fell through to "bigint", and then the
		// server computed in bigint what DuckDB overflows in UINT8
		// (`CAST(tiny AS UTINYINT) + 1` at tiny = 255: 256 there, "Overflow in
		// addition of UINT8" here), while out.type still said UTINYINT.
		tsql = target == LogicalTypeId::UTINYINT   ? "tinyint"
			   : target == LogicalTypeId::SMALLINT ? "smallint"
			   : target == LogicalTypeId::INTEGER  ? "int"
												   : "bigint";
	} else if (target == LogicalTypeId::DOUBLE &&
			   (from > 0 || child.type.id() == LogicalTypeId::DECIMAL || child.type.id() == LogicalTypeId::DOUBLE)) {
		tsql = "float";
	} else {
		return Veto("cast " + cast.ToString());
	}
	out = Operand();
	out.division = child.division;
	out.approximate = child.approximate;
	out.sql = ExpressionVocabulary::Cast(child.sql, tsql);
	out.type = target == LogicalTypeId::DOUBLE ? LogicalType::DOUBLE : LogicalType(target);
	out.kind = target == LogicalTypeId::DOUBLE ? ComparableKind::Float : ComparableKind::ExactNumeric;
	return true;
}

bool NodeWriter::WriteArithmetic(const FunctionExpression &fn, Operand &out) {
	const auto &name = fn.FunctionName().GetIdentifierName();
	const auto &args = fn.GetArguments();
	const bool binary = args.size() == 2 && (name == "+" || name == "-" || name == "*" || name == "%");
	const bool unary_minus = args.size() == 1 && name == "-";
	if (!binary && !unary_minus) {
		return Veto("function " + fn.ToString());
	}
	std::vector<Operand> operands(args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		if (!WriteValue(args[i].GetExpression(), operands[i])) {
			return false;
		}
	}
	if (unary_minus) {
		auto &operand = operands[0];
		// UTINYINT has no negative values in DuckDB; the server's tinyint
		// negation widens. Constants never get here: the rewriter folds them.
		const bool floating = operand.kind == ComparableKind::Float;
		if (operand.constant || (!floating && operand.kind != ComparableKind::ExactNumeric) ||
			operand.type.id() == LogicalTypeId::UTINYINT || (!floating && !IsArithmeticType(operand.type))) {
			return Veto("negation " + fn.ToString());
		}
		if (operand.cast_result) {
			// A value read as the server's type and cast back (an integer SUM, a
			// decimal product): its negation would come back as the server's.
			return Veto("negation of a value whose type is cast back after the read");
		}
		std::string why;
		auto mapping =
			ExpressionVocabulary::FunctionFor("negate", {operand.type}, why, options_.division_by_zero_errors);
		if (!mapping) {
			return Veto(why);
		}
		out.sql = ExpressionVocabulary::ApplyFunction(*mapping, {operand.sql});
		out.type = operand.type;
		out.kind = operand.kind;
		out.type_uncertain = operand.type_uncertain;
		out.division = operand.division;
		out.approximate = operand.approximate;
		// MoveUnaryMinusRule moves `-x > c` to `x < -c` for any numeric type,
		// but only an integer negation can overflow (at its minimum).
		out.moves_constant = IntegerRank(operand.type.id()) > 0;
		return true;
	}
	auto &left = operands[0];
	auto &right = operands[1];
	if (left.constant && right.constant) {
		return Veto("arithmetic on two constants " + fn.ToString());
	}
	const bool with_constant = left.constant || right.constant;
	if (left.constant && !BindConstant(right, left, true)) {
		return false;
	}
	if (right.constant && !BindConstant(left, right, true)) {
		return false;
	}
	// No arithmetic over doubles: an overflow is inf in DuckDB and error 8115
	// on the server, raised for every row WHERE passes (TOP's discards
	// included) -- near 1e154 for a product, 1.8e308 for a sum. FunctionFor
	// refuses + - * over a float for both paths (review of #396); a double
	// falls through to the exact-numeric check below and is vetoed.
	// Same type on both sides, or the promotions differ: DuckDB widens
	// smallint + int to INTEGER, the server by its own precedence.
	if (left.kind != ComparableKind::ExactNumeric || right.kind != ComparableKind::ExactNumeric ||
		!IsArithmeticType(left.type) || left.type != right.type) {
		return Veto("arithmetic over " + left.type.ToString() + " and " + right.type.ToString());
	}
	// MoveConstantsRule (integral types only; `%` not among its operators).
	out.moves_constant = with_constant && name != "%" && IntegerRank(left.type.id()) > 0;
	std::string why;
	auto mapping =
		ExpressionVocabulary::FunctionFor(name, {left.type, right.type}, why, options_.division_by_zero_errors);
	if (!mapping) {
		return Veto(why);
	}
	out.sql = ExpressionVocabulary::ApplyFunction(*mapping, {left.sql, right.sql});
	out.kind = ComparableKind::ExactNumeric;
	if (left.type.id() != LogicalTypeId::DECIMAL) {
		out.type = left.type;
	} else if (name == "+" || name == "-") {
		// DECIMAL(w,s) +/- DECIMAL(w,s) is DECIMAL(w+1,s) on both (measured) --
		// below 38. At 38 the server's result would need 39 digits, and past 38
		// it reduces the SCALE (rounding the value) where DuckDB stays exact.
		const auto width = DecimalType::GetWidth(left.type);
		if (width >= 38) {
			return Veto("decimal arithmetic at width 38");
		}
		out.type = LogicalType::DECIMAL(width + 1, DecimalType::GetScale(left.type));
		// A constant beside it is typed by DuckDB's literal rules, not the
		// column's: `v + 700` is DECIMAL(13,2) there.
		out.type_uncertain = with_constant || left.type_uncertain || right.type_uncertain;
		// Over operands cast back after the read (a product) the server's width
		// grows from THEIR wider types: the sum is cast back too.
		out.result_type = out.type;
		out.cast_result = left.cast_result || right.cast_result;
	} else {
		// A product. FunctionFor (above) has refused the ones DuckDB caps at
		// DECIMAL(18) -- overflowing where the server's decimal(w1 + w2 + 1, …)
		// holds the value (99999999.99 squared in DECIMAL(10,2)) -- and the ones
		// past the server's 38 digits. What is left DuckDB types exactly as
		// DECIMAL(w1 + w2, s1 + s2): the server's value, read and cast to it.
		const auto width = DecimalType::GetWidth(left.type);
		const auto scale = DecimalType::GetScale(left.type);
		if (2 * width > 38) {
			return Veto("a decimal product past 38 digits");
		}
		out.type = LogicalType::DECIMAL(2 * width, 2 * scale);
		out.result_type = out.type;
		out.cast_result = true;
		out.type_uncertain = with_constant || left.type_uncertain || right.type_uncertain;
	}
	return true;
}

bool NodeWriter::BindPair(Operand &left, Operand &right, const std::string &what) {
	if (left.constant && right.constant) {
		return Veto("a comparison of two constants " + what);
	}
	if (left.constant) {
		return BindConstant(right, left);
	}
	if (right.constant) {
		return BindConstant(left, right);
	}
	// Value against value: one type, or DuckDB and the server promote
	// differently; two strings only as columns of one collation -- two
	// collations meeting in a comparison are the server's error 468.
	if (left.kind == ComparableKind::None || left.kind != right.kind || left.type.id() == LogicalTypeId::INVALID ||
		left.type != right.type) {
		return Veto("a comparison of " + left.type.ToString() + " and " + right.type.ToString() + " " + what);
	}
	if (left.kind == ComparableKind::String &&
		(!left.column || !right.column || left.column->collation_name != right.column->collation_name)) {
		return Veto("a comparison of strings that are not columns of one collation " + what);
	}
	return true;
}

bool NodeWriter::WriteComparison(const ComparisonExpression &cmp, std::string &sql) {
	std::string op;
	if (!ExpressionVocabulary::ComparisonOperator(cmp.GetExpressionType(), op)) {
		return Veto("comparison " + cmp.ToString());
	}
	Operand left;
	Operand right;
	if (!WriteValue(cmp.Left(), left) || !WriteValue(cmp.Right(), right)) {
		return false;
	}
	if ((left.moves_constant && right.constant) || (right.moves_constant && left.constant)) {
		// DuckDB moves the constant across (`t - 5 > 3` is `t > 8` there) and
		// never computes what the server would, and could overflow on.
		return Veto("arithmetic with a constant compared with a constant " + cmp.ToString());
	}
	if (!BindPair(left, right, cmp.ToString())) {
		return false;
	}
	sql = ExpressionVocabulary::Comparison(op, left.sql, right.sql);
	return true;
}

bool NodeWriter::WriteIn(const OperatorExpression &op, std::string &sql) {
	auto &children = op.GetChildren();
	if (children.size() < 2 || !ExpressionVocabulary::InListFits(children.size() - 1)) {
		return Veto("an IN list of " + std::to_string(children.size() - 1) + " items");
	}
	Operand operand;
	if (!WriteValue(*children[0], operand)) {
		return false;
	}
	if (operand.constant) {
		return Veto("IN over a constant");
	}
	std::vector<std::string> items;
	for (idx_t i = 1; i < children.size(); i++) {
		Operand item;
		if (!WriteValue(*children[i], item)) {
			return false;
		}
		// A NULL in the list is three-valued on both sides alike.
		if (item.constant && item.constant->GetExpressionClass() == ExpressionClass::CONSTANT &&
			item.constant->Cast<ConstantExpression>().GetLiteral().IsNull()) {
			items.push_back("NULL");
			continue;
		}
		Operand peer = operand;
		if (!BindPair(peer, item, op.ToString())) {
			return false;
		}
		items.push_back(item.sql);
	}
	sql = ExpressionVocabulary::In(operand.sql, items, op.GetExpressionType() == ExpressionType::COMPARE_NOT_IN);
	return true;
}

bool NodeWriter::WriteBetween(const BetweenExpression &between, std::string &sql) {
	Operand input;
	if (!WriteValue(between.Input(), input)) {
		return false;
	}
	if (input.constant) {
		return Veto("BETWEEN over a constant");
	}
	Operand lower;
	Operand upper;
	if (!WriteValue(between.LowerBound(), lower) || !WriteValue(between.UpperBound(), upper)) {
		return false;
	}
	if (input.moves_constant && (lower.constant || upper.constant)) {
		// BETWEEN is two comparisons to DuckDB, each moved as above.
		return Veto("arithmetic with a constant in BETWEEN " + between.ToString());
	}
	Operand peer = input;
	if (!BindPair(peer, lower, between.ToString()) || !BindPair(peer, upper, between.ToString())) {
		return false;
	}
	sql = ExpressionVocabulary::Between(input.sql, lower.sql, upper.sql, true, true);
	return true;
}

bool NodeWriter::WriteLike(const FunctionExpression &fn, std::string &sql) {
	const auto &name = fn.FunctionName().GetIdentifierName();
	const auto &args = fn.GetArguments();
	if (args.size() != 2) {
		return Veto("LIKE " + fn.ToString());
	}
	Operand value;
	if (!WriteValue(args[0].GetExpression(), value)) {
		return false;
	}
	if (value.constant || value.kind != ComparableKind::String) {
		return Veto("LIKE over " + value.type.ToString());
	}
	const auto &pattern = args[1].GetExpression();
	if (pattern.GetExpressionClass() != ExpressionClass::CONSTANT ||
		pattern.Cast<ConstantExpression>().GetLiteral().kind != LiteralKind::STRING) {
		return Veto("LIKE with a pattern that is not a string constant");
	}
	// The pattern is compared with the value, so it is declared from its
	// column (a varchar keeps a seekable prefix); `[` taken literally.
	const std::string text = ExpressionVocabulary::LikePattern(pattern.Cast<ConstantExpression>().GetLiteral().text);
	if (text.size() > 4000) {
		// A LIKE pattern is limited to 8000 bytes on the server.
		return Veto("a LIKE pattern past 4000 characters");
	}
	const std::string like = ExpressionVocabulary::Like(value.sql, Parameter(value.column, Value(text)));
	sql = name == "!~~" ? ExpressionVocabulary::Not(like) : like;
	return true;
}

bool NodeWriter::WritePredicate(const ParsedExpression &expr, std::string &sql) {
	++predicate_depth_;
	const bool ok = WritePredicateImpl(expr, sql);
	--predicate_depth_;
	return ok;
}

bool NodeWriter::WritePredicateImpl(const ParsedExpression &expr, std::string &sql) {
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
	case ExpressionClass::SUBQUERY:
		return WriteSubqueryCondition(expr.Cast<SubqueryExpression>(), sql);
	case ExpressionClass::BETWEEN:
		return WriteBetween(expr.Cast<BetweenExpression>(), sql);
	case ExpressionClass::FUNCTION: {
		// LIKE / NOT LIKE (the parser's `~~` / `!~~`). ILIKE (`~~*`), GLOB
		// (`~~~`) and LIKE … ESCAPE are not: the server has no case folding of
		// DuckDB's (#392) nor GLOB's syntax.
		auto &fn = expr.Cast<FunctionExpression>();
		const auto &name = fn.FunctionName().GetIdentifierName();
		if (name == "~~" || name == "!~~") {
			return WriteLike(fn, sql);
		}
		return Veto("predicate " + expr.ToString());
	}
	case ExpressionClass::OPERATOR: {
		auto &op = expr.Cast<OperatorExpression>();
		auto &children = op.GetChildren();
		if (expr.GetExpressionType() == ExpressionType::COMPARE_IN ||
			expr.GetExpressionType() == ExpressionType::COMPARE_NOT_IN) {
			return WriteIn(op, sql);
		}
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
			children.size() == 1) {
			Operand value;
			if (!WriteValue(*children[0], value)) {
				return false;
			}
			if (value.constant) {
				return Veto("IS NULL of a constant");
			}
			sql = ExpressionVocabulary::IsNull(value.sql,
											   expr.GetExpressionType() == ExpressionType::OPERATOR_IS_NOT_NULL);
			return true;
		}
		return Veto("operator " + expr.ToString());
	}
	case ExpressionClass::COLUMN_REF: {
		// A bit column as a condition: `WHERE flag` is `([flag] = 1)` in T-SQL
		// (a bare bit is error 4145).
		Operand value;
		if (!WriteValue(expr, value)) {
			return false;
		}
		if (value.kind != ComparableKind::Boolean) {
			return Veto("predicate " + expr.ToString());
		}
		SqlOperand operand;
		operand.sql = value.sql;
		operand.type = LogicalType::BOOLEAN;
		sql = ExpressionVocabulary::AsCondition(operand);
		return true;
	}
	default:
		return Veto("predicate " + expr.ToString());
	}
}

}  // namespace node_writer
}  // namespace mssql
}  // namespace duckdb
