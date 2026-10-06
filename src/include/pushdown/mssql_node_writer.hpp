#pragma once

//===----------------------------------------------------------------------===//
// Spec 079: the node writer behind SQLWriter -- INTERNAL to src/pushdown.
// Its methods are split by concern: mssql_node_writer_from.cpp (relations,
// column resolution, FROM, derived tables, CTEs, keys and the gain),
// _select.cpp (a SELECT node, its select list, aggregates, groups, ORDER BY,
// set operations), _values.cpp (constants, arithmetic, casts, CASE,
// comparisons, IN / BETWEEN / LIKE, predicates), _subqueries.cpp (correlation
// and subquery expressions); the free helpers below in mssql_node_writer.cpp.
// The public SQLWriter, ColumnTypeName, the counters and PushesMoreThanScan
// stay in mssql_sql_writer.cpp.
//===----------------------------------------------------------------------===//

#include "catalog/mssql_column_info.hpp"
#include "pushdown/mssql_sql_writer.hpp"
#include "query/mssql_identifier.hpp"
#include "query/mssql_sql_params.hpp"

#include <deque>

#include "duckdb/parser/expression/between_expression.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/expression/type_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

namespace duckdb {
namespace mssql {
namespace node_writer {

//! A SQL Server type whose values `=`, `<` … compare as DuckDB compares the
//! column's DuckDB type, for a constant cast to that type. PR B's list: exact
//! numerics, bit, date and the character types (D4: their sets are the
//! server's, under the column's collation). Time-of-day and the datetime
//! family wait for PR C: a constant finer than the column's precision is
//! ROUNDED into the parameter on the server and compared exactly by DuckDB
//! (the #358 lesson), which needs its own rule. text / ntext / xml / the
//! cast-required types cannot be compared with `=` at all.
enum class ComparableKind { None, Boolean, ExactNumeric, Float, Date, String };

ComparableKind KindOf(const MSSQLColumnInfo &column);

//! A builtin DuckDB type named by a cast target with no modifiers: what the
//! rewriter's constant folding leaves for `DATE '2024-01-01'`.
bool BuiltinTypeOf(const TypeExpression &type, LogicalTypeId &out);

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

//! The largest derived table / inlined CTE the writer renders.
constexpr size_t MAX_DERIVED_SQL = 1 << 20;

//! The aggregates the writer renders, by DuckDB name.
enum class AggregateKind { CountStar, Count, Sum, Avg, Min, Max, Stdev, StdevP, Var, VarP, StringAgg };

bool AggregateFor(const std::string &function_name, AggregateKind &out);

//! One aggregate call as the writer needs it, from a FunctionExpression or
//! from a window's aggregate (spec 079 PR E2): both carry their arguments as
//! FunctionArguments.
struct AggregateCall {
	const std::vector<FunctionArgument> &args;
	bool distinct;
	const ParsedExpression *filter;
	//! An ordered aggregate's ORDER BY (string_agg); null when none.
	const std::vector<OrderByNode> *orders;
	//! Exported state or a qualified name: not one of ours.
	bool foreign;
	std::string name;
	std::string text;
	//! ` OVER (...)` after every aggregate call, for a window's aggregate.
	std::string over;
};

//! Whether `expr` holds one of those aggregates -- what makes a node an
//! aggregate query in DuckDB's binder as in T-SQL.
bool ContainsAggregate(const ParsedExpression &expr);

//! Whether a node's own expressions hold a subquery (PR E1) -- not those of
//! a derived table in its FROM, which its own writer renders.
bool HasSubqueryExpression(const SelectNode &node);

//! A column GROUP BY and DISTINCT may take: one the server compares -- and
//! so groups -- as DuckDB does (strings under D4: their sets are the
//! server's). Geometry, xml and text are not comparable there at all (249,
//! 421); datetime2(7) reads values past 2262 as NULL (#168).
bool IsGroupable(const MSSQLColumnInfo &column);

//! Integer and exact-decimal types that arithmetic keeps as they are on both
//! sides: same-type operands give that type, overflow an error on both
//! (measured: tinyint UTINYINT, smallint, int, bigint; decimal(10,2) +
//! decimal(10,2) is DECIMAL(11,2) on both).
bool IsArithmeticType(const LogicalType &type);

//! Integer width rank, for a widening cast; -1 for a non-integer.
int IntegerRank(LogicalTypeId id);

//! One table of the FROM.
struct Relation {
	WriterTable table;
	std::string name;		// how the query names it: its alias, else the table's name
	std::string sql;		// how the statement names it: [r1] under a join, empty for one table
	bool nullable = false;	// on the NULL-supplying side of an outer join
	//! The right side of a SEMI / ANTI join: sent as an EXISTS / NOT EXISTS,
	//! its columns visible only in that join's own condition.
	bool semi = false;
	//! A subquery in FROM (PR E1): its node rendered as `(<T-SQL>) AS [rN]`;
	//! its columns are that node's results, already read (a CAST, STAsBinary
	//! or a cast back applied inside).
	bool derived = false;
	std::string derived_sql;
	//! A derived table's total_input_rows / total_size_unknown.
	idx_t total_rows = 0;
	bool total_unknown = false;
};

//! A derived table's column as the outer node sees it. A column of the inner
//! node read as itself keeps that column's metadata -- its type, collation,
//! comparability; a computed one (an aggregate, arithmetic) gets metadata of
//! its type where the type maps to a T-SQL type whose values compare as
//! DuckDB's do, else it is opaque: selectable, never compared nor ordered.
MSSQLColumnInfo DerivedColumn(const std::string &name, const LogicalType &type);

class NodeWriter {
public:
	NodeWriter(const SQLWriterOptions &options, const SQLWriter::TableResolver &resolver, WrittenQuery &out,
			   std::string &why)
		: options_(options),
		  resolver_(resolver),
		  out_(out),
		  why_(why),
		  params_(&own_params_),
		  param_values_(&own_param_values_) {}
	//! A nested node's writer (a derived table): the statement's parameters
	//! are one set, numbered once, whichever node renders them.
	NodeWriter(NodeWriter &parent, WrittenQuery &out)
		: options_(parent.options_),
		  resolver_(parent.resolver_),
		  out_(out),
		  why_(parent.why_),
		  params_(parent.params_),
		  param_values_(parent.param_values_),
		  written_unqualified_(parent.written_unqualified_),
		  cte_parent_(&parent) {}
	//! A subquery in an expression (PR E1): it may name the columns of the
	//! nodes around it (correlation), so its relations are aliased past theirs.
	struct Correlated {};
	NodeWriter(NodeWriter &parent, WrittenQuery &out, Correlated) : NodeWriter(parent, out) {
		outer_ = &parent;
		alias_base_ = parent.alias_base_ + parent.relations_.size();
	}

