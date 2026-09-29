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
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
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
	Value ieee;
	if (context.TryGetCurrentSetting("ieee_floating_point_ops", ieee) && !ieee.IsNull()) {
		options.ieee_floating_point_ops = ieee.GetValue<bool>();
	}
	Value division_errors;
	if (context.TryGetCurrentSetting("error_on_division_by_zero", division_errors) && !division_errors.IsNull()) {
		options.division_by_zero_errors = division_errors.GetValue<bool>();
	}
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
	idx_t column_index;	 // into the table's columns; INVALID for a computed one
};

//! An operand of an expression the writer renders, with what its type rules
//! need. A constant has no type of its own in the parsed tree -- the binder
//! gives it one from what it meets -- so it waits (`constant`) until its peer
//! is known, and is then typed as DuckDB would type it or vetoed.
struct Operand {
	std::string sql;
	//! Its DuckDB type; INVALID when the writer cannot know it for sure (a
	//! decimal product: DuckDB's and the server's scales differ).
	LogicalType type;
	//! How a constant compared with / combined with it is typed.
	ComparableKind kind = ComparableKind::None;
	//! The table column it IS, for a plain column reference.
	const MSSQLColumnInfo *column = nullptr;
	//! An untyped constant, awaiting its peer.
	const ParsedExpression *constant = nullptr;
	//! It is (a cast of) a division: NULL at a zero divisor where DuckDB says
	//! inf / NaN -- fine as a selected value, not under COALESCE, which would
	//! turn that NULL into its fallback.
	bool division = false;
};

//! Integer and exact-decimal types that arithmetic keeps as they are on both
//! sides: same-type operands give that type, overflow an error on both
//! (measured: tinyint UTINYINT, smallint, int, bigint; decimal(10,2) +
//! decimal(10,2) is DECIMAL(11,2) on both).
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
	bool WritePredicateImpl(const ParsedExpression &expr, std::string &sql);
	bool WriteComparison(const ComparisonExpression &cmp, std::string &sql);
	bool ConstantFor(ComparableKind kind, const LogicalType &target, const std::string &peer_name,
					 const ParsedExpression &expr, Value &out);
	std::string Parameter(const MSSQLColumnInfo *column, const Value &value);
	//! A value: a column, a constant (left untyped, see Operand), or an
	//! arithmetic expression over them.
	bool WriteValue(const ParsedExpression &expr, Operand &out);
	bool WriteArithmetic(const FunctionExpression &fn, Operand &out);
	bool WriteDivide(const FunctionExpression &fn, Operand &out);
	bool WriteCast(const CastExpression &cast, Operand &out);
	bool WriteCase(const CaseExpression &expr, Operand &out);
	//! COALESCE / NULLIF / CASE branches: one type for all, constants typed
	//! from the first operand that has one, a NULL constant kept as NULL.
	bool Unify(std::vector<Operand> &operands, Operand &result, bool consumes_null = false);
	//! Two operands compared: a constant is typed from the other; two values
	//! must have one type, and two strings one collation (else 468).
	bool BindPair(Operand &left, Operand &right, const std::string &what);
	bool WriteIn(const OperatorExpression &op, std::string &sql);
	bool WriteBetween(const BetweenExpression &between, std::string &sql);
	bool WriteLike(const FunctionExpression &fn, std::string &sql);
	//! Types `constant` from `peer` and renders it. An operand of arithmetic
	//! keeps its type in literal form too (see the definition).
	bool BindConstant(const Operand &peer, Operand &constant, bool arithmetic = false);
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
	//! Inside WHERE / a WHEN: a division there is vetoed (see WriteDivide).
	int predicate_depth_ = 0;
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
		case ExpressionClass::FUNCTION:
		case ExpressionClass::CASE:
		case ExpressionClass::CAST:
		case ExpressionClass::OPERATOR: {
			// A computed column: rendered by the same rules as WHERE, named as
			// DuckDB names it (the alias, else the expression's text), typed by
			// the server's describe -- the spec's "types of a pushed query are the
			// server's" (column_types '' for it).
			Operand value;
			if (!WriteValue(*item, value)) {
				return false;
			}
			if (value.constant) {
				return Veto("a constant in the select list");
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
			outputs_.push_back(OutputColumn{output, DConstants::INVALID_INDEX});
			out_.column_types.push_back(LogicalType::INVALID);
			break;
		}
		default:
			return Veto("select-list expression " + item->ToString());
		}
	}
	out_.statement += list;
	return true;
}

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
		if (kind != ComparableKind::ExactNumeric) {
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
	const size_t before = params_.params.size();
	std::string sql =
		ExpressionVocabulary::Constant(value, value.type(), options_.parameterize ? &params_ : nullptr, column);
	if (params_.params.size() > before) {
		param_values_.push_back(value);
	}
	return sql;
}

