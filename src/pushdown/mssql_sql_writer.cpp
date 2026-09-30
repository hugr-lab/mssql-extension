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
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"

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
	options.division_by_zero_errors = LoadErrorOnDivisionByZero(context);
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
enum class ComparableKind { None, Boolean, ExactNumeric, Float, Date, String };

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

//! A builtin DuckDB type named by a cast target with no modifiers: what the
//! rewriter's constant folding leaves for `DATE '2024-01-01'`.
bool BuiltinTypeOf(const TypeExpression &type, LogicalTypeId &out) {
	if (!type.GetChildren().empty() || !type.GetSchema().empty() || !type.GetCatalog().empty()) {
		return false;
	}
	out = TransformStringToLogicalTypeId(type.GetTypeName().GetIdentifierName());
	return out != LogicalTypeId::INVALID && out != LogicalTypeId::UNBOUND;
}

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
	//! A floating-point aggregate (or a value computed from one): DuckDB and
	//! the server accumulate in different orders, so it differs in its last
	//! bits (spec 079 D2) -- recorded for a value, vetoed where it would choose
	//! rows (a condition, an ORDER BY, a DISTINCT).
	bool approximate = false;
	//! An aggregate the server orders as DuckDB does (COUNT, an exact SUM, MIN
	//! / MAX of a column that orders alike): an ORDER BY key.
	bool orderable = false;
	//! Never NULL (COUNT): no NULL placement to emulate when ordered.
	bool not_null = false;
	//! The DuckDB type of the value as a result column, when the writer knows
	//! it (an aggregate); INVALID leaves it to the server's describe.
	LogicalType result_type;
	//! `result_type` is not what the wire decodes into: cast after the read.
	bool cast_result = false;
	//! A decimal whose DuckDB type the writer cannot tell: a constant took
	//! part, which DuckDB types by its own literal rules (`v + 700` is
	//! DECIMAL(13,2) there, decimal(11,2) on the server). Fine in a condition,
	//! not as a result column.
	bool type_uncertain = false;
	//! An integer `x + c`, `x - c`, `x * c` or `-x`: DuckDB's optimizer moves
	//! the constant across a comparison with a constant (MoveConstantsRule, on
	//! integral types; MoveUnaryMinusRule: `t - 5 > 3` is `t > 8` there) and
	//! never computes the value the server would -- and overflow on (tinyint
	//! `t - 5`, error 8115). A decimal or a double DuckDB computes as the
	//! server does.
	bool moves_constant = false;
};

struct OutputColumn {
	std::string name;	 // the name DuckDB gives the result column
	idx_t column_index;	 // into the table's columns; INVALID for a computed one
	//! A column read as itself (no CAST in BuildReadExpression): what an ORDER
	//! BY under DISTINCT can name (else T-SQL error 145).
	bool plain_read = false;
	//! A computed column's value.
	Operand value;
};

//! The aggregates the writer renders, by DuckDB name.
enum class AggregateKind { CountStar, Count, Sum, Avg, Min, Max, Stdev, StdevP, Var, VarP };

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

//! Whether `expr` holds one of those aggregates -- what makes a node an
//! aggregate query in DuckDB's binder as in T-SQL.
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

//! A column GROUP BY and DISTINCT may take: one the server compares -- and
//! so groups -- as DuckDB does (strings under D4: their sets are the
//! server's). Geometry, xml and text are not comparable there at all (249,
//! 421); datetime2(7) reads values past 2262 as NULL (#168).
bool IsGroupable(const MSSQLColumnInfo &column) {
	const auto type = StringUtil::Lower(column.sql_type_name);
	if ((type == "time" || type == "datetimeoffset") && column.scale >= 7) {
		// Read as microseconds: two values 100 ns apart are two groups there and
		// one value here -- harmless for an order, not for a set.
		return false;
	}
	return KindOf(column) != ComparableKind::None || column.OrdersLikeDuckDB();
}

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

//! Integer width rank, for a widening cast; -1 for a non-integer.
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