	bool Write(const SelectNode &node);
	void SetWrittenUnqualified(const SQLWriter::QualificationProbe *probe) {
		written_unqualified_ = probe;
	}
	//! A SELECT, or a set operation (PR E1: nested in a subquery; the one at
	//! a statement's top stays with DuckDB, see MSSQLCatalog::RemoteExecute).
	bool WriteQueryNode(const QueryNode &node);

private:
	bool WriteSetOperation(const SetOperationNode &node);
	//! A CTE of a WITH clause, inlined where it is referenced (PR E1): T-SQL's
	//! WITH stands only at a statement's top, and the server inlines its own
	//! CTEs anyway. DuckDB evaluates one once, so a body that is not the same
	//! on every evaluation -- a LIMIT picks among ties -- is inlined only once,
	//! and never where a subquery expression could evaluate it per row.
	struct CteEntry {
		std::string name;
		const QueryNode *body;
		idx_t uses;
	};
	//! The CTE `name` means here: this node's, then its parents' -- where a CTE
	//! body sees only the CTEs defined before it. `owner` is the writer that
	//! holds it, `index` its place there.
	CteEntry *FindCte(const std::string &name, NodeWriter *&owner, idx_t &index);
	bool IsWrittenUnqualified(const BaseTableRef &ref) const;
	//! `per_row`: an inlined CTE body referenced where a subquery expression
	//! evaluates it per row -- a CTE it reads in turn is evaluated so too.
	//! `picks_rows` (if set) is whether the node holds a LIMIT / OFFSET.
	bool CollectDerivedNode(const QueryNode &node, const std::string &name, bool synthetic, NodeWriter *cte_owner,
							idx_t cte_index, bool per_row, bool *picks_rows, std::vector<idx_t> &members);
	//! Whether a node around this one has relations a column could be of.
	bool HasOuterRelations() const {
		for (auto outer = outer_; outer; outer = outer->outer_) {
			if (!outer->relations_.empty()) {
				return true;
			}
		}
		return false;
	}

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
	bool WriteAggregate(const AggregateCall &call, AggregateKind kind, Operand &out);
	bool WriteStringAgg(const AggregateCall &call, const Operand &arg, const std::string &filter, Operand &out);
	//! A window function (spec 079 PR E2): mssql_node_writer_windows.cpp.
	bool WriteWindow(const WindowExpression &window, Operand &out);
	bool WriteWindowFrame(const WindowExpression &window, std::string &sql);
	//! A node with QUALIFY (PR E2): `SELECT * FROM (<the node, its QUALIFY a
	//! hidden bit column>) WHERE <it>` with the node's DISTINCT / ORDER BY /
	//! LIMIT, which DuckDB applies after QUALIFY.
	bool WriteQualified(const SelectNode &node);
	bool WriteQualifyColumn(const SelectNode &node);
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
	bool CollectDerived(const SubqueryRef &ref, std::vector<idx_t> &members);
	//! Read access to a written node's results, for the node around it.
	const std::vector<OutputColumn> &Outputs() const {
		return outputs_;
	}
	const MSSQLColumnInfo &FlatColumn(idx_t index) const {
		return columns_[index];
	}
	bool WriteFrom(const TableRef &ref, std::string &sql);
	//! The columns of `relation` that a `*` over the whole FROM lists: all, but
	//! a USING column of the right side only once (on the left).
	bool IsHiddenUsingColumn(idx_t index) const;
	bool WriteOrder(const OrderModifier &order, bool limited, std::string &sql);
	//! The ORDER BY of an ordered aggregate (`string_agg(x, ',' ORDER BY k)`)
	//! or of a window: keys named from the FROM, never a result name or a
	//! position. The server sorts these rows whatever the key, so a UTF-8
	//! varchar goes by its bytes and NULL placement is emulated without the
	//! LIMIT a statement's ORDER BY needs for either (spec 079 PR E2).
	bool WriteSortKeys(const std::vector<OrderByNode> &orders, std::string &sql);
	bool ConstantCount(const ParsedExpression &expr, int64_t &out, const char *what = "LIMIT / OFFSET");

