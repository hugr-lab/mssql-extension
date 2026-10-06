//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// pushdown/mssql_sql_writer.hpp
//
// Spec 079: the T-SQL writer behind DuckDB's remote-pushdown rewriter. It
// renders a parsed query node -- catalog already stripped by the rewriter --
// into one statement and its parameters, or says why it cannot.
//
// There is ONE list of what pushes (spec 079 D1): `SupportsPushdown(QueryNode)`
// is this writer run into a scratch buffer, and `RemoteExecute` is the same
// run kept. A construct is supported iff the writer has an exact form for it.
//
// PR B took one base table: its columns (each through
// `MSSQLColumnInfo::BuildReadExpression`, the read expression the catalog scan
// uses), a WHERE of column-vs-constant comparisons, IS [NOT] NULL, AND / OR /
// NOT, ORDER BY on columns, and LIMIT / OFFSET as TOP / OFFSET-FETCH. PR C
// renders expressions through `ExpressionVocabulary`; PR D adds aggregates
// (COUNT / SUM / AVG / MIN / MAX / STDEV / VAR), GROUP BY on columns, HAVING,
// DISTINCT, and joins of one catalog's tables (ON / USING, each relation
// aliased [rN]; SEMI / ANTI as EXISTS / NOT EXISTS).
//===----------------------------------------------------------------------===//

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "duckdb/common/enums/order_type.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

class BaseTableRef;
class ClientContext;
class QueryNode;
struct MSSQLColumnInfo;

namespace mssql {

//! The table a BaseTableRef names, as the writer needs it.
struct WriterTable {
	std::string schema;
	std::string name;
	const std::vector<MSSQLColumnInfo> *columns = nullptr;
	//! The DuckDB types the catalog entry reports, parallel to `columns`;
	//! null = derive them from `columns` (unit tests).
	const std::vector<LogicalType> *types = nullptr;
	//! The row count the planner is given (statistics cache, else the one
	//! loaded with the table), and the columns of a unique key when it is
	//! loaded (empty: none known) -- the PR E1 gain check, no round trip.
	idx_t approx_rows = 0;
	//! False for a view: it has no row count of its own (sys.partitions has
	//! none), so its size is unknown, never small.
	bool size_known = true;
	std::vector<std::string> unique_key;
};

//! One `@pN` of the statement: its declaration (from the column it is
//! compared with, spec 076 / #361) and the DuckDB value it carries.
struct WrittenParam {
	std::string name;  // without the '@'
	std::string declaration;
	Value value;
};

struct WrittenQuery {
	std::string statement;
	std::vector<WrittenParam> params;
	//! Per result column, the type the catalog reports for the column it
	//! reads -- the vehicle's `column_types`, so a pushed SELECT has the
	//! catalog path's types, not the describe's.
	std::vector<LogicalType> column_types;
	//! Per result column, the DuckDB type the value is cast to after the read
	//! (INVALID: none) -- a type no wire type decodes into: SUM over integers
	//! is HUGEINT in DuckDB, decimal(38,0) on the server.
	std::vector<LogicalType> cast_types;
	//! Per result column, its name, as DuckDB names it.
	std::vector<std::string> column_names;
	//! A join whose gain is not certain: a link that equates no unique key of
	//! either side (many-to-many, CROSS), with no aggregate / DISTINCT / LIMIT
	//! over it -- it could send more rows than the scans (PR E1).
	bool gain_uncertain = false;
	//! The largest cached row count among the tables it joins (not the ones
	//! only EXISTS / NOT EXISTS reads, which multiply nothing).
	idx_t largest_input_rows = 0;
	//! One of them is a view, whose size is unknown.
	bool input_size_unknown = false;
	//! The cached row counts of every table the statement reads -- joined,
	//! EXISTS-read, inside a derived table or a subquery expression: what the
	//! scan path would read instead (mssql_pushdown_min_rows, PR E2).
	idx_t total_input_rows = 0;
	//! One of those is a view, whose size is unknown.
	bool total_size_unknown = false;
	//! A result column is a division or a floating-point aggregate: its
	//! divergence (NULL for inf, the last bits) is recorded for a value that
	//! reaches the user -- not for one DuckDB computes on, as a part pushed
	//! under a node that stays local would have it (review of E1).
	bool value_divergence = false;
	//! The writer stopped at a column that no relation of the node has but a
	//! node around it could: a correlated subquery asked about on its own
	//! (the rewriter asks about every nested node; PR E1).
	bool refers_outside = false;
	//! A LIMIT / OFFSET somewhere in the node: which rows it gives can differ
	//! between two evaluations (ties), where DuckDB evaluates a CTE once.
	bool picks_rows = false;