//! Integer width rank, for a widening cast; -1 for a non-integer.
static int IntegerRank(LogicalTypeId id) {
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
	const size_t before = params_.params.size();
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
		if (params_.params.size() == before) {
			constant.sql = ExpressionVocabulary::Cast(constant.sql, own);
		}
	} else if (arithmetic && params_.params.size() == before) {
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
		if (!ResolveColumn(expr.Cast<ColumnRefExpression>(), index)) {
			return false;
		}
		const auto &column = columns_[index];
		out.sql = ColumnSql(index);
		out.type = column.duckdb_type;
		out.kind = KindOf(column);
		out.column = &column;
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
	case ExpressionClass::FUNCTION: {
		auto &fn = expr.Cast<FunctionExpression>();
		const auto &name = fn.FunctionName().GetIdentifierName();
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
	}
	result = Operand();
	result.type = typed_peer.type;
	result.kind = typed_peer.kind;
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
	as_double.kind = ComparableKind::ExactNumeric;
	as_double.type = LogicalType::DOUBLE;
	as_double.sql = "a DOUBLE operand";
	for (auto &operand : operands) {
		if (operand.constant) {
			if (!BindConstant(as_double, operand, true)) {
				return false;
			}
			continue;
		}
		if (operand.kind != ComparableKind::ExactNumeric ||
			(!IsArithmeticType(operand.type) && operand.type.id() != LogicalTypeId::DOUBLE)) {
			return Veto("division over " + operand.type.ToString());
		}
	}
	out.sql = ExpressionVocabulary::Divide(operands[0].sql, operands[1].sql);
	out.type = LogicalType::DOUBLE;
	out.kind = ComparableKind::ExactNumeric;
	out.division = true;
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
	if (child.constant || child.kind != ComparableKind::ExactNumeric) {
		return Veto("cast " + cast.ToString());
	}
	// Widening only, where both sides convert exactly the same: a wider
	// integer, or a double (IEEE nearest on both). Anything that can fail,
	// round or format -- to a string, a date, a narrower type -- stays DuckDB's.
	const int from = IntegerRank(child.type.id());
	const int to = IntegerRank(target);
	std::string tsql;
	if (to > 0 && from > 0 && to >= from) {
		tsql = target == LogicalTypeId::SMALLINT ? "smallint" : target == LogicalTypeId::INTEGER ? "int" : "bigint";
	} else if (target == LogicalTypeId::DOUBLE &&
			   (from > 0 || child.type.id() == LogicalTypeId::DECIMAL || child.type.id() == LogicalTypeId::DOUBLE)) {
		tsql = "float";
	} else {
		return Veto("cast " + cast.ToString());
	}
	out = Operand();
	out.division = child.division;
	out.sql = ExpressionVocabulary::Cast(child.sql, tsql);
	out.type = target == LogicalTypeId::DOUBLE ? LogicalType::DOUBLE : LogicalType(target);
	out.kind = ComparableKind::ExactNumeric;
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
	if (name == "%" && !options_.division_by_zero_errors) {
		// DuckDB then gives NULL for `x % 0`, the server error 8134.
		return Veto("% with error_on_division_by_zero = false");
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
		if (operand.constant || operand.kind != ComparableKind::ExactNumeric ||
			operand.type.id() == LogicalTypeId::UTINYINT || !IsArithmeticType(operand.type)) {
			return Veto("negation " + fn.ToString());
		}
		std::string why;
		auto mapping = ExpressionVocabulary::FunctionFor("negate", {operand.type}, why);
		if (!mapping) {
			return Veto(why);
		}
		out.sql = ExpressionVocabulary::ApplyFunction(*mapping, {operand.sql});
		out.type = operand.type;
		out.kind = ComparableKind::ExactNumeric;
		return true;
	}
	auto &left = operands[0];
	auto &right = operands[1];
	if (left.constant && right.constant) {
		return Veto("arithmetic on two constants " + fn.ToString());
	}
	if (left.constant && !BindConstant(right, left, true)) {
		return false;
	}
	if (right.constant && !BindConstant(left, right, true)) {
		return false;
	}
	// Same type on both sides, or the promotions differ: DuckDB widens
	// smallint + int to INTEGER, the server by its own precedence.
	if (left.kind != ComparableKind::ExactNumeric || right.kind != ComparableKind::ExactNumeric ||
		!IsArithmeticType(left.type) || left.type != right.type) {
		return Veto("arithmetic over " + left.type.ToString() + " and " + right.type.ToString());
	}
	std::string why;
	auto mapping = ExpressionVocabulary::FunctionFor(name, {left.type, right.type}, why);
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
	} else {
		// A product (FunctionFor, below, refuses the ones DuckDB caps at
		// DECIMAL(18) and the ones past the server's 38 digits): the value
		// agrees, the type does not (decimal(p1 + p2 + 1, …) there), so nothing
		// is compared with a product.
		if (2 * DecimalType::GetWidth(left.type) + 1 > 38) {
			return Veto("a decimal product wider than 38 digits");
		}
		out.type = LogicalType::INVALID;
		out.kind = ComparableKind::None;
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
			if (index == DConstants::INVALID_INDEX) {
				return Veto("ORDER BY a computed column");
			}
		} else if (key.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto &ref = key.Cast<ColumnRefExpression>();
			if (!ref.IsQualified()) {
				for (auto &output : outputs_) {
					if (StringUtil::CIEquals(output.name, ref.GetColumnName().GetIdentifierName())) {
						if (output.column_index == DConstants::INVALID_INDEX) {
							return Veto("ORDER BY a computed column");
						}
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
	if (type.id() == LogicalTypeId::INVALID) {
		// A computed column: '' leaves the vehicle the type the server describes.
		return std::string();
	}
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
