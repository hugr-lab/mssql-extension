// test/cpp/test_sql_writer.cpp
// Unit tests for the spec 079 T-SQL writer (pushdown/mssql_sql_writer.hpp).
//
// The writer is the ONE list of what the remote-pushdown rewriter may push
// (D1): SupportsPushdown is its dry run, RemoteExecute its kept run. A result
// cannot tell a pushed statement from one DuckDB evaluated itself, so these
// tests pin the statement TEXT and the vetoes, without a server.

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "catalog/mssql_column_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "pushdown/mssql_sql_writer.hpp"

using namespace duckdb;
using namespace duckdb::mssql;

static int failures = 0;

static std::vector<MSSQLColumnInfo> Columns() {
	std::vector<MSSQLColumnInfo> columns;
	columns.emplace_back("id", 1, "int", 4, 10, 0, false, "", "SQL_Latin1_General_CP1_CI_AS");
	columns.emplace_back("name", 2, "nvarchar", 200, 0, 0, true, "SQL_Latin1_General_CP1_CI_AS",
						 "SQL_Latin1_General_CP1_CI_AS");
	columns.emplace_back("code", 3, "varchar", 20, 0, 0, true, "Latin1_General_100_BIN2_UTF8",
						 "SQL_Latin1_General_CP1_CI_AS");
	columns.emplace_back("amount", 4, "decimal", 9, 10, 2, true, "", "SQL_Latin1_General_CP1_CI_AS");
	columns.emplace_back("day", 5, "date", 3, 10, 0, true, "", "SQL_Latin1_General_CP1_CI_AS");
	columns.emplace_back("flag", 6, "bit", 1, 1, 0, false, "", "SQL_Latin1_General_CP1_CI_AS");
	columns.emplace_back("ts", 7, "datetime2", 8, 27, 7, true, "", "SQL_Latin1_General_CP1_CI_AS");
	columns.emplace_back("legacy", 8, "varchar", 30, 0, 0, true, "SQL_Latin1_General_CP1_CI_AS",
						 "SQL_Latin1_General_CP1_CI_AS");
	columns.emplace_back("wide", 9, "decimal", 17, 38, 10, true, "", "SQL_Latin1_General_CP1_CI_AS");
	columns.emplace_back("mid", 10, "decimal", 9, 19, 4, true, "", "SQL_Latin1_General_CP1_CI_AS");
	// tinyint is 0-255 on the server, which is DuckDB's UTINYINT -- the only
	// integer type whose rank is 1, so the only one a widening cast can target
	// without widening (roborev 1819 finding 1).
	columns.emplace_back("tiny", 11, "tinyint", 1, 3, 0, true, "", "SQL_Latin1_General_CP1_CI_AS");
	columns.emplace_back("ratio", 12, "float", 8, 53, 0, true, "", "SQL_Latin1_General_CP1_CI_AS");
	columns.emplace_back("doc", 13, "xml", -1, 0, 0, true, "", "SQL_Latin1_General_CP1_CI_AS");
	columns.emplace_back("small", 14, "decimal", 5, 5, 2, true, "", "SQL_Latin1_General_CP1_CI_AS");
	return columns;
}

//! A second table for joins: u(id int NOT NULL, t_id int NULL, label nvarchar(50) NULL, big bigint NULL).
static std::vector<MSSQLColumnInfo> OtherColumns() {
	std::vector<MSSQLColumnInfo> columns;
	columns.emplace_back("id", 1, "int", 4, 10, 0, false, "", "SQL_Latin1_General_CP1_CI_AS");
	columns.emplace_back("t_id", 2, "int", 4, 10, 0, true, "", "SQL_Latin1_General_CP1_CI_AS");
	columns.emplace_back("label", 3, "nvarchar", 100, 0, 0, true, "SQL_Latin1_General_CP1_CI_AS",
						 "SQL_Latin1_General_CP1_CI_AS");
	columns.emplace_back("big", 4, "bigint", 8, 19, 0, true, "", "SQL_Latin1_General_CP1_CI_AS");
	return columns;
}

static bool WriteWith(const SQLWriterOptions &options, const std::string &sql, WrittenQuery &out, std::string &why) {
	static const auto columns = Columns();
	static const auto other = OtherColumns();
	auto parser = Parser::GetBuiltinParser();
	parser.ParseQuery(sql);
	auto &node = *parser.statements[0]->Cast<SelectStatement>().node;
	SQLWriter writer(options, [](const BaseTableRef &ref, WriterTable &table) {
		const auto &name = ref.Table().GetIdentifierName();
		if (name != "t" && name != "u") {
			return false;
		}
		table.schema = "dbo";
		table.name = name;
		table.columns = name == "t" ? &columns : &other;
		table.approx_rows = name == "t" ? 5000000 : 10;
		table.unique_key = {"id"};	// both tables' primary key
		return true;
	});
	return writer.Write(node, out, why);
}

static bool Write(const std::string &sql, WrittenQuery &out, std::string &why, bool parameterize = true) {
	SQLWriterOptions options;
	options.parameterize = parameterize;
	return WriteWith(options, sql, out, why);
}

static void ExpectSql(const std::string &sql, const std::string &expected, bool parameterize = true) {
	WrittenQuery out;
	std::string why;
	if (!Write(sql, out, why, parameterize)) {
		std::cerr << "FAIL: " << sql << "\n  refused: " << why << "\n";
		failures++;
		return;
	}
	if (out.statement != expected) {
		std::cerr << "FAIL: " << sql << "\n  expected: " << expected << "\n  actual:   " << out.statement << "\n";
		failures++;
	}
}

static void ExpectParams(const std::string &sql, const std::string &declarations) {
	WrittenQuery out;
	std::string why;
	if (!Write(sql, out, why) || out.Declarations() != declarations) {
		std::cerr << "FAIL: " << sql << "\n  expected declarations: " << declarations
				  << "\n  actual: " << out.Declarations() << " (" << why << ")\n";
		failures++;
	}
}

static void ExpectVeto(const std::string &sql) {
	WrittenQuery out;
	std::string why;
	if (Write(sql, out, why)) {
		std::cerr << "FAIL: expected a veto for " << sql << "\n  wrote: " << out.statement << "\n";
		failures++;
	}
}

static void ExpectGain(const std::string &sql, bool expected) {
	auto parser = Parser::GetBuiltinParser();
	parser.ParseQuery(sql);
	auto &node = *parser.statements[0]->Cast<SelectStatement>().node;
	if (SQLWriter::PushesMoreThanScan(node) != expected) {
		std::cerr << "FAIL: PushesMoreThanScan(" << sql << ") != " << expected << "\n";
		failures++;
	}
}