//! One table of the FROM.
struct Relation {
	WriterTable table;
	std::string name;		// how the query names it: its alias, else the table's name
	std::string sql;		// how the statement names it: [r1] under a join, empty for one table
	bool nullable = false;	// on the NULL-supplying side of an outer join
	//! The right side of a SEMI / ANTI join: sent as an EXISTS / NOT EXISTS,
	//! its columns visible only in that join's own condition.
	bool semi = false;
};

class NodeWriter {
public:
	NodeWriter(const SQLWriterOptions &options, const SQLWriter::TableResolver &resolver, WrittenQuery &out,
			   std::string &why)
		: options_(options), resolver_(resolver), out_(out), why_(why) {}

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
	//! ResolveColumn without the veto: 1 found, 0 not a column of the table,
	//! -1 a reference no lookup can answer (another relation, ambiguous).
	int FindColumn(const ColumnRefExpression &ref, idx_t &out) const;
	//! After grouping (SELECT, HAVING, ORDER BY of an aggregate query), a bare
	//! column must be a GROUP BY key -- DuckDB's binder error otherwise, which
	//! the server's (8120) must not replace.
	bool CheckGrouped(idx_t index);
	bool WriteGroups(const SelectNode &node, std::string &sql);
	bool GroupKeyColumn(const SelectNode &node, const ParsedExpression &key, idx_t &out);
	bool WriteAggregate(const FunctionExpression &fn, AggregateKind kind, Operand &out);
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
	//! A column in WHERE / GROUP BY / an expression. One table: its bare
	//! name. A join: qualified by the relation's own alias, `[r1].[c]`, which no
	//! user alias or table name can shadow.
	std::string ColumnSql(idx_t index) const {
		auto &relation = relations_[relation_of_[index]];
		return (relation.sql.empty() ? "" : relation.sql + ".") + QuoteIdentifier(columns_[index].name);
	}
	//! A column as ORDER BY must name it: always qualified, as a bare name there
	//! is a select-list alias first (measured: `SELECT [id] AS [value] ...
	//! ORDER BY [value]` sorts by id).
	std::string OrderColumnSql(idx_t index) const {
		auto &relation = relations_[relation_of_[index]];
		return (relation.sql.empty() ? QuoteIdentifier(relation.table.name) : relation.sql) + "." +
			   QuoteIdentifier(columns_[index].name);
	}
	//! The FROM: every base table resolved (its columns appended to the flat
	//! list), then the join tree rendered with its conditions.
	bool CollectRelations(const TableRef &ref, std::vector<idx_t> &members);
	bool WriteFrom(const TableRef &ref, std::string &sql);
	//! The columns of `relation` that a `*` over the whole FROM lists: all, but
	//! a USING column of the right side only once (on the left).
	bool IsHiddenUsingColumn(idx_t index) const;
	bool WriteOrder(const OrderModifier &order, bool limited, std::string &sql);
	bool ConstantCount(const ParsedExpression &expr, int64_t &out);