	const SQLWriterOptions &options_;
	const SQLWriter::TableResolver &resolver_;
	std::vector<Relation> relations_;
	//! Every column of every relation, in FROM order -- copies, so a column on
	//! the NULL-supplying side of an outer join can say it is nullable.
	std::vector<MSSQLColumnInfo> columns_;
	//! The DuckDB type the catalog reports for each, parallel to columns_.
	std::vector<LogicalType> types_;
	std::vector<idx_t> relation_of_;
	//! Per flat column: the type a derived table's column is cast to after the
	//! read (an integer SUM read as decimal(38,0), HUGEINT here); INVALID else.
	std::vector<LogicalType> cast_of_;
	//! Per flat column: a derived table's column, already read by its node.
	std::vector<bool> already_read_;
	//! Per flat column: a derived table's division or floating-point
	//! aggregate -- a value, never a condition's operand outside its node.
	std::vector<bool> divergent_of_;
	//! A USING column's name -> the column an unqualified reference to it
	//! means: the left side's (USING in a RIGHT / FULL join is vetoed).
	std::vector<std::pair<std::string, idx_t>> using_columns_;
	//! The right side's USING columns, which `*` leaves out.
	std::vector<idx_t> hidden_using_;
	//! The relations a column reference can see: while the FROM is written,
	//! those joined so far (an ON condition cannot name a later one).
	idx_t visible_ = DConstants::INVALID_INDEX;
	//! The node around a subquery in an expression, for a correlated column;
	//! null for a statement's node and for a derived table (not correlated).
	NodeWriter *outer_ = nullptr;
	//! Relations are aliased [r<alias_base_ + 1>], ...: a correlated subquery's
	//! aliases must not shadow the outer node's.
	idx_t alias_base_ = 0;
	//! It names a column of a node around it (set on every writer between).
	bool correlated_ = false;
	//! The node's LIMIT, -1 without one.
	int64_t limit_ = -1;
	//! The select list's aliases: a name matching one is DuckDB's alias
	//! reference, never a correlated column.
	std::vector<std::string> select_aliases_;
	//! Whether an unresolved reference may be a correlated one: an unqualified
	//! name no column here answers, or a qualifier no relation here has.
	bool MayBeOuter(const ColumnRefExpression &ref, int found) const;
	//! A column of a node around this one (correlation); `found` false when no
	//! enclosing node has it either.
	bool WriteOuterColumn(const ColumnRefExpression &ref, Operand &out, bool &found);
	bool WriteSubqueryCondition(const SubqueryExpression &subquery, std::string &sql);
	bool WriteScalarSubquery(const SubqueryExpression &subquery, Operand &out);
	//! A subquery's node written by a correlated writer: a SELECT, no ORDER BY
	//! without LIMIT (1033); `columns` its result columns required (0: any).
	bool WriteSubqueryNode(const SubqueryExpression &subquery, NodeWriter &inner, idx_t columns);
	//! A written subquery's single result as an operand of the node around it.
	bool SubqueryOperand(const NodeWriter &inner, const WrittenQuery &inner_out, const std::string &sql, Operand &out);
	//! Copies of the columns a subquery's result is, owned for its operand.
	std::deque<MSSQLColumnInfo> subquery_columns_;
	//! Whether a table was written unqualified (the SQLWriter's probe).
	const SQLWriter::QualificationProbe *written_unqualified_ = nullptr;
	//! This node's CTEs, in their WITH order.
	std::vector<CteEntry> ctes_;
	//! The writer this one was made by, for CTE lookups; a CTE body's is the
	//! writer holding the CTE, of whose CTEs it sees the first
	//! `cte_parent_limit_`.
	NodeWriter *cte_parent_ = nullptr;
	idx_t cte_parent_limit_ = DConstants::INVALID_INDEX;
	//! A subquery expression's node: evaluated per row of the node around it.
	bool in_expression_ = false;
	//! A derived table (or inlined CTE) whose own joins are of uncertain gain:
	//! this node's gain is uncertain too unless it reduces the rows.
	bool derived_uncertain_ = false;
	//! Whether the written node returns at most one row for each row of the
	//! node around it: an equality on every column of its single table's
	//! unique key, with a constant or a column of a node around it.
	bool KeyedToOneRow(const SelectNode &node) const;
	//! The SELECT * wrapper a set operation's ORDER BY / LIMIT is written
	//! through, kept while the writer lives.
	unique_ptr<SelectNode> synthetic_node_;
	//! The wrapper's subquery and the set operation it stands for: its tables
	//! are the original's (the resolver knows those objects, not a copy's).
	const SubqueryRef *synthetic_ref_ = nullptr;
	const SetOperationNode *synthetic_setop_ = nullptr;
	//! The node a QUALIFY wrapper stands for, written by the inner writer.
	const SelectNode *synthetic_select_ = nullptr;
	//! Writing the node inside a QUALIFY wrapper: no modifiers (the wrapper's),
	//! the QUALIFY condition as a hidden result column.
	bool qualifying_ = false;
	//! Writing that condition: a name that is no FROM column may be a
	//! select-list alias (DuckDB's order: the column first).
	bool qualify_aliases_ = false;
	//! The wrapper's hidden column, left out of its `*`.
	std::string hidden_column_;
	//! A QUALIFY on select-list aliases only, filtered by the wrapper on its
	//! columns: the names, none of which may be a FROM column.
	std::vector<std::string> qualify_names_;
	//! Tie-dependent windows written (WriteWindow), and whether the QUALIFY
	//! condition named a computed alias: a window evaluated twice could break
	//! its ties two ways.
	idx_t tie_windows_ = 0;
	bool qualify_named_alias_ = false;
	//! The SEMI / ANTI relation whose condition is being written: the one
	//! place its columns can be named.
	idx_t semi_scope_ = DConstants::INVALID_INDEX;
	//! The EXISTS / NOT EXISTS a SEMI / ANTI join becomes, ANDed to WHERE.
	std::vector<std::string> semi_filters_;
	//! Join links that equate no unique key of either side (PR E1 gain check).
	idx_t unbounded_links_ = 0;
	//! CROSS links (a comma join included), judged once WHERE is known: its
	//! equalities bound them as an ON would. {left_first, right_relation}.
	std::vector<std::pair<idx_t, idx_t>> cross_links_;
	//! Whether `columns` (flat indexes) cover the unique key of every relation
	//! the node joins -- then a GROUP BY / DISTINCT over them reduces nothing.
	bool CoversEveryKey(const std::vector<idx_t> &columns) const;
	//! Column pairs a link's condition equates (flat column indexes).
	std::vector<std::pair<idx_t, idx_t>> link_equalities_;
	void CollectEqualities(const ParsedExpression &condition);
	bool KeyCovered(idx_t relation, idx_t other_first, idx_t other_last) const;
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
	//! The WITHIN GROUP order of the node's first ordered string_agg: the
	//! server refuses two different ones in one scope (8711).
	bool string_agg_ordered_ = false;
	std::string string_agg_order_;
	SqlParamSet own_params_;
	std::vector<Value> own_param_values_;
	SqlParamSet *params_;
	std::vector<Value> *param_values_;

public:
	//! The statement's parameters, once written.
	void TakeParams(std::vector<WrittenParam> &out) {
		for (size_t i = 0; i < params_->params.size(); i++) {
			WrittenParam param;
			param.name = params_->params[i].name;
			param.declaration = params_->params[i].declaration;
			param.value = (*param_values_)[i];
			out.push_back(std::move(param));
		}
	}
};

}  // namespace node_writer
}  // namespace mssql
}  // namespace duckdb
