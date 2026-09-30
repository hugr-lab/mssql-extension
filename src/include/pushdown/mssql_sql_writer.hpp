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
// PR B's vocabulary is one base table: its columns (each through
// `MSSQLColumnInfo::BuildReadExpression`, the read expression the catalog scan
// uses), a WHERE of column-vs-constant comparisons, IS [NOT] NULL, AND / OR /
// NOT, ORDER BY on columns, and LIMIT / OFFSET as TOP / OFFSET-FETCH.
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

	//! Render `node`. False, with the reason in `why`, when any part of it has
	//! no exact T-SQL form -- then `out` is unspecified.
	bool Write(const QueryNode &node, WrittenQuery &out, std::string &why);

	//! Whether pushing `node` sends the server more than the catalog scan
	//! would. The scan already pushes projections and filters, and unlike a
	//! pushed node it still takes filters from ABOVE: the rewriter pushes a
	//! set operation's children, an INSERT's and a CTAS's query on their own,
	//! and a pushed `SELECT * FROM t` there would read the whole table where
	//! the scan reads what an outer WHERE lets through. So a node the scan
	//! could serve as well stays with the scan; in PR B the gain is ORDER BY
	//! and LIMIT / OFFSET.
	static bool PushesMoreThanScan(const QueryNode &node);

private:
	SQLWriterOptions options_;
	TableResolver resolver_;
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