int main() {
	// Projections go through BuildReadExpression: a non-UTF-8 varchar is read
	// through its NVARCHAR cast, as the catalog scan reads it.
	ExpectSql("SELECT id, name FROM t LIMIT 5", "SELECT TOP (5) [id], [name] FROM [dbo].[t]");
	ExpectSql("SELECT legacy FROM t LIMIT 1",
			  "SELECT TOP (1) CAST([legacy] AS NVARCHAR(30)) AS [legacy] FROM [dbo].[t]");
	ExpectSql("SELECT id AS x, t.name FROM t LIMIT 1", "SELECT TOP (1) [id] AS [x], [name] FROM [dbo].[t]");
	ExpectSql("SELECT id AS id FROM t LIMIT 1", "SELECT TOP (1) [id] FROM [dbo].[t]");
	ExpectSql("SELECT * FROM t LIMIT 1",
			  "SELECT TOP (1) [id], [name], [code], [amount], [day], [flag], [ts], CAST([legacy] AS NVARCHAR(30)) AS "
			  "[legacy], [wide], [mid], [tiny], [ratio], [doc], [small] FROM [dbo].[t]");
	ExpectSql("SELECT q.id FROM t AS q LIMIT 1", "SELECT TOP (1) [id] FROM [dbo].[t]");

	// WHERE: constants are parameters declared from the column.
	ExpectSql("SELECT id FROM t WHERE id = 5 AND (name = 'a' OR NOT flag = true) LIMIT 2",
			  "SELECT TOP (2) [id] FROM [dbo].[t] WHERE (([id] = @p0) AND (([name] = @p1) OR (NOT ([flag] = @p2))))");
	ExpectParams("SELECT id FROM t WHERE id = 5 AND amount >= 1.5 AND day < '2024-01-01' LIMIT 2",
				 "@p0 int, @p1 decimal(10,2), @p2 date");
	// A typed constant, as the rewriter's folding leaves `DATE '2024-01-01'`.
	ExpectParams("SELECT id FROM t WHERE day = CAST('2024-01-01' AS DATE) LIMIT 1", "@p0 date");
	ExpectVeto("SELECT id FROM t WHERE day = CAST('2024-01-01' AS TIMESTAMP) LIMIT 1");
	ExpectVeto("SELECT id FROM t WHERE id = TRY_CAST('5' AS INTEGER) LIMIT 1");
	ExpectSql("SELECT id FROM t WHERE 5 < id LIMIT 1", "SELECT TOP (1) [id] FROM [dbo].[t] WHERE (@p0 < [id])");
	ExpectSql("SELECT id FROM t WHERE name IS NULL AND code IS NOT NULL LIMIT 1",
			  "SELECT TOP (1) [id] FROM [dbo].[t] WHERE (([name] IS NULL) AND ([code] IS NOT NULL))");
	ExpectSql("SELECT id FROM t WHERE id = 5 LIMIT 1", "SELECT TOP (1) [id] FROM [dbo].[t] WHERE ([id] = 5)", false);
	ExpectSql("SELECT id FROM t WHERE amount = 2 LIMIT 1", "SELECT TOP (1) [id] FROM [dbo].[t] WHERE ([amount] = 2.00)",
			  false);

	// A constant DuckDB and the server would compare differently stays local.
	ExpectVeto("SELECT id FROM t WHERE id = 2.5 LIMIT 1");			 // not exact in int
	ExpectVeto("SELECT id FROM t WHERE amount = 1.005 LIMIT 1");	 // rounds in decimal(10,2)
	ExpectVeto("SELECT id FROM t WHERE id = 99999999999 LIMIT 1");	 // does not fit int
	ExpectVeto("SELECT id FROM t WHERE name = 5 LIMIT 1");			 // number against a string
	ExpectVeto("SELECT id FROM t WHERE ts = '2024-01-01' LIMIT 1");	 // datetime2: PR C
	ExpectVeto("SELECT id FROM t WHERE id = NULL LIMIT 1");
	ExpectVeto("SELECT id FROM t WHERE id = code LIMIT 1");			 // column against column: PR C
	ExpectVeto("SELECT id FROM t WHERE lower(name) = 'a' LIMIT 1");	 // functions: PR C

	// ORDER BY: DuckDB's order only (D4), NULL placement emulated under a LIMIT.
	ExpectSql("SELECT id FROM t ORDER BY id DESC LIMIT 3", "SELECT TOP (3) [id] FROM [dbo].[t] ORDER BY [t].[id] DESC");
	ExpectSql("SELECT id FROM t ORDER BY amount LIMIT 3",
			  "SELECT TOP (3) [id] FROM [dbo].[t] ORDER BY CASE WHEN [t].[amount] IS NULL THEN 1 ELSE 0 END, "
			  "[t].[amount] ASC");
	ExpectSql("SELECT id FROM t ORDER BY amount DESC", "SELECT [id] FROM [dbo].[t] ORDER BY [t].[amount] DESC");
	ExpectVeto("SELECT id FROM t ORDER BY amount");	 // placement differs, no LIMIT to justify the CASE key
	ExpectSql("SELECT id FROM t ORDER BY code LIMIT 3",
			  "SELECT TOP (3) [id] FROM [dbo].[t] ORDER BY CASE WHEN [t].[code] IS NULL THEN 1 ELSE 0 END, "
			  "CAST([t].[code] AS "
			  "varbinary(20)) ASC");
	ExpectVeto("SELECT id FROM t ORDER BY name LIMIT 3");  // nvarchar never orders like DuckDB
	ExpectVeto("SELECT id FROM t ORDER BY ts LIMIT 3");	   // datetime2(7) is off the allow-list
	ExpectSql("SELECT id AS x, name FROM t ORDER BY x DESC LIMIT 3",
			  "SELECT TOP (3) [id] AS [x], [name] FROM [dbo].[t] ORDER BY [t].[id] DESC");
	ExpectSql("SELECT name, id FROM t ORDER BY 2 LIMIT 3",
			  "SELECT TOP (3) [name], [id] FROM [dbo].[t] ORDER BY [t].[id] ASC");
	// A bare ORDER BY name is a select-list alias first in T-SQL: the key is
	// qualified, so an alias named like another column cannot capture it.
	ExpectSql("SELECT id AS amount FROM t ORDER BY t.amount DESC LIMIT 3",
			  "SELECT TOP (3) [id] AS [amount] FROM [dbo].[t] ORDER BY [t].[amount] DESC");
	ExpectSql(
		"SELECT day AS id, id AS day FROM t ORDER BY 1 LIMIT 2",
		"SELECT TOP (2) [day] AS [id], [id] AS [day] FROM [dbo].[t] ORDER BY CASE WHEN [t].[day] IS NULL THEN 1 ELSE "
		"0 END, [t].[day] ASC");
	ExpectVeto("SELECT id FROM t ORDER BY 3 LIMIT 3");
	ExpectVeto("SELECT id FROM t ORDER BY id + 1 LIMIT 3");

	// LIMIT / OFFSET.
	ExpectSql("SELECT id FROM t ORDER BY id LIMIT 3 OFFSET 2",
			  "SELECT [id] FROM [dbo].[t] ORDER BY [t].[id] ASC OFFSET 2 ROWS FETCH NEXT 3 ROWS ONLY");
	ExpectSql("SELECT id FROM t OFFSET 2", "SELECT [id] FROM [dbo].[t] ORDER BY (SELECT NULL) OFFSET 2 ROWS");
	ExpectVeto("SELECT id FROM t LIMIT 10%");

	// Arithmetic: same-type operands keep their type on both sides; a constant
	// is typed from its peer, as the binder types it.
	ExpectSql("SELECT id + 1 FROM t LIMIT 1", "SELECT TOP (1) ([id] + @p0) AS [(id + 1)] FROM [dbo].[t]");
	ExpectParams("SELECT id + 1 FROM t LIMIT 1", "@p0 int");
	ExpectSql("SELECT id * 2 AS x FROM t LIMIT 1", "SELECT TOP (1) ([id] * @p0) AS [x] FROM [dbo].[t]");
	ExpectSql("SELECT -id AS n FROM t LIMIT 1", "SELECT TOP (1) (-[id]) AS [n] FROM [dbo].[t]");
	ExpectVeto("SELECT id FROM t WHERE id + 1 > 5 LIMIT 1");  // DuckDB moves the constant (full review)
	ExpectSql("SELECT id FROM t WHERE id % 2 = 0 LIMIT 1",
			  "SELECT TOP (1) [id] FROM [dbo].[t] WHERE (([id] % @p0) = @p1)");
	ExpectSql("SELECT amount + amount AS d FROM t LIMIT 1",
			  "SELECT TOP (1) ([amount] + [amount]) AS [d] FROM [dbo].[t]");
	ExpectSql(
		"SELECT id FROM t WHERE amount + 1 > 2 LIMIT 1",
		"SELECT TOP (1) [id] FROM [dbo].[t] WHERE (([amount] + @p0) > @p1)");  // a decimal: DuckDB computes it too
	ExpectSql("SELECT id + 1 FROM t LIMIT 1", "SELECT TOP (1) ([id] + CAST(1 AS int)) AS [(id + 1)] FROM [dbo].[t]",
			  false);
	ExpectVeto("SELECT wide + wide FROM t LIMIT 1");				   // width 38: the server would round
	ExpectVeto("SELECT mid * mid FROM t LIMIT 1");					   // p1 + p2 + 1 > 38: the server reduces the scale
	ExpectVeto("SELECT amount * amount AS p FROM t LIMIT 1");		   // DuckDB's DECIMAL(18) product overflows first
	ExpectVeto("SELECT id + amount FROM t LIMIT 1");				   // int + decimal: the promotions differ
	ExpectVeto("SELECT id + 2.5 FROM t LIMIT 1");					   // not exact in int
	ExpectVeto("SELECT name + 1 FROM t LIMIT 1");					   // not a number
	ExpectVeto("SELECT amount % 2 FROM t LIMIT 1");					   // T-SQL % takes integers only (8117)
	ExpectVeto("SELECT id FROM t WHERE amount * amount > 1 LIMIT 1");  // a decimal product's type differs
	ExpectVeto("SELECT -code FROM t LIMIT 1");
	ExpectVeto("SELECT id + 1 AS x FROM t ORDER BY x LIMIT 1");	 // ORDER BY a computed column
	ExpectVeto("SELECT id + 1 FROM t ORDER BY 1 LIMIT 1");
	ExpectVeto("SELECT abs(id) FROM t LIMIT 1");  // functions: a later step

	// Division: DuckDB's `/` is floating (5 / 2 = 2.5); a zero divisor gives
	// NULL on the server where DuckDB says inf -- the recorded divergence.
	ExpectSql("SELECT id / 2 AS h FROM t LIMIT 1",
			  "SELECT TOP (1) (CAST([id] AS float) / NULLIF(CAST(@p0 AS float), 0)) AS [h] FROM [dbo].[t]");
	ExpectSql(
		"SELECT id / 2 AS h FROM t LIMIT 1",
		"SELECT TOP (1) (CAST([id] AS float) / NULLIF(CAST(CAST(2.0 AS float) AS float), 0)) AS [h] FROM [dbo].[t]",
		false);
	ExpectParams("SELECT id / 2.5 AS h FROM t LIMIT 1", "@p0 float");
	ExpectVeto("SELECT id FROM t WHERE amount / id > 0.5 LIMIT 1");	 // a zero divisor would change the rows
	ExpectVeto("SELECT CASE WHEN id / 2 > 1 THEN id END FROM t LIMIT 1");
	ExpectVeto("SELECT name / 2 FROM t LIMIT 1");

	// Widening casts only.
	ExpectSql("SELECT CAST(id AS BIGINT) AS b FROM t LIMIT 1",
			  "SELECT TOP (1) CAST([id] AS bigint) AS [b] FROM [dbo].[t]");
	ExpectSql("SELECT CAST(amount AS DOUBLE) AS d FROM t LIMIT 1",
			  "SELECT TOP (1) CAST([amount] AS float) AS [d] FROM [dbo].[t]");
	ExpectVeto("SELECT CAST(id AS SMALLINT) FROM t LIMIT 1");  // narrowing can fail differently
	ExpectVeto("SELECT CAST(id AS VARCHAR) FROM t LIMIT 1");   // formatting is DuckDB's
	ExpectVeto("SELECT TRY_CAST(id AS BIGINT) FROM t LIMIT 1");
	// A UTINYINT target is rank 1, so it passes the widening gate over a
	// tinyint column and must render as `tinyint`. It used to fall through the
	// arm list to `bigint`, and then the server computed in bigint what DuckDB
	// overflows in UINT8 (roborev 1819 finding 1).
	ExpectSql("SELECT CAST(tiny AS UTINYINT) AS u FROM t LIMIT 1",
			  "SELECT TOP (1) CAST([tiny] AS tinyint) AS [u] FROM [dbo].[t]");
	ExpectSql("SELECT CAST(tiny AS INTEGER) AS w FROM t LIMIT 1",
			  "SELECT TOP (1) CAST([tiny] AS int) AS [w] FROM [dbo].[t]");
	ExpectVeto("SELECT CAST(id AS UTINYINT) FROM t LIMIT 1");  // narrowing

	// The overflow agreement for a narrow integer peer rests on the constant
	// carrying the COLUMN's type: `tinyint + <tinyint>` is tinyint on the server
	// and overflows at 255 + 1, exactly as DuckDB overflows UINT8. A parameter
	// takes it from DeclarationForColumn; an UNPARAMETERISED literal takes it
	// from the CAST that BindConstant wraps it in -- without which T-SQL would
	// read `1` as an int, widen the sum to 256 and return a row where DuckDB
	// raises. Only the parameterised form was pinned; roborev 1821 finding 5
	// read the literal as bare, which it is not, so both forms are pinned now.
	ExpectSql("SELECT tiny + 1 AS s FROM t LIMIT 1", "SELECT TOP (1) ([tiny] + @p0) AS [s] FROM [dbo].[t]");
	ExpectSql("SELECT tiny + 1 AS s FROM t LIMIT 1",
			  "SELECT TOP (1) ([tiny] + CAST(1 AS tinyint)) AS [s] FROM [dbo].[t]", /*parameterize=*/false);
	ExpectSql("SELECT id + 1 AS s FROM t LIMIT 1", "SELECT TOP (1) ([id] + CAST(1 AS int)) AS [s] FROM [dbo].[t]",
			  /*parameterize=*/false);

	// CASE, COALESCE, NULLIF: one type for every branch.
	ExpectSql("SELECT CASE WHEN id > 1 THEN id ELSE 0 END AS c FROM t LIMIT 1",
			  "SELECT TOP (1) CASE WHEN ([id] > @p0) THEN [id] ELSE @p1 END AS [c] FROM [dbo].[t]");
	ExpectVeto("SELECT coalesce(amount, 0) AS c FROM t LIMIT 1");  // DECIMAL(12,2) in DuckDB
	ExpectSql("SELECT nullif(id, 0) AS c FROM t LIMIT 1", "SELECT TOP (1) NULLIF([id], @p0) AS [c] FROM [dbo].[t]");
	ExpectSql("SELECT CASE WHEN flag THEN id END AS c FROM t LIMIT 1",
			  "SELECT TOP (1) CASE WHEN ([flag] = 1) THEN [id] ELSE NULL END AS [c] FROM [dbo].[t]");
	ExpectSql("SELECT id FROM t WHERE flag LIMIT 1", "SELECT TOP (1) [id] FROM [dbo].[t] WHERE ([flag] = 1)");
	ExpectVeto("SELECT CASE WHEN flag THEN 1 END FROM t LIMIT 1");	// constants only: no type to give them
	ExpectVeto("SELECT id FROM t WHERE id LIMIT 1");				// an int is not a condition
	ExpectVeto("SELECT CASE WHEN id > 1 THEN id ELSE amount END FROM t LIMIT 1");  // int and decimal
	ExpectVeto("SELECT coalesce(name, code) FROM t LIMIT 1");					   // two collations
	ExpectVeto("SELECT coalesce(legacy, 'x') FROM t LIMIT 1");	// a code-page varchar arrives untranscoded
	ExpectVeto("SELECT CASE WHEN id > 1 THEN id ELSE 2.0 END FROM t LIMIT 1");	// 2.0 widens DuckDB's type
	ExpectVeto("SELECT id + 2.0 FROM t LIMIT 1");
	ExpectSql("SELECT id FROM t WHERE id = 2.0 LIMIT 1", "SELECT TOP (1) [id] FROM [dbo].[t] WHERE ([id] = @p0)");
	ExpectSql("SELECT coalesce(name, 'none') AS c FROM t LIMIT 1",
			  "SELECT TOP (1) COALESCE([name], @p0) AS [c] FROM [dbo].[t]");
	ExpectSql("SELECT coalesce(name, 'none') AS c FROM t LIMIT 1",
			  "SELECT TOP (1) COALESCE([name], CAST(N'none' AS nvarchar(100))) AS [c] FROM [dbo].[t]", false);
	ExpectVeto("SELECT coalesce(code, 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa') FROM t LIMIT 1");  // wider than varchar(20)
	ExpectVeto("SELECT coalesce(id / 2, 0) FROM t LIMIT 1");  // COALESCE would hide the zero-divisor NULL
	ExpectSql("SELECT CASE WHEN id > 1 THEN id / 2 END AS h FROM t LIMIT 1",
			  "SELECT TOP (1) CASE WHEN ([id] > @p0) THEN (CAST([id] AS float) / NULLIF(CAST(@p1 AS float), 0)) ELSE "
			  "NULL END AS "
			  "[h] FROM [dbo].[t]");

	// IN / NOT IN, BETWEEN, LIKE / NOT LIKE, value against value.
	ExpectSql("SELECT id FROM t WHERE id IN (1, 2) LIMIT 1",
			  "SELECT TOP (1) [id] FROM [dbo].[t] WHERE ([id] IN (@p0, @p1))");
	ExpectParams("SELECT id FROM t WHERE id IN (1, 2) LIMIT 1", "@p0 int, @p1 int");
	ExpectSql("SELECT id FROM t WHERE id NOT IN (1, NULL) LIMIT 1",
			  "SELECT TOP (1) [id] FROM [dbo].[t] WHERE (NOT ([id] IN (@p0, NULL)))");
	ExpectSql("SELECT id FROM t WHERE amount BETWEEN 1 AND 2 LIMIT 1",
			  "SELECT TOP (1) [id] FROM [dbo].[t] WHERE ([amount] BETWEEN @p0 AND @p1)");
	ExpectSql("SELECT id FROM t WHERE name LIKE 'a[b%' LIMIT 1",
			  "SELECT TOP (1) [id] FROM [dbo].[t] WHERE ([name] LIKE N'a[[]b%')", false);
	ExpectSql("SELECT id FROM t WHERE name NOT LIKE 'x%' LIMIT 1",
			  "SELECT TOP (1) [id] FROM [dbo].[t] WHERE (NOT ([name] LIKE @p0))");
	ExpectSql("SELECT id FROM t WHERE amount > amount LIMIT 1",
			  "SELECT TOP (1) [id] FROM [dbo].[t] WHERE ([amount] > [amount])");
	ExpectSql("SELECT id FROM t WHERE name = legacy LIMIT 1",
			  "SELECT TOP (1) [id] FROM [dbo].[t] WHERE ([name] = [legacy])");
	ExpectSql("SELECT id FROM t WHERE id + 1 IS NULL LIMIT 1",
			  "SELECT TOP (1) [id] FROM [dbo].[t] WHERE (([id] + @p0) IS NULL)");
	ExpectVeto("SELECT id FROM t WHERE name = code LIMIT 1");  // two collations: 468
	ExpectVeto("SELECT id FROM t WHERE id = amount LIMIT 1");  // two types
	ExpectVeto("SELECT id FROM t WHERE name ILIKE 'x%' LIMIT 1");
	ExpectVeto("SELECT id FROM t WHERE name GLOB 'x*' LIMIT 1");
	ExpectVeto("SELECT id FROM t WHERE name LIKE 'x!%' ESCAPE '!' LIMIT 1");
	ExpectVeto("SELECT id FROM t WHERE id LIKE '1%' LIMIT 1");		// LIKE over an int
	ExpectVeto("SELECT id FROM t WHERE name LIKE legacy LIMIT 1");	// pattern not a constant

	// Everything outside the writer's vocabulary is a veto, never a guess.
	ExpectVeto("SELECT rowid FROM t LIMIT 1");
	ExpectVeto("SELECT * EXCLUDE (id) FROM t LIMIT 1");
	ExpectVeto("SELECT id, id FROM t LIMIT 1");
	ExpectVeto("SELECT id FROM w LIMIT 1");
	ExpectSql("SELECT t.id FROM t, t AS u LIMIT 1",
			  "SELECT TOP (1) [r1].[id] AS [id] FROM [dbo].[t] AS [r1] CROSS JOIN [dbo].[t] AS [r2]");
	// An unused CTE is left out: DuckDB does not bind one either.
	ExpectSql("WITH c AS (SELECT 1) SELECT id FROM t LIMIT 1", "SELECT TOP (1) [id] FROM [dbo].[t]");
	ExpectVeto("SELECT other.id FROM t LIMIT 1");

	// Aggregates (PR D): COUNT as COUNT_BIG, an integer SUM as decimal(38,0)
	// (cast back to HUGEINT after the read), AVG as an exact sum divided.
	ExpectSql("SELECT count(*) FROM t", "SELECT COUNT_BIG(*) AS [count_star()] FROM [dbo].[t]");
	ExpectSql("SELECT count(1) AS n FROM t", "SELECT COUNT_BIG(*) AS [n] FROM [dbo].[t]");
	ExpectSql("SELECT count(DISTINCT name) AS n FROM t", "SELECT COUNT_BIG(DISTINCT [name]) AS [n] FROM [dbo].[t]");
	ExpectSql("SELECT sum(id) AS s, sum(amount) AS a FROM t",
			  "SELECT SUM(CAST([id] AS decimal(38,0))) AS [s], SUM([amount]) AS [a] FROM [dbo].[t]");
	ExpectSql("SELECT avg(id) AS a FROM t",
			  "SELECT CAST(SUM(CAST([id] AS decimal(38,0))) AS float) / COUNT_BIG([id]) AS [a] FROM [dbo].[t]");
	ExpectSql("SELECT avg(ratio) AS a, stddev(ratio) AS s, var_pop(id) AS v FROM t",
			  "SELECT AVG([ratio]) AS [a], STDEV([ratio]) AS [s], VARP([id]) AS [v] FROM [dbo].[t]");
	ExpectSql("SELECT min(flag) AS f, max(day) AS d FROM t",
			  "SELECT CAST(MIN(CAST([flag] AS tinyint)) AS bit) AS [f], MAX([day]) AS [d] FROM [dbo].[t]");
	{
		WrittenQuery out;
		std::string why;
		if (!Write("SELECT sum(id) AS s, count(*) AS c, max(day) AS d FROM t", out, why) ||
			out.cast_types[0] != LogicalType::HUGEINT || out.column_types[0].id() != LogicalTypeId::INVALID ||
			out.column_types[1] != LogicalType::BIGINT || out.cast_types[1].id() != LogicalTypeId::INVALID ||
			out.column_types[2] != LogicalType::DATE || out.column_names[0] != "s") {
			std::cerr << "FAIL: aggregate result types (" << why << ")\n";
			failures++;
		}
	}
	{
		// COALESCE / CASE over an integer SUM stays HUGEINT: cast back as is.
		WrittenQuery out;
		std::string why;
		if (!Write("SELECT coalesce(sum(id), 0) AS s FROM t", out, why) || out.cast_types[0] != LogicalType::HUGEINT) {
			std::cerr << "FAIL: coalesce(sum(id), 0) is not cast back to HUGEINT (" << why << ")\n";
			failures++;
		}
	}
	ExpectVeto("SELECT coalesce(sum(id), sum(amount)) FROM t");
	ExpectVeto("SELECT -sum(id) FROM t");
	ExpectVeto("SELECT min(name) FROM t");	// strings order by collation (#362)
	ExpectVeto("SELECT max(ts) FROM t");	// datetime2(7) is off the order list
	ExpectVeto("SELECT sum(flag) FROM t");	// bit
	ExpectVeto("SELECT sum(name) FROM t");
	// FILTER as CASE (PR E2): a row the condition does not take is a NULL the
	// aggregate skips; COUNT(*) counts the CASE's 1s.
	ExpectSql("SELECT count(*) FILTER (WHERE id > 1) AS c, sum(id) FILTER (WHERE flag) AS s FROM t",

			  "SELECT COUNT_BIG(CASE WHEN ([id] > 1) THEN 1 END) AS [c], SUM(CAST(CASE WHEN ([flag] = 1) THEN [id] END "
			  "AS decimal(38,0))) AS [s] FROM [dbo].[t]",
			  false);
	ExpectSql(
		"SELECT count(DISTINCT day) FILTER (WHERE amount IS NOT NULL) AS c, max(amount) FILTER (WHERE id < 3) AS m "
		"FROM t",

		"SELECT COUNT_BIG(DISTINCT CASE WHEN ([amount] IS NOT NULL) THEN [day] END) AS [c], MAX(CASE WHEN ([id] < 3) "
		"THEN [amount] END) AS [m] FROM [dbo].[t]",
		false);
	ExpectVeto("SELECT count(*) FILTER (WHERE count(*) > 1) FROM t");  // an aggregate in the filter
	ExpectSql(
		"SELECT avg(id) FILTER (WHERE flag) AS a, max(flag) FILTER (WHERE id > 1) AS m, count(1) FILTER (WHERE id > 2) "
		"AS c FROM t",

		"SELECT CAST(SUM(CAST(CASE WHEN ([flag] = 1) THEN [id] END AS decimal(38,0))) AS float) / COUNT_BIG(CASE WHEN "
		"([flag] = 1) THEN [id] END) AS [a], CAST(MAX(CAST(CASE WHEN ([id] > 1) THEN [flag] END AS tinyint)) AS bit) "
		"AS [m], COUNT_BIG(CASE WHEN ([id] > 2) THEN 1 END) AS [c] FROM [dbo].[t]",
		false);
	ExpectVeto("SELECT count(*) FILTER (WHERE EXISTS (SELECT 1 FROM u)) FROM t");					   // 130 there
	ExpectVeto("SELECT count(*) FILTER (WHERE id IN (SELECT t_id FROM u)) FROM t");					   // 130 there
	ExpectVeto("SELECT id FROM t WHERE id IN (SELECT count(*) FILTER (WHERE u.t_id = t.id) FROM u)");  // 8124
	ExpectSql(
		"SELECT string_agg(name ORDER BY id, id DESC) AS a, string_agg(legacy ORDER BY id) AS b FROM t",
		"SELECT STRING_AGG(CAST([name] AS nvarchar(max)), N',') WITHIN GROUP (ORDER BY [id] ASC) AS [a], "
		"STRING_AGG(CAST([legacy] AS nvarchar(max)), N',') WITHIN GROUP (ORDER BY [id] ASC) AS [b] FROM [dbo].[t]",
		false);	 // a repeated key once; one order shared by both
	ExpectVeto("SELECT string_agg(name ORDER BY count(*)) FROM t");
	// Windows (PR E2).
	ExpectSql(
		"SELECT id, row_number() OVER (PARTITION BY flag ORDER BY day DESC) AS r, ntile(4) OVER (ORDER BY id) AS q "
		"FROM t",

		"SELECT [id], ROW_NUMBER() OVER (PARTITION BY [flag] ORDER BY [day] DESC) AS [r], NTILE(4) OVER (ORDER BY [id] "
		"ASC) AS [q] FROM [dbo].[t]",
		false);
	ExpectSql("SELECT id, sum(id) OVER (ORDER BY id ROWS BETWEEN 2 PRECEDING AND CURRENT ROW) AS s FROM t",
			  "SELECT [id], SUM(CAST([id] AS decimal(38,0))) OVER (ORDER BY [id] ASC ROWS BETWEEN 2 PRECEDING AND "
			  "CURRENT ROW) AS [s] FROM [dbo].[t]",
			  false);
	ExpectSql(
		"SELECT id, lag(amount, 2) OVER (ORDER BY id) AS l, last_value(name) OVER (ORDER BY id ROWS BETWEEN UNBOUNDED "
		"PRECEDING AND UNBOUNDED FOLLOWING) AS v FROM t",

		"SELECT [id], LAG([amount], 2) OVER (ORDER BY [id] ASC) AS [l], LAST_VALUE([name]) OVER (ORDER BY [id] ASC "
		"RANGE BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS [v] FROM [dbo].[t]",
		false);
	ExpectSql(
		"SELECT id, count(*) FILTER (WHERE flag) OVER (PARTITION BY day) AS c FROM t",
		"SELECT [id], COUNT_BIG(CASE WHEN ([flag] = 1) THEN 1 END) OVER (PARTITION BY [day]) AS [c] FROM [dbo].[t]",
		false);
	ExpectSql("SELECT id, rank() OVER (ORDER BY code) AS r FROM t",
			  "SELECT [id], RANK() OVER (ORDER BY CASE WHEN [code] IS NULL THEN 1 ELSE 0 END, CAST([code] AS "
			  "varbinary(20)) ASC) AS [r] FROM [dbo].[t]",
			  false);  // bytes, NULL placement
	ExpectSql("SELECT day, max(amount) AS m, rank() OVER (ORDER BY max(amount) DESC) AS r FROM t GROUP BY day",
			  "SELECT [day], MAX([amount]) AS [m], RANK() OVER (ORDER BY MAX([amount]) DESC) AS [r] FROM [dbo].[t] "
			  "GROUP BY [day]",
			  false);  // an aggregate key: nullable, emulated
	ExpectVeto("SELECT row_number() OVER () FROM t");
	ExpectVeto("SELECT rank() OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) FROM t");	 // 10752
	ExpectVeto("SELECT sum(id) OVER (ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) FROM t");			 // no ORDER BY
	ExpectVeto("SELECT sum(id) OVER (ORDER BY id ROWS BETWEEN CURRENT ROW AND 1 PRECEDING) FROM t");
	ExpectVeto("SELECT sum(id) OVER (ORDER BY id RANGE BETWEEN 1 PRECEDING AND CURRENT ROW) FROM t");
	ExpectVeto("SELECT count(DISTINCT id) OVER () FROM t");
	ExpectVeto("SELECT lag(id IGNORE NULLS) OVER (ORDER BY id) FROM t");
	ExpectVeto("SELECT lag(legacy) OVER (ORDER BY id) FROM t");		  // a code-page varchar value
	ExpectVeto("SELECT row_number() OVER (ORDER BY legacy) FROM t");  // its order is the collation's
	ExpectSql("SELECT row_number() OVER (PARTITION BY ratio ORDER BY id) AS r FROM t",
			  "SELECT ROW_NUMBER() OVER (PARTITION BY [ratio] ORDER BY [id] ASC) AS [r] FROM [dbo].[t]",
			  false);															   // groupable as GROUP BY's keys are
	ExpectVeto("SELECT row_number() OVER (PARTITION BY doc ORDER BY id) FROM t");  // xml
	ExpectVeto("SELECT row_number() OVER (PARTITION BY id % 2 ORDER BY id) FROM t");  // an expression
	ExpectVeto("SELECT nth_value(id, 2) OVER (ORDER BY id) FROM t");
	ExpectVeto("SELECT id FROM t WHERE row_number() OVER (ORDER BY id) = 1");
	// A window over a division keeps its NULL-at-zero (review of E2).
	ExpectVeto("SELECT coalesce(lag(id / 2) OVER (ORDER BY id), 0) FROM t");
	ExpectVeto("SELECT * FROM (SELECT id, lag(id / 2) OVER (ORDER BY id) AS l FROM t) d WHERE l IS NULL");
	ExpectVeto("SELECT sum(id) OVER (ORDER BY id ROWS BETWEEN 3000000000 PRECEDING AND CURRENT ROW) FROM t");  // 102
	// A tie-dependent window picks rows: a CTE holding one is not inlined twice.
	ExpectVeto(
		"WITH r AS (SELECT id, row_number() OVER (ORDER BY day) AS rn FROM t) SELECT a.id FROM r a JOIN r b ON a.id = "
		"b.id");
	ExpectSql(
		"WITH r AS (SELECT id, rank() OVER (ORDER BY day) AS rk FROM t) SELECT a.id FROM r a JOIN r b ON a.id = b.id",

		"SELECT [r1].[id] AS [id] FROM (SELECT [id], RANK() OVER (ORDER BY CASE WHEN [day] IS NULL THEN 1 ELSE 0 END, "
		"[day] ASC) AS [rk] FROM [dbo].[t]) AS [r1] INNER JOIN (SELECT [id], RANK() OVER (ORDER BY CASE WHEN [day] IS "
		"NULL THEN 1 ELSE 0 END, [day] ASC) AS [rk] FROM [dbo].[t]) AS [r2] ON ([r1].[id] = [r2].[id])",
		false);	 // a peer's answer is the same whatever the order of ties
	ExpectVeto("SELECT count(*) FILTER (WHERE id / 2 > 1) FROM t");	 // a division in a condition
	// string_agg (PR E2): over nvarchar(max), one WITHIN GROUP order per node.
	ExpectSql("SELECT string_agg(name, ';') AS a FROM t",
			  "SELECT STRING_AGG(CAST([name] AS nvarchar(max)), N';') AS [a] FROM [dbo].[t]", false);
	ExpectSql("SELECT string_agg(legacy ORDER BY id DESC) AS a, string_agg(name) AS b FROM t",
			  "SELECT STRING_AGG(CAST([legacy] AS nvarchar(max)), N',') WITHIN GROUP (ORDER BY [id] DESC) AS [a], "
			  "STRING_AGG(CAST([name] AS nvarchar(max)), N',') AS [b] FROM [dbo].[t]",
			  false);
	ExpectSql(
		"SELECT string_agg(name, ',' ORDER BY code) FILTER (WHERE id > 1) AS a FROM t",
		"SELECT STRING_AGG(CASE WHEN ([id] > 1) THEN CAST([name] AS nvarchar(max)) END, N',') WITHIN GROUP (ORDER BY "
		"CASE WHEN [code] IS NULL THEN 1 ELSE 0 END, CAST([code] AS varbinary(20)) ASC) AS [a] FROM [dbo].[t]",
		false);
	ExpectSql("SELECT string_agg(name ORDER BY day) AS a FROM t",
			  "SELECT STRING_AGG(CAST([name] AS nvarchar(max)), N',') WITHIN GROUP (ORDER BY CASE WHEN [day] IS NULL "
			  "THEN 1 ELSE 0 END, [day] ASC) AS [a] FROM [dbo].[t]",
			  false);  // NULL placement emulated
	ExpectVeto("SELECT string_agg(DISTINCT name) FROM t");
	ExpectVeto("SELECT string_agg(name ORDER BY id), string_agg(legacy ORDER BY day) FROM t");	// 8711
	ExpectVeto("SELECT string_agg(name ORDER BY legacy) FROM t");  // a code-page varchar's order
	ExpectVeto("SELECT string_agg(name, code) FROM t");			   // a separator that is not a constant
	ExpectVeto("SELECT string_agg(doc) FROM t");
	ExpectVeto("SELECT string_agg(name) FROM t HAVING string_agg(name) = 'a'");	 // no comparison (D4)
	ExpectVeto("SELECT sum(id / 2) FROM t");									 // inf here, a skipped NULL there
	ExpectVeto("SELECT sum(sum(id)) FROM t");
	ExpectVeto("SELECT id FROM t WHERE count(*) > 1");	// an aggregate in WHERE
	ExpectVeto("SELECT id, count(*) FROM t");			// DuckDB's binder error, not the server's
	ExpectVeto("SELECT * FROM t GROUP BY id");

	// GROUP BY columns -- by name, position or select alias -- and HAVING.
	ExpectSql("SELECT id, count(*) AS c FROM t GROUP BY id",
			  "SELECT [id], COUNT_BIG(*) AS [c] FROM [dbo].[t] GROUP BY [id]");
	ExpectSql(
		"SELECT legacy, count(*) AS c FROM t GROUP BY 1",
		"SELECT CAST([legacy] AS NVARCHAR(30)) AS [legacy], COUNT_BIG(*) AS [c] FROM [dbo].[t] GROUP BY [legacy]");
	ExpectSql("SELECT day AS d, sum(amount) AS s FROM t WHERE id > 0 GROUP BY d HAVING count(*) > 1",
			  "SELECT [day] AS [d], SUM([amount]) AS [s] FROM [dbo].[t] WHERE ([id] > @p0) GROUP BY [day] HAVING "
			  "(COUNT_BIG(*) > @p1)");
	ExpectParams("SELECT id FROM t GROUP BY id HAVING sum(amount) > 10 AND max(day) < '2024-01-01'",
				 "@p0 decimal(38,2), @p1 date");
	ExpectSql("SELECT count(*) AS c FROM t GROUP BY ()", "SELECT COUNT_BIG(*) AS [c] FROM [dbo].[t]");
	ExpectVeto("SELECT id, name, count(*) FROM t GROUP BY id");	 // name neither grouped nor aggregated
	ExpectVeto("SELECT id FROM t GROUP BY id HAVING name = 'a'");
	ExpectVeto("SELECT id, count(*) FROM t GROUP BY ROLLUP (id)");
	ExpectVeto("SELECT id, count(*) FROM t GROUP BY ALL");
	ExpectVeto("SELECT id + 1, count(*) FROM t GROUP BY id + 1");	   // expression keys: not yet
	ExpectVeto("SELECT ts, count(*) FROM t GROUP BY ts");			   // datetime2(7)
	ExpectVeto("SELECT doc, count(*) FROM t GROUP BY doc");			   // xml: 249 there
	ExpectVeto("SELECT id FROM t GROUP BY id HAVING avg(ratio) > 1");  // last bits choose rows

	// ORDER BY an aggregate: the top-N shape; NULL placement as for columns.
	ExpectSql("SELECT id, count(*) AS c FROM t GROUP BY id ORDER BY c DESC LIMIT 3",
			  "SELECT TOP (3) [id], COUNT_BIG(*) AS [c] FROM [dbo].[t] GROUP BY [id] ORDER BY COUNT_BIG(*) DESC");
	ExpectSql("SELECT id FROM t GROUP BY id ORDER BY count(*) DESC, id LIMIT 3",
			  "SELECT TOP (3) [id] FROM [dbo].[t] GROUP BY [id] ORDER BY COUNT_BIG(*) DESC, [t].[id] ASC");
	ExpectSql("SELECT id, sum(amount) AS s FROM t GROUP BY id ORDER BY s LIMIT 3",
			  "SELECT TOP (3) [id], SUM([amount]) AS [s] FROM [dbo].[t] GROUP BY [id] ORDER BY CASE WHEN "
			  "SUM([amount]) IS NULL THEN 1 ELSE 0 END, SUM([amount]) ASC");
	ExpectVeto("SELECT id, avg(amount) AS a FROM t GROUP BY id ORDER BY a LIMIT 3");  // approximate
	ExpectVeto("SELECT id, sum(amount) AS s FROM t GROUP BY id ORDER BY s");		  // placement, no LIMIT
	ExpectVeto("SELECT id FROM t GROUP BY id ORDER BY name LIMIT 1");

	// DISTINCT: comparable result columns; an ORDER BY key must be one of them.
	ExpectSql("SELECT DISTINCT id, name FROM t LIMIT 5", "SELECT DISTINCT TOP (5) [id], [name] FROM [dbo].[t]");
	ExpectSql("SELECT DISTINCT id FROM t ORDER BY id LIMIT 5",
			  "SELECT DISTINCT TOP (5) [id] FROM [dbo].[t] ORDER BY [t].[id] ASC");
	ExpectVeto("SELECT DISTINCT legacy FROM t ORDER BY legacy LIMIT 5");  // read through a CAST: 145
	ExpectVeto("SELECT DISTINCT day FROM t ORDER BY day LIMIT 5");		  // the NULL-placing CASE: 145
	ExpectVeto("SELECT DISTINCT code FROM t ORDER BY code LIMIT 5");	  // the varbinary key: 145
	ExpectVeto("SELECT DISTINCT doc FROM t");							  // xml: 421 there
	ExpectVeto("SELECT DISTINCT id FROM t OFFSET 2");
	ExpectVeto("SELECT DISTINCT ON (id) id, name FROM t");
	ExpectVeto("SELECT DISTINCT avg(ratio) FROM t");

	// Joins (PR D): every relation under its own alias, every column qualified.
	ExpectSql("SELECT t.id, u.label FROM t JOIN u ON u.t_id = t.id",
			  "SELECT [r1].[id] AS [id], [r2].[label] AS [label] FROM [dbo].[t] AS [r1] INNER JOIN [dbo].[u] AS [r2] "
			  "ON ([r2].[t_id] = "
			  "[r1].[id])");
	ExpectSql("SELECT a.name, b.big FROM t AS a LEFT JOIN u AS b ON a.id = b.t_id WHERE a.id > 1",
			  "SELECT [r1].[name] AS [name], [r2].[big] AS [big] FROM [dbo].[t] AS [r1] LEFT JOIN [dbo].[u] AS [r2] ON "
			  "([r1].[id] = "
			  "[r2].[t_id]) WHERE ([r1].[id] > @p0)");
	ExpectSql(
		"SELECT label, count(*) AS c FROM t JOIN u ON t.id = u.t_id GROUP BY label",
		"SELECT [r2].[label] AS [label], COUNT_BIG(*) AS [c] FROM [dbo].[t] AS [r1] INNER JOIN [dbo].[u] AS [r2] ON "
		"([r1].[id] = [r2].[t_id]) GROUP BY [r2].[label]");
	// A self-join, told apart by aliases.
	ExpectSql("SELECT a.id AS x, b.id AS y FROM t a JOIN t b ON a.id = b.id",
			  "SELECT [r1].[id] AS [x], [r2].[id] AS [y] FROM [dbo].[t] AS [r1] INNER JOIN [dbo].[t] AS [r2] ON "
			  "([r1].[id] = [r2].[id])");
	// USING: one condition per column; the unqualified name is the left one,
	// and `*` lists it once.
	ExpectSql("SELECT id, label FROM t JOIN u USING (id)",
			  "SELECT [r1].[id] AS [id], [r2].[label] AS [label] FROM [dbo].[t] AS [r1] INNER JOIN [dbo].[u] AS [r2] "
			  "ON ([r1].[id] = "
			  "[r2].[id])");
	ExpectSql("SELECT u.* FROM t JOIN u USING (id)",
			  "SELECT [r2].[id] AS [id], [r2].[t_id] AS [t_id], [r2].[label] AS [label], [r2].[big] AS [big] FROM "
			  "[dbo].[t] AS [r1] INNER JOIN [dbo].[u] AS "
			  "[r2] ON ([r1].[id] = [r2].[id])");
	ExpectSql(
		"SELECT t.id, name FROM t CROSS JOIN u LIMIT 1",
		"SELECT TOP (1) [r1].[id] AS [id], [r1].[name] AS [name] FROM [dbo].[t] AS [r1] CROSS JOIN [dbo].[u] AS [r2]");
	// An outer join's NULL-supplying side is nullable: its NOT NULL id needs
	// the NULL placement a LIMIT allows.
	ExpectSql("SELECT u.id FROM t LEFT JOIN u ON t.id = u.t_id ORDER BY u.id LIMIT 3",
			  "SELECT TOP (3) [r2].[id] AS [id] FROM [dbo].[t] AS [r1] LEFT JOIN [dbo].[u] AS [r2] ON ([r1].[id] = "
			  "[r2].[t_id]) ORDER BY CASE WHEN [r2].[id] IS NULL THEN 1 ELSE 0 END, [r2].[id] ASC");
	ExpectVeto("SELECT u.id FROM t LEFT JOIN u ON t.id = u.t_id ORDER BY u.id");
	ExpectSql(
		"SELECT t.id FROM t JOIN u ON t.id = u.t_id ORDER BY t.id",
		"SELECT [r1].[id] AS [id] FROM [dbo].[t] AS [r1] INNER JOIN [dbo].[u] AS [r2] ON ([r1].[id] = [r2].[t_id]) "
		"ORDER BY [r1].[id] ASC");
	ExpectVeto("SELECT id FROM t JOIN u ON t.id = u.t_id");	  // id is in both
	ExpectVeto("SELECT * FROM t JOIN u ON t.id = u.t_id");	  // two result columns named id
	ExpectVeto("SELECT t.id FROM t JOIN u ON t.id = u.big");  // int = bigint: promotions differ
	ExpectVeto("SELECT t.id FROM t JOIN t ON t.id = t.id");	  // two tables named t
	ExpectVeto("SELECT t.id FROM t NATURAL JOIN u");
	// SEMI / ANTI: EXISTS / NOT EXISTS ANDed to WHERE (not IN / NOT IN, which
	// a NULL on the right would empty).
	ExpectSql("SELECT id, name FROM t SEMI JOIN u ON t.id = u.t_id WHERE t.id > 1",
			  "SELECT [r1].[id] AS [id], [r1].[name] AS [name] FROM [dbo].[t] AS [r1] WHERE (([r1].[id] > @p0) AND "
			  "EXISTS (SELECT 1 FROM [dbo].[u] AS [r2] WHERE ([r1].[id] = [r2].[t_id])))");
	ExpectSql("SELECT id FROM t ANTI JOIN u ON t.id = u.t_id",
			  "SELECT [r1].[id] AS [id] FROM [dbo].[t] AS [r1] WHERE (NOT EXISTS (SELECT 1 FROM [dbo].[u] AS [r2] "
			  "WHERE ([r1].[id] = [r2].[t_id])))");
	ExpectSql(
		"SELECT * FROM t SEMI JOIN u USING (id) LIMIT 1",
		"SELECT TOP (1) [r1].[id] AS [id], [r1].[name] AS [name], [r1].[code] AS [code], [r1].[amount] AS [amount], "
		"[r1].[day] AS [day], [r1].[flag] AS [flag], [r1].[ts] AS [ts], CAST([r1].[legacy] AS NVARCHAR(30)) AS "
		"[legacy], [r1].[wide] AS [wide], [r1].[mid] AS [mid], [r1].[tiny] AS [tiny], [r1].[ratio] AS [ratio], "
		"[r1].[doc] AS [doc], "
		"[r1].[small] AS [small] FROM "
		"[dbo].[t] AS [r1] WHERE EXISTS (SELECT 1 FROM [dbo].[u] AS [r2] WHERE ([r1].[id] = [r2].[id]))");
	ExpectVeto("SELECT u.label FROM t SEMI JOIN u ON t.id = u.t_id");  // the right side is gone
	ExpectVeto("SELECT id FROM t SEMI JOIN u ON t.id = u.t_id WHERE u.big > 1");
	ExpectVeto("SELECT u.* FROM t SEMI JOIN u ON t.id = u.t_id");
	// A later USING finds its left column among the tables that left columns.
	ExpectVeto("SELECT t.id FROM t SEMI JOIN u ON t.id = u.t_id JOIN u AS v USING (label)");
	ExpectSql("SELECT t.id FROM t SEMI JOIN u USING (id) JOIN u AS v USING (id)",
			  "SELECT [r1].[id] AS [id] FROM [dbo].[t] AS [r1] INNER JOIN [dbo].[u] AS [r3] ON ([r1].[id] = [r3].[id]) "
			  "WHERE EXISTS (SELECT 1 FROM [dbo].[u] AS [r2] WHERE ([r1].[id] = [r2].[id]))");
	// A LEFT join before it keeps the EXISTS exact; a RIGHT / FULL join over it
	// would bring rows the moved EXISTS drops.
	ExpectSql("SELECT t.id FROM t LEFT JOIN u ON t.id = u.t_id SEMI JOIN t AS x ON x.id = t.id",
			  "SELECT [r1].[id] AS [id] FROM [dbo].[t] AS [r1] LEFT JOIN [dbo].[u] AS [r2] ON ([r1].[id] = "
			  "[r2].[t_id]) WHERE EXISTS (SELECT 1 FROM [dbo].[t] AS [r3] WHERE ([r3].[id] = [r1].[id]))");
	ExpectVeto("SELECT u.id FROM t SEMI JOIN t AS x ON x.id = t.id RIGHT JOIN u ON u.t_id = t.id");
	ExpectVeto("SELECT u.id FROM t ANTI JOIN t AS x ON x.id = t.id FULL JOIN u ON u.t_id = t.id");
	ExpectVeto("SELECT t.id FROM t ASOF JOIN u ON t.id >= u.t_id");
	ExpectVeto("SELECT t.id FROM t POSITIONAL JOIN u");
	ExpectVeto("SELECT id FROM t RIGHT JOIN u USING (id)");
	ExpectVeto("SELECT id FROM t FULL JOIN u USING (id)");
	ExpectVeto("SELECT t.id FROM t JOIN (u JOIN t AS x ON u.t_id = x.id) ON t.id = u.t_id");
	ExpectSql("SELECT t.id FROM t JOIN (SELECT * FROM u) s ON t.id = s.t_id",
			  "SELECT [r1].[id] AS [id] FROM [dbo].[t] AS [r1] INNER JOIN (SELECT [id], [t_id], [label], [big] FROM "
			  "[dbo].[u]) AS [r2] ON ([r1].[id] = [r2].[t_id])");
	ExpectVeto("SELECT t.id FROM t JOIN v ON t.id = v.id");								  // not this catalog's
	ExpectVeto("SELECT t.id FROM t JOIN u ON t.id = x.id JOIN t AS x ON x.id = u.t_id");  // ON names a later table

	// Full review of PR D (fuzz): a repeated ORDER BY key is dropped (169).
	ExpectSql("SELECT id FROM t ORDER BY id, id DESC LIMIT 2",
			  "SELECT TOP (2) [id] FROM [dbo].[t] ORDER BY [t].[id] ASC");
	// DuckDB moves a constant across a comparison with a constant and never
	// computes `id - 5`; the server would, and could overflow.
	ExpectVeto("SELECT id FROM t WHERE id - 5 > 3 LIMIT 1");
	ExpectVeto("SELECT id FROM t WHERE id * 2 = 10 LIMIT 1");
	ExpectVeto("SELECT id FROM t WHERE -id > 3 LIMIT 1");
	ExpectSql("SELECT id FROM t WHERE -amount > 3 LIMIT 1",
			  "SELECT TOP (1) [id] FROM [dbo].[t] WHERE ((-[amount]) > @p0)");	// moved, but no overflow
	ExpectVeto("SELECT id FROM t WHERE ratio + 1 > 5 LIMIT 1");	 // not moved, but a float sum is not pushed
	ExpectVeto("SELECT -(amount + 700) AS x FROM t LIMIT 1");	 // the negation keeps the uncertainty
	{
		WrittenQuery out;
		std::string why;
		if (!Write("SELECT id, coalesce(sum(amount), 0) AS s FROM t GROUP BY id", out, why) ||
			out.column_types[1] != LogicalType::DECIMAL(38, 2)) {
			std::cerr << "FAIL: coalesce(sum(amount), 0) is not DECIMAL(38,2) (" << why << ")\n";
			failures++;
		}
	}
	ExpectVeto("SELECT id FROM t WHERE id + 1 BETWEEN 2 AND 5 LIMIT 1");
	ExpectSql("SELECT id FROM t WHERE id + id > 3 LIMIT 1",
			  "SELECT TOP (1) [id] FROM [dbo].[t] WHERE (([id] + [id]) > @p0)");
	// A computed decimal carries DuckDB's type, or is not pushed.
	ExpectVeto("SELECT amount + 700 AS a FROM t LIMIT 1");	// DECIMAL(13,2) there
	ExpectSql("SELECT id FROM t WHERE id + 1 > id LIMIT 1",
			  "SELECT TOP (1) [id] FROM [dbo].[t] WHERE (([id] + @p0) > [id])");  // beside a column: not moved
	{
		WrittenQuery out;
		std::string why;
		if (!Write("SELECT amount + amount AS a FROM t LIMIT 1", out, why) ||
			out.column_types[0] != LogicalType::DECIMAL(11, 2) || out.cast_types[0].id() != LogicalTypeId::INVALID) {
			std::cerr << "FAIL: amount + amount types (" << why << ")\n";
			failures++;
		}
	}
	{
		// A product DuckDB types exactly (5 + 5 digits fit 18): the server's
		// decimal(11,4) is read and cast to DuckDB's DECIMAL(10,4).
		WrittenQuery out;
		std::string why;
		if (!Write("SELECT small * small AS p FROM t LIMIT 1", out, why) ||
			out.cast_types[0] != LogicalType::DECIMAL(10, 4) || out.column_types[0].id() != LogicalTypeId::INVALID) {
			std::cerr << "FAIL: small * small is not cast back to DECIMAL(10,4) (" << why << ")\n";
			failures++;
		}
	}
	ExpectVeto("SELECT -(small * small) FROM t LIMIT 1");
	// Doubles: compared, not computed -- + - * overflow on the server where
	// DuckDB says inf (review of #396, one rule for both paths).
	ExpectSql("SELECT id FROM t WHERE ratio > 1.5 LIMIT 1", "SELECT TOP (1) [id] FROM [dbo].[t] WHERE ([ratio] > @p0)");
	ExpectVeto("SELECT ratio - ratio AS r FROM t LIMIT 1");
	ExpectVeto("SELECT ratio + 1 AS r FROM t LIMIT 1");
	ExpectVeto("SELECT ratio * ratio AS r FROM t LIMIT 1");
	ExpectVeto("SELECT ratio % 2 FROM t LIMIT 1");
	ExpectVeto("SELECT ratio + amount FROM t LIMIT 1");

	// Derived tables (PR E1): the inner node rendered in place, its columns
	// already read (the NVARCHAR cast of a code-page varchar is not repeated),
	// one parameter set for the statement.
	ExpectSql(
		"SELECT s.g, s.c FROM (SELECT id AS g, count(*) AS c FROM t WHERE id > 3 GROUP BY id) s WHERE s.c > 1 "
		"ORDER BY s.g LIMIT 5",
		"SELECT TOP (5) [r1].[g] AS [g], [r1].[c] AS [c] FROM (SELECT [id] AS [g], COUNT_BIG(*) AS [c] FROM "
		"[dbo].[t] WHERE ([id] > @p0) GROUP BY [id]) AS [r1] WHERE ([r1].[c] > @p1) ORDER BY [r1].[g] ASC");
	ExpectSql("SELECT s.legacy FROM (SELECT legacy FROM t ORDER BY id LIMIT 3) s",
			  "SELECT [r1].[legacy] AS [legacy] FROM (SELECT TOP (3) CAST([legacy] AS NVARCHAR(30)) AS [legacy] FROM "
			  "[dbo].[t] ORDER BY [t].[id] ASC) AS [r1]");
	{
		WrittenQuery out;
		std::string why;
		if (!Write("SELECT s.total FROM (SELECT id, sum(id) AS total FROM t GROUP BY id) s WHERE s.total > 10", out,
				   why) ||
			out.cast_types[0] != LogicalType::HUGEINT) {
			std::cerr << "FAIL: a derived integer SUM is not cast back (" << why << ")\n";
			failures++;
		}
	}
	ExpectVeto("SELECT sum(s.total) FROM (SELECT id, sum(id) AS total FROM t GROUP BY id) s");	// HUGEINT here
	ExpectVeto("SELECT s.id FROM (SELECT id FROM t ORDER BY id) s");				   // ORDER BY without TOP: 1033
	ExpectVeto("SELECT s.a FROM (SELECT id FROM t) s(a)");							   // column aliases: not yet
	ExpectVeto("SELECT s.legacy FROM (SELECT legacy FROM t) s WHERE s.legacy = 'x'");  // read through a CAST
	// A division or a floating-point aggregate stays a value outside its node.
	ExpectVeto("SELECT s.id FROM (SELECT id, id / amount AS r FROM t) s WHERE s.r > 1");
	ExpectVeto("SELECT s.id FROM (SELECT id, id / amount AS r FROM t) s ORDER BY s.r LIMIT 2");
	ExpectVeto("SELECT coalesce(s.r, -1) FROM (SELECT id, id / amount AS r FROM t) s");
	ExpectVeto("SELECT s.id FROM (SELECT id, avg(ratio) AS a FROM t GROUP BY id) s WHERE s.a > 1");
	ExpectSql(
		"SELECT s.r FROM (SELECT id, id / amount AS r FROM t) s LIMIT 1",
		"SELECT TOP (1) [r1].[r] AS [r] FROM (SELECT [id], (CAST([id] AS float) / NULLIF(CAST([amount] AS float), 0)) "
		"AS [r] FROM [dbo].[t]) AS [r1]");
	ExpectGain("SELECT * FROM (SELECT id, count(*) AS c FROM t GROUP BY id) s", true);
	ExpectGain("SELECT * FROM (SELECT id FROM t WHERE id > 1) s", false);

	// The PR E1 gain check: a join link that equates a unique key cannot send
	// more rows than the other side; one that equates none (many-to-many,
	// CROSS) is uncertain unless an aggregate / DISTINCT / LIMIT bounds it.
	{
		auto uncertain = [&](const std::string &sql, bool expected) {
			WrittenQuery out;
			std::string why;
			if (!Write(sql, out, why) || out.gain_uncertain != expected || out.largest_input_rows != 5000000) {
				std::cerr << "FAIL: gain_uncertain(" << sql << ") != " << expected << " (" << why << ")\n";
				failures++;
			}
		};
		uncertain("SELECT t.name FROM t JOIN u ON u.t_id = t.id", false);	// t's key
		uncertain("SELECT u.label FROM u JOIN t ON t.id = u.t_id", false);	// the right side's key
		uncertain("SELECT a.label FROM u a JOIN u b ON a.t_id = b.t_id JOIN t ON t.id = a.t_id", true);
		uncertain(
			"SELECT a.t_id, count(*) AS c FROM u a JOIN u b ON a.t_id = b.t_id JOIN t ON t.id = a.t_id GROUP BY a.t_id",
			false);
		uncertain("SELECT a.label FROM u a JOIN u b ON a.t_id = b.t_id JOIN t ON t.id = a.t_id LIMIT 5", false);
		uncertain("SELECT t.name FROM t JOIN u ON u.t_id = t.id AND u.label = t.name", false);
		uncertain("SELECT t.id, u.label FROM t CROSS JOIN u ORDER BY t.id", true);
		uncertain("SELECT t.name FROM t SEMI JOIN u ON u.t_id = t.id ORDER BY t.id", false);
		// A GROUP BY / DISTINCT keeping a key of every table reduces nothing.
		uncertain(
			"SELECT a.id, b.id AS bid, count(*) AS c FROM u a JOIN u b ON a.t_id = b.t_id JOIN t ON t.id = a.t_id "
			"GROUP BY a.id, b.id, t.id",
			true);
		uncertain(
			"SELECT DISTINCT a.id, b.id AS bid, t.id AS tid FROM u a JOIN u b ON a.t_id = b.t_id JOIN t ON t.id = "
			"a.t_id",
			true);
		// A comma join is bounded by WHERE's equalities as by an ON.
		uncertain("SELECT t.name FROM t, u WHERE u.t_id = t.id ORDER BY t.id", false);
		uncertain("SELECT t.name FROM t, u WHERE u.t_id > t.id ORDER BY t.id", true);
		// An OR equality bounds nothing.
		uncertain("SELECT t.name FROM t JOIN u ON u.t_id = t.id OR u.big = 1 ORDER BY t.id", true);
		// Wrapped in a derived table or a CTE, the join's gain is still
		// uncertain (review of E1), unless the node around it reduces.
		uncertain("SELECT * FROM (SELECT a.id FROM t a JOIN t b ON a.name = b.name) s", true);
		uncertain("WITH s AS (SELECT a.id FROM t a JOIN t b ON a.name = b.name) SELECT * FROM s", true);
		uncertain("SELECT count(*) AS c FROM (SELECT a.id FROM t a JOIN t b ON a.name = b.name) s", false);
		// A GROUP BY over the wrapper's columns does not reduce its join: they
		// are not keys of the tables inside.
		uncertain("SELECT s.id, count(*) AS c FROM (SELECT a.id FROM t a JOIN t b ON a.name = b.name) s GROUP BY s.id",
				  true);
		// A derived table's GROUP BY columns are its key.
		uncertain("SELECT t.name, s.c FROM t JOIN (SELECT id, count(*) AS c FROM t GROUP BY id) s ON s.id = t.id",
				  false);
	}

	// Subqueries in expressions (PR E1): IN / EXISTS / a scalar subquery,
	// correlated through the outer node's aliases, the inner's aliased past them.
	ExpectSql("SELECT id FROM t WHERE id IN (SELECT t_id FROM u)",
			  "SELECT [r1].[id] AS [id] FROM [dbo].[t] AS [r1] WHERE ([r1].[id] IN (SELECT [r2].[t_id] AS [t_id] FROM "
			  "[dbo].[u] AS [r2]))");
	ExpectSql("SELECT id FROM t WHERE NOT EXISTS (SELECT 1 FROM u WHERE u.t_id = t.id)",
			  "SELECT [r1].[id] AS [id] FROM [dbo].[t] AS [r1] WHERE (NOT EXISTS (SELECT 1 AS [1] FROM [dbo].[u] AS "
			  "[r2] WHERE ([r2].[t_id] = [r1].[id])))");
	ExpectSql("SELECT id, (SELECT max(t_id) FROM u WHERE u.id = t.id) AS m FROM t",
			  "SELECT [r1].[id] AS [id], (SELECT MAX([r2].[t_id]) AS [max(t_id)] FROM [dbo].[u] AS [r2] WHERE "
			  "([r2].[id] = [r1].[id])) AS [m] FROM [dbo].[t] AS [r1]");
	ExpectVeto("SELECT id FROM t WHERE id IN (SELECT t_id, id FROM u)");
	// An integer constant outside int: BIGINT here, numeric(10,0) there
	// (review of #399: only the upper bound was checked).
	ExpectVeto("SELECT -3000000000 FROM t LIMIT 1");
	ExpectVeto("SELECT 3000000000 FROM t LIMIT 1");									// two columns
	ExpectVeto("SELECT id FROM t WHERE id IN (SELECT t_id FROM u ORDER BY t_id)");	// 1033
	ExpectVeto("SELECT id FROM t WHERE id > ANY (SELECT t_id FROM u)");				// not = ANY
	ExpectVeto("SELECT id FROM t WHERE 0 < (SELECT max(u.t_id + t.id) FROM u)");	// an outer aggregate
	ExpectVeto("SELECT sum((SELECT max(t_id) FROM u)) FROM t");						// 130 there
	ExpectVeto("SELECT id FROM u WHERE label IN (SELECT legacy FROM t)");			// read through a CAST
	ExpectVeto("SELECT id FROM t WHERE (SELECT id / 0 FROM u LIMIT 1) > 1");		// a division compared
	ExpectGain("SELECT id FROM t WHERE id IN (SELECT t_id FROM u)", true);
	{
		// A correlated subquery asked about on its own refers outside.
		WrittenQuery out;
		std::string why;
		if (Write("SELECT 1 FROM u WHERE u.t_id = t.id", out, why) || !out.refers_outside) {
			std::cerr << "FAIL: a correlated subquery alone does not refer outside (" << why << ")\n";
			failures++;
		}
		if (Write("SELECT 1 FROM u WHERE u.nope = 1", out, why) || out.refers_outside) {
			std::cerr << "FAIL: an unknown qualified column refers outside\n";
			failures++;
		}
	}
	{
		SQLWriterOptions lenient;
		lenient.scalar_subquery_errors = false;
		WrittenQuery out;
		std::string why;
		if (WriteWith(lenient, "SELECT id FROM t WHERE id = (SELECT t_id FROM u LIMIT 1)", out, why)) {
			std::cerr << "FAIL: a scalar subquery pushed under scalar_subquery_error_on_multiple_rows = false\n";
			failures++;
		}
	}

	// Set operations (PR E1): rendered where nested -- the catalog keeps one
	// at a statement's top with DuckDB; the writer renders it either way.
	ExpectSql("SELECT count(*) FROM (SELECT id FROM t UNION ALL SELECT t_id FROM u) s",
			  "SELECT COUNT_BIG(*) AS [count_star()] FROM (SELECT [r1].[id] AS [id] FROM [dbo].[t] AS [r1] UNION ALL "
			  "SELECT [r1].[t_id] AS [t_id] FROM [dbo].[u] AS [r1]) AS [r1]");
	ExpectSql("SELECT id FROM t WHERE id IN (SELECT id FROM t EXCEPT SELECT t_id FROM u)",
			  "SELECT [r1].[id] AS [id] FROM [dbo].[t] AS [r1] WHERE ([r1].[id] IN (SELECT [r2].[id] AS [id] FROM "
			  "[dbo].[t] AS [r2] EXCEPT SELECT [r2].[t_id] AS [t_id] FROM [dbo].[u] AS [r2]))");
	ExpectVeto("SELECT count(*) FROM (SELECT id FROM t EXCEPT ALL SELECT t_id FROM u) s");	   // no EXCEPT ALL
	ExpectVeto("SELECT count(*) FROM (SELECT id FROM t UNION SELECT big FROM u) s");		   // int with bigint
	ExpectVeto("SELECT count(*) FROM (SELECT id FROM t UNION BY NAME SELECT t_id FROM u) s");  // BY NAME
	// Floating-point aggregates compared.
	ExpectVeto("SELECT count(*) FROM (SELECT avg(ratio) AS a FROM t UNION SELECT avg(ratio) AS a FROM t) s");
	ExpectVeto("SELECT count(*) FROM (SELECT legacy FROM t UNION ALL SELECT label FROM u) s");	// two string types
	// ORDER BY / LIMIT of the set operation: a SELECT * wrapper; of a member:
	// the member in a derived table of its own.
	ExpectSql("SELECT s.id FROM (SELECT id FROM t UNION SELECT t_id FROM u ORDER BY id LIMIT 2) s LIMIT 5",
			  "SELECT TOP (5) [r1].[id] AS [id] FROM (SELECT TOP (2) [r1].[id] AS [id] FROM (SELECT [r2].[id] AS [id] "
			  "FROM [dbo].[t] AS [r2] UNION SELECT [r2].[t_id] AS [t_id] FROM [dbo].[u] AS [r2]) AS [r1] ORDER BY "
			  "CASE WHEN [r1].[id] IS NULL THEN 1 ELSE 0 END, [r1].[id] ASC) AS [r1]");
	ExpectSql("SELECT count(*) FROM ((SELECT id FROM t ORDER BY id LIMIT 1) UNION ALL SELECT t_id FROM u) s",
			  "SELECT COUNT_BIG(*) AS [count_star()] FROM (SELECT * FROM (SELECT TOP (1) [r1].[id] AS [id] FROM "
			  "[dbo].[t] AS [r1] ORDER BY [r1].[id] ASC) AS [q1] UNION ALL SELECT [r1].[t_id] AS [t_id] FROM [dbo].[u] "
			  "AS [r1]) AS [r1]");
	ExpectVeto("SELECT count(*) FROM (SELECT id FROM t UNION ALL (SELECT t_id FROM u ORDER BY t_id)) s");  // 1033
	ExpectVeto("SELECT s.id FROM (SELECT id FROM t UNION SELECT t_id FROM u ORDER BY id + 1 LIMIT 2) s");
	ExpectGain("SELECT * FROM (SELECT id FROM t UNION ALL SELECT t_id FROM u) s", false);
	ExpectGain("SELECT * FROM (SELECT id FROM t UNION SELECT t_id FROM u) s", true);

	// CTEs (PR E1): inlined where referenced, a body seeing the earlier ones.
	ExpectSql("WITH q AS (SELECT id, count(*) AS c FROM t GROUP BY id) SELECT q.id FROM q WHERE q.c > 1",
			  "SELECT [r1].[id] AS [id] FROM (SELECT [id], COUNT_BIG(*) AS [c] FROM [dbo].[t] GROUP BY [id]) AS [r1] "
			  "WHERE ([r1].[c] > @p0)");
	ExpectSql("WITH a AS (SELECT id FROM t GROUP BY id), b AS (SELECT a.id FROM a) SELECT count(*) FROM b",
			  "SELECT COUNT_BIG(*) AS [count_star()] FROM (SELECT [r1].[id] AS [id] FROM (SELECT [id] FROM [dbo].[t] "
			  "GROUP BY [id]) AS [r1]) AS [r1]");
	// A qualified name is the table, not the CTE of that name.
	ExpectSql("WITH t AS (SELECT t_id FROM u GROUP BY t_id) SELECT count(*) FROM dbo.t",
			  "SELECT COUNT_BIG(*) AS [count_star()] FROM [dbo].[t]");
	ExpectVeto("WITH a AS (SELECT id FROM t ORDER BY id LIMIT 2) SELECT count(*) FROM a x, a y");  // LIMIT twice
	ExpectVeto("WITH a AS (SELECT id FROM t ORDER BY id LIMIT 2) SELECT id FROM t WHERE id IN (SELECT id FROM a)");
	ExpectVeto("WITH a(x) AS (SELECT id FROM t GROUP BY id) SELECT count(*) FROM a");  // column aliases
	ExpectVeto("WITH a AS (SELECT b.id FROM b), b AS (SELECT id FROM t GROUP BY id) SELECT count(*) FROM a");
	ExpectGain("WITH q AS (SELECT id, count(*) AS c FROM t GROUP BY id) SELECT * FROM q", true);
	{
		// A CTE read twice by a CTE read twice ... doubles per level: past the
		// cap it is vetoed, and quickly (review of E1).
		std::string sql = "WITH c0 AS (SELECT id FROM t WHERE id > 1)";
		for (int level = 1; level <= 30; level++) {
			sql += ", c" + std::to_string(level) + " AS (SELECT x.id FROM c" + std::to_string(level - 1) + " x JOIN c" +
				   std::to_string(level - 1) + " y ON x.id = y.id)";
		}
		sql += " SELECT count(*) FROM c30";
		ExpectVeto(sql);
	}
	// A scalar subquery pushed only when it returns one row: a key lookup, an
	// aggregate, a LIMIT 1 (review of E1: lazily evaluated on the server).
	ExpectSql("SELECT id, (SELECT label FROM u WHERE u.id = t.id) AS l FROM t",
			  "SELECT [r1].[id] AS [id], (SELECT [r2].[label] AS [label] FROM [dbo].[u] AS [r2] WHERE ([r2].[id] = "
			  "[r1].[id])) AS [l] FROM [dbo].[t] AS [r1]");
	ExpectVeto("SELECT id, (SELECT label FROM u WHERE u.t_id = t.id) AS l FROM t");
	// A string key against an outer column: other rules may meet two keys.
	ExpectVeto("SELECT id, (SELECT t_id FROM u WHERE u.label = t.name) AS l FROM t");
	ExpectVeto("SELECT id, (SELECT label FROM u WHERE u.id = t.id OR u.id = 1) AS l FROM t");
	ExpectVeto("SELECT id, (SELECT max(t_id) FROM u WHERE u.id = t.id) FROM t");  // no alias
	// A LIMIT anywhere in the body, or reached through another CTE under a
	// subquery expression (review of E1).
	ExpectVeto("WITH a AS (SELECT * FROM (SELECT id FROM t ORDER BY id LIMIT 1) s) SELECT count(*) FROM a x, a y");
	ExpectVeto(
		"WITH a AS (SELECT id FROM t ORDER BY id LIMIT 1), b AS (SELECT id FROM a) SELECT id FROM t WHERE EXISTS "
		"(SELECT 1 FROM b WHERE b.id = t.id)");
	{
		// A body inlined twice renders its constant twice, each its own @pN.
		WrittenQuery out;
		std::string why;
		if (!Write("WITH a AS (SELECT id FROM t WHERE id > 5) SELECT x.id FROM a x JOIN a y ON x.id = y.id", out,
				   why) ||
			out.params.size() != 2 || out.statement.find("@p0") == std::string::npos ||
			out.statement.find("@p1") == std::string::npos) {
			std::cerr << "FAIL: a CTE inlined twice (" << why << "): " << out.statement << "\n";
			failures++;
		}
	}

	// The gain rule: a node the catalog scan serves as well stays with it.
	ExpectGain("SELECT * FROM t", false);
	ExpectGain("SELECT id FROM t WHERE id = 1", false);
	ExpectGain("SELECT id FROM t ORDER BY id", true);
	ExpectGain("SELECT id FROM t LIMIT 1", true);
	ExpectGain("SELECT DISTINCT id FROM t", true);
	ExpectGain("SELECT count(*) FROM t", true);
	ExpectGain("SELECT id FROM t GROUP BY id", true);
	ExpectGain("SELECT id + 1 FROM t WHERE id > 1", false);
	ExpectGain("SELECT t.id FROM t JOIN u ON t.id = u.t_id", true);
	ExpectGain("SELECT t.id FROM t CROSS JOIN u", false);
	ExpectGain("SELECT t.id FROM t JOIN u ON t.id = u.t_id CROSS JOIN t AS z", false);
	ExpectGain("SELECT t.id FROM t, u WHERE t.id = u.t_id", false);

	// column_types spells the collation that ToString leaves out.
	auto columns = Columns();
	auto legacy = ColumnTypeName(columns[7].NativeDuckDBType());
	if (legacy != "MSSQL_VARCHAR(30, 'SQL_Latin1_General_CP1_CI_AS')") {
		std::cerr << "FAIL: ColumnTypeName(legacy) = " << legacy << "\n";
		failures++;
	}
	if (!ColumnTypeName(LogicalType::INVALID).empty()) {
		std::cerr << "FAIL: ColumnTypeName(INVALID) is not empty\n";
		failures++;
	}
	if (ColumnTypeName(columns[6].duckdb_type) != "TIMESTAMP_NS") {
		std::cerr << "FAIL: ColumnTypeName(ts) = " << ColumnTypeName(columns[6].duckdb_type) << "\n";
		failures++;
	}

	if (failures) {
		std::cerr << failures << " failure(s)\n";
		return 1;
	}
	std::cout << "test_sql_writer: all passed\n";
	return 0;
}