	const SQLWriterOptions &options_;
	const SQLWriter::TableResolver &resolver_;
	std::vector<Relation> relations_;
	//! Every column of every relation, in FROM order -- copies, so a column on
	//! the NULL-supplying side of an outer join can say it is nullable.
	std::vector<MSSQLColumnInfo> columns_;
	//! The DuckDB type the catalog reports for each, parallel to columns_.
	std::vector<LogicalType> types_;
	std::vector<idx_t> relation_of_;
	//! A USING column's name -> the column an unqualified reference to it
	//! means: the left side's (USING in a RIGHT / FULL join is vetoed).
	std::vector<std::pair<std::string, idx_t>> using_columns_;
	//! The right side's USING columns, which `*` leaves out.
	std::vector<idx_t> hidden_using_;
	//! The relations a column reference can see: while the FROM is written,
	//! those joined so far (an ON condition cannot name a later one).
	idx_t visible_ = DConstants::INVALID_INDEX;
	//! The SEMI / ANTI relation whose condition is being written: the one
	//! place its columns can be named.
	idx_t semi_scope_ = DConstants::INVALID_INDEX;
	//! The EXISTS / NOT EXISTS a SEMI / ANTI join becomes, ANDed to WHERE.
	std::vector<std::string> semi_filters_;
	WrittenQuery &out_;
	std::string &why_;
	std::vector<OutputColumn> outputs_;
	//! Inside WHERE / a WHEN: a division there is vetoed (see WriteDivide).
	int predicate_depth_ = 0;
	//! GROUP BY, HAVING or an aggregate: the node is an aggregate query.
	bool aggregated_ = false;
	//! The columns GROUP BY names.
	std::vector<idx_t> group_keys_;
	//! Writing SELECT / HAVING / ORDER BY -- evaluated after grouping, where an
	//! aggregate may appear -- rather than WHERE.
	bool after_grouping_ = false;
	//! Writing an aggregate's argument.
	bool in_aggregate_ = false;
	bool distinct_ = false;
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

int NodeWriter::FindColumn(const ColumnRefExpression &ref, idx_t &out) const {
	auto &names = ref.ColumnNames();
	if (names.size() > 2) {
		return -1;
	}
	const auto &name = names.back().GetIdentifierName();
	idx_t relation = DConstants::INVALID_INDEX;
	if (names.size() == 2) {
		for (idx_t r = 0; r < relations_.size(); r++) {
			if (StringUtil::CIEquals(names[0].GetIdentifierName(), relations_[r].name)) {
				relation = r;
			}
		}
		if (relation == DConstants::INVALID_INDEX) {
			return -1;
		}
	} else {
		// An unqualified USING column is the merged one DuckDB's binder makes.
		for (auto &merged : using_columns_) {
			if (StringUtil::CIEquals(merged.first, name)) {
				out = merged.second;
				return 1;
			}
		}
	}
	if (relation != DConstants::INVALID_INDEX && ((visible_ != DConstants::INVALID_INDEX && relation >= visible_) ||
												  (relations_[relation].semi && relation != semi_scope_))) {
		return -1;
	}
	bool found = false;
	for (idx_t i = 0; i < columns_.size(); i++) {
		if ((visible_ != DConstants::INVALID_INDEX && relation_of_[i] >= visible_) ||
			(relations_[relation_of_[i]].semi && relation_of_[i] != semi_scope_) ||
			(relation != DConstants::INVALID_INDEX && relation_of_[i] != relation) ||
			!StringUtil::CIEquals(columns_[i].name, name)) {
			continue;
		}
		if (found) {
			// Two relations with the column (DuckDB's ambiguity error), or a
			// case-sensitive database holding `a` and `A`.
			return -1;
		}
		found = true;
		out = i;
	}
	return found ? 1 : 0;
}

bool NodeWriter::ResolveColumn(const ColumnRefExpression &ref, idx_t &out) {
	auto &names = ref.ColumnNames();
	switch (FindColumn(ref, out)) {
	case 1:
		return true;
	case 0:
		return Veto("column " + names.back().GetIdentifierName() + " is not a column of the query's tables");
	default:
		if (names.size() > 2) {
			return Veto("column reference " + ref.ToString() + " has more than two parts");
		}
		if (names.size() == 2) {
			bool known = false;
			for (auto &relation : relations_) {
				known = known || StringUtil::CIEquals(names[0].GetIdentifierName(), relation.name);
			}
			if (!known) {
				return Veto("column reference " + ref.ToString() + " names another relation");
			}
			for (auto &relation : relations_) {
				if (relation.semi && StringUtil::CIEquals(names[0].GetIdentifierName(), relation.name)) {
					return Veto("column reference " + ref.ToString() +
								" names a SEMI / ANTI join's table, which only its condition sees");
				}
			}
		}
		return Veto("column name " + names.back().GetIdentifierName() + " matches more than one column");
	}
}

bool NodeWriter::IsHiddenUsingColumn(idx_t index) const {
	for (auto hidden : hidden_using_) {
		if (hidden == index) {
			return true;
		}
	}
	return false;
}

bool NodeWriter::CollectRelations(const TableRef &ref, std::vector<idx_t> &members) {
	if (ref.type == TableReferenceType::BASE_TABLE) {
		auto &base = ref.Cast<BaseTableRef>();
		if (base.sample || !base.column_name_alias.empty() || base.at_clause) {
			return Veto("TABLESAMPLE / column aliases / AT on a table");
		}
		Relation relation;
		if (!resolver_(base, relation.table) || !relation.table.columns) {
			return Veto("table " + base.Table().GetIdentifierName() + " does not resolve in this catalog");
		}
		relation.name = base.alias.empty() ? relation.table.name : base.alias.GetIdentifierName();
		for (auto &other : relations_) {
			if (StringUtil::CIEquals(other.name, relation.name)) {
				// A self-join without aliases, or two schemas' tables of one name --
				// which the rewriter's strip would also merge (`db.s1.t.c` -> `t.c`).
				return Veto("two tables named " + relation.name);
			}
		}
		const auto &columns = *relation.table.columns;
		for (idx_t i = 0; i < columns.size(); i++) {
			columns_.push_back(columns[i]);
			types_.push_back(relation.table.types ? (*relation.table.types)[i] : columns[i].NativeDuckDBType());
			relation_of_.push_back(relations_.size());
		}
		members.push_back(relations_.size());
		relations_.push_back(std::move(relation));
		return true;
	}
	if (ref.type != TableReferenceType::JOIN) {
		return Veto("FROM holds a " + std::string(ref.type == TableReferenceType::SUBQUERY ? "subquery" : "non-table"));
	}
	auto &join = ref.Cast<JoinRef>();
	if (!join.alias.empty() || !join.column_name_alias.empty() || !join.duplicate_eliminated_columns.empty() ||
		join.sample) {
		return Veto("an alias, sample or column aliases on a join");
	}
	if (join.ref_type != JoinRefType::REGULAR && join.ref_type != JoinRefType::CROSS) {
		return Veto("a NATURAL / POSITIONAL / ASOF / LATERAL / NEAREST join");
	}
	const bool semi = join.type == JoinType::SEMI || join.type == JoinType::ANTI;
	if (join.type != JoinType::INNER && join.type != JoinType::LEFT && join.type != JoinType::RIGHT &&
		join.type != JoinType::OUTER && !semi) {
		return Veto("a MARK / SINGLE join");
	}
	if (semi && join.ref_type == JoinRefType::CROSS) {
		return Veto("a SEMI / ANTI join without a condition");
	}
	if (join.right->type != TableReferenceType::BASE_TABLE) {
		// Left-deep only: `a JOIN (b JOIN c)` would need its own parentheses and
		// the nullability of a nested outer join.
		return Veto("a join whose right side is not a table");
	}
	std::vector<idx_t> left;
	std::vector<idx_t> right;
	if (!CollectRelations(*join.left, left) || !CollectRelations(*join.right, right)) {
		return false;
	}
	bool semi_on_left = false;
	for (auto r : left) {
		semi_on_left = semi_on_left || relations_[r].semi;
	}
	if (semi) {
		for (auto r : right) {
			relations_[r].semi = true;
		}
	}
	if (semi_on_left && (join.type == JoinType::RIGHT || join.type == JoinType::OUTER)) {
		// A SEMI join's EXISTS is ANDed to WHERE, after every join. That is
		// exact for the rows its left side produces, which later INNER and LEFT
		// joins keep, and whatever joins came before it -- but a later RIGHT /
		// FULL join adds rows with that side NULL, which the moved EXISTS would
		// drop.
		return Veto("a RIGHT / FULL join over a SEMI / ANTI join");
	}
	// The NULL-supplying side: its columns can be NULL whatever they declare.
	if (join.type == JoinType::LEFT || join.type == JoinType::OUTER) {
		for (auto r : right) {
			relations_[r].nullable = true;
		}
	}
	if (join.type == JoinType::RIGHT || join.type == JoinType::OUTER) {
		for (auto r : left) {
			relations_[r].nullable = true;
		}
	}
	members.insert(members.end(), left.begin(), left.end());
	members.insert(members.end(), right.begin(), right.end());
	return true;
}

bool NodeWriter::WriteFrom(const TableRef &ref, std::string &sql) {
	if (ref.type == TableReferenceType::BASE_TABLE) {
		// The relations were collected in this same order.
		auto &relation = relations_[visible_++];
		sql = QuoteIdentifier(relation.table.schema) + "." + QuoteIdentifier(relation.table.name);
		if (!relation.sql.empty()) {
			sql += " AS " + relation.sql;
		}
		return true;
	}
	auto &join = ref.Cast<JoinRef>();
	const idx_t left_first = visible_;
	std::string left;
	std::string right;
	if (!WriteFrom(*join.left, left)) {
		return false;
	}
	const idx_t right_relation = visible_;
	if (!WriteFrom(*join.right, right)) {
		return false;
	}
	if (join.ref_type == JoinRefType::CROSS) {
		sql = left + " CROSS JOIN " + right;
		return true;
	}
	const bool semi = join.type == JoinType::SEMI || join.type == JoinType::ANTI;
	if (semi) {
		// Its columns are named in its condition only.
		semi_scope_ = right_relation;
	}
	const char *keyword = join.type == JoinType::INNER	 ? " INNER JOIN "
						  : join.type == JoinType::LEFT	 ? " LEFT JOIN "
						  : join.type == JoinType::RIGHT ? " RIGHT JOIN "
														 : " FULL JOIN ";
	std::string condition;
	if (!join.using_columns.empty()) {
		if (join.type == JoinType::RIGHT || join.type == JoinType::OUTER) {
			// The merged column is the right side's, or COALESCE of both: its
			// place in `*` and its value are not the left column's.
			return Veto("USING in a RIGHT / FULL join");
		}
		std::vector<std::string> parts;
		for (auto &using_name : join.using_columns) {
			const auto &name = using_name.GetIdentifierName();
			idx_t left_column = DConstants::INVALID_INDEX;
			idx_t right_column = DConstants::INVALID_INDEX;
			for (idx_t i = 0; i < columns_.size(); i++) {
				if (!StringUtil::CIEquals(columns_[i].name, name)) {
					continue;
				}
				// A SEMI / ANTI join's table is not on the left: it left no columns.
				if (relation_of_[i] >= left_first && relation_of_[i] < right_relation &&
					!relations_[relation_of_[i]].semi) {
					if (left_column != DConstants::INVALID_INDEX) {
						return Veto("USING column " + name + " is in more than one table on the left");
					}
					left_column = i;
				} else if (relation_of_[i] == right_relation) {
					right_column = i;
				}
			}
			if (left_column == DConstants::INVALID_INDEX || right_column == DConstants::INVALID_INDEX) {
				return Veto("USING column " + name + " is not on both sides");
			}
			Operand l;
			Operand r;
			for (auto side : {std::make_pair(left_column, &l), std::make_pair(right_column, &r)}) {
				auto &operand = *side.second;
				operand.sql = ColumnSql(side.first);
				operand.type = columns_[side.first].duckdb_type;
				operand.kind = KindOf(columns_[side.first]);
				operand.column = &columns_[side.first];
			}
			if (!BindPair(l, r, "USING (" + name + ")")) {
				return false;
			}
			parts.push_back(ExpressionVocabulary::Comparison(" = ", l.sql, r.sql));
			using_columns_.emplace_back(name, left_column);
			hidden_using_.push_back(right_column);
		}
		condition = ExpressionVocabulary::Conjunction(parts, true);
	} else {
		if (!join.condition) {
			return Veto("a join without a condition");
		}
		// ON sees the tables joined so far -- a later one is DuckDB's binder
		// error, not a statement for the server.
		if (!WritePredicate(*join.condition, condition)) {
			return false;
		}
	}
	if (semi) {
		// A SEMI join keeps each left row that has a match, once; an ANTI join
		// each that has none -- EXISTS / NOT EXISTS, not IN / NOT IN, which a
		// NULL on the right would empty (measured on DuckDB: ANTI JOIN gives
		// [2, NULL] where NOT IN gives nothing).
		semi_scope_ = DConstants::INVALID_INDEX;
		const std::string exists = "EXISTS (SELECT 1 FROM " + right + " WHERE " + condition + ")";
		semi_filters_.push_back(join.type == JoinType::ANTI ? ExpressionVocabulary::Not(exists) : exists);
		sql = left;
		return true;
	}
	sql = left + keyword + right + " ON " + condition;
	return true;
}

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
		const auto read =
			MSSQLColumnInfo::BuildReadExpression(column.name, column.sql_type_name, column.max_length,
												 column.collation_name, options_.convert_varchar_max, "", "");
		const auto &relation = relations_[relation_of_[index]];
		list += MSSQLColumnInfo::BuildReadExpression(column.name, column.sql_type_name, column.max_length,
													 column.collation_name, options_.convert_varchar_max,
													 relation.sql.empty() ? "" : relation.sql + ".", alias);
		OutputColumn entry;
		entry.name = output;
		entry.column_index = index;
		entry.plain_read = read == QuoteIdentifier(column.name);
		outputs_.push_back(std::move(entry));
		out_.column_types.push_back(types_[index]);
		out_.cast_types.push_back(LogicalType::INVALID);
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
		case ExpressionClass::FUNCTION:
		case ExpressionClass::CASE:
		case ExpressionClass::CAST:
		case ExpressionClass::OPERATOR: {
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
	const size_t before = params_.params.size();
	std::string sql =
		ExpressionVocabulary::Constant(value, value.type(), options_.parameterize ? &params_ : nullptr, column);
	if (params_.params.size() > before) {
		param_values_.push_back(value);
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
		if (!CheckGrouped(index)) {
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
		AggregateKind aggregate;
		if (AggregateFor(name, aggregate)) {
			return WriteAggregate(fn, aggregate, out);
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
	if (!node.cte_map.map.empty()) {
		return Veto("a WITH clause");
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
	std::vector<idx_t> members;
	if (!CollectRelations(*node.from_table, members)) {
		return false;
	}
	if (relations_.size() > 1) {
		// A join: every relation under its own alias, every column qualified.
		for (idx_t r = 0; r < relations_.size(); r++) {
			relations_[r].sql = "[r" + std::to_string(r + 1) + "]";
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
	NodeWriter writer(options_, resolver_, out, why);
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
			modifier->type == ResultModifierType::LIMIT_MODIFIER ||
			modifier->type == ResultModifierType::DISTINCT_MODIFIER) {
			return true;
		}
	}
	if (node.type != QueryNodeType::SELECT_NODE) {
		return false;
	}
	auto &select = node.Cast<SelectNode>();
	// A join with conditions: the server matches the rows, where the scans
	// would each send their whole (filtered) table. A CROSS JOIN anywhere in
	// the chain -- a comma join included -- can send the product, more rather
	// than less, so it is a gain only with one of the others below.
	bool joined = false;
	bool crossed = false;
	for (auto ref = select.from_table.get(); ref && ref->type == TableReferenceType::JOIN;
		 ref = ref->Cast<JoinRef>().left.get()) {
		joined = true;
		crossed = crossed || ref->Cast<JoinRef>().ref_type == JoinRefType::CROSS;
	}
	if (joined && !crossed) {
		return true;
	}
	if (!select.groups.group_expressions.empty() || !select.groups.grouping_sets.empty() || select.having ||
		select.aggregate_handling != AggregateHandling::STANDARD_HANDLING) {
		return true;
	}
	for (auto &item : select.select_list) {
		if (ContainsAggregate(*item)) {
			return true;
		}
	}
	return false;
}

}  // namespace mssql
}  // namespace duckdb