	//! "@p1 int, @p2 varchar(50)" -- empty without parameters.
	std::string Declarations() const;
};

struct SQLWriterOptions {
	//! mssql_convert_varchar_max, as the catalog scan reads it.
	bool convert_varchar_max = true;
	//! mssql_scan_parameterize_filters: constants as @pN, or literals.
	bool parameterize = true;
	//! DuckDB's (deprecated) error_on_division_by_zero: false makes `x % 0`
	//! NULL here where the server raises 8134, so `%` is then not pushed.
	bool division_by_zero_errors = true;
	//! DuckDB's ieee_floating_point_ops: true gives inf / NaN for `x / 0`,
	//! which the pushed `/ NULLIF(…, 0)` renders as NULL (the recorded
	//! divergence); false makes DuckDB raise, so `/` is then not pushed.
	bool ieee_floating_point_ops = true;
	//! DuckDB's scalar_subquery_error_on_multiple_rows: true raises on a
	//! scalar subquery of several rows, as SQL Server does (512); false returns
	//! an arbitrary row, and a scalar subquery is then not pushed.
	bool scalar_subquery_errors = true;
	//! The session's default_order / default_null_order, resolved.
	OrderType default_order = OrderType::ASCENDING;
	OrderByNullType default_null_order_asc = OrderByNullType::NULLS_LAST;
	OrderByNullType default_null_order_desc = OrderByNullType::NULLS_LAST;

	static SQLWriterOptions FromContext(ClientContext &context);
};

class SQLWriter {
public:
	//! Resolves a base table of the node; false when it is not this catalog's
	//! or is unknown -- a veto, never a guess (D1).
	using TableResolver = std::function<bool(const BaseTableRef &ref, WriterTable &out)>;

	SQLWriter(SQLWriterOptions options, TableResolver resolver);

	//! Whether the query wrote `ref` with no catalog and no schema: then a CTE
	//! in scope of that name is what it means (PR E1). The rewriter strips the
	//! catalog in place (`db.t` arrives as a bare `t`), so the catalog answers
	//! from the names it noted; unset, the name as it stands decides.
	using QualificationProbe = std::function<bool(const BaseTableRef &ref)>;
	void SetWrittenUnqualified(QualificationProbe probe) {
		written_unqualified_ = std::move(probe);
	}

	//! Render `node`. False, with the reason in `why`, when any part of it has
	//! no exact T-SQL form -- then `out` is unspecified.
	bool Write(const QueryNode &node, WrittenQuery &out, std::string &why);

	//! Whether pushing `node` sends the server more than the catalog scan
	//! would. The scan already pushes projections and filters, and unlike a
	//! pushed node it still takes filters from ABOVE: the rewriter pushes a
	//! set operation's children, an INSERT's and a CTAS's query on their own,
	//! and a pushed `SELECT * FROM t` there would read the whole table where
	//! the scan reads what an outer WHERE lets through. So a node the scan
	//! could serve as well is handed back to it by RemoteExecute (PR E1; the
	//! dry run answers renderability only, as the rewriter asks it about
	//! nested nodes too). The gain is ORDER BY, LIMIT /
	//! OFFSET, DISTINCT, GROUP BY / HAVING, an aggregate, and a join with no
	//! CROSS link: each sends fewer rows than the tables' (or their first N).
	//! `probe` tells a CTE reference from a table of that name (see
	//! SetWrittenUnqualified); unset, the name as it stands decides.
	static bool PushesMoreThanScan(const QueryNode &node, const QualificationProbe *probe = nullptr);

private:
	SQLWriterOptions options_;
	TableResolver resolver_;
	QualificationProbe written_unqualified_;
};

//! A type as `column_types` spells it: its ToString, except the MSSQL string
//! types, whose collation ToString leaves out (`MSSQL_VARCHAR(50,
//! 'SQL_Latin1_General_CP1_CI_AS')`, spec 060's cast syntax). '' for INVALID,
//! a computed column, whose type is the server's describe.
std::string ColumnTypeName(const LogicalType &type);

//! Count one statement the rewriter pushed (MSSQL_COUNTERS / MSSQL_DEBUG
//! print the running total): the pushed path is invisible in results, and a
//! suite that cannot see which path ran goes vacuous (spec 063's lesson).
void CountRemotePushdown();

}  // namespace mssql
}  // namespace duckdb
