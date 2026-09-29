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
	return columns;
}

static bool Write(const std::string &sql, WrittenQuery &out, std::string &why, bool parameterize = true) {
	static const auto columns = Columns();
	Parser parser;
	parser.ParseQuery(sql);
	auto &node = *parser.statements[0]->Cast<SelectStatement>().node;
	SQLWriterOptions options;
	options.parameterize = parameterize;
	SQLWriter writer(options, [](const BaseTableRef &ref, WriterTable &table) {
		if (ref.Table().GetIdentifierName() != "t") {
			return false;
		}
		table.schema = "dbo";
		table.name = "t";
		table.columns = &columns;
		return true;
	});
	return writer.Write(node, out, why);
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
	Parser parser;
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
			  "[legacy], [wide], [mid] FROM [dbo].[t]");
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
	ExpectSql("SELECT id FROM t WHERE id + 1 > 5 LIMIT 1",
			  "SELECT TOP (1) [id] FROM [dbo].[t] WHERE (([id] + @p0) > @p1)");
	ExpectSql("SELECT id FROM t WHERE id % 2 = 0 LIMIT 1",
			  "SELECT TOP (1) [id] FROM [dbo].[t] WHERE (([id] % @p0) = @p1)");
	ExpectSql("SELECT amount + amount AS d FROM t LIMIT 1",
			  "SELECT TOP (1) ([amount] + [amount]) AS [d] FROM [dbo].[t]");
	ExpectSql("SELECT id FROM t WHERE amount + 1 > 2 LIMIT 1",
			  "SELECT TOP (1) [id] FROM [dbo].[t] WHERE (([amount] + @p0) > @p1)");
	ExpectSql("SELECT id + 1 FROM t LIMIT 1", "SELECT TOP (1) ([id] + CAST(1 AS int)) AS [(id + 1)] FROM [dbo].[t]",
			  false);
	ExpectVeto("SELECT wide + wide FROM t LIMIT 1");  // width 38: the server would round
	ExpectVeto("SELECT mid * mid FROM t LIMIT 1");	  // p1 + p2 + 1 > 38: the server reduces the scale
	ExpectSql("SELECT amount * amount AS p FROM t LIMIT 1",
			  "SELECT TOP (1) ([amount] * [amount]) AS [p] FROM [dbo].[t]");
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

	// CASE, COALESCE, NULLIF: one type for every branch.
	ExpectSql("SELECT CASE WHEN id > 1 THEN id ELSE 0 END AS c FROM t LIMIT 1",
			  "SELECT TOP (1) CASE WHEN ([id] > @p0) THEN [id] ELSE @p1 END AS [c] FROM [dbo].[t]");
	ExpectSql("SELECT coalesce(amount, 0) AS c FROM t LIMIT 1",
			  "SELECT TOP (1) COALESCE([amount], @p0) AS [c] FROM [dbo].[t]");
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

	// Everything outside PR B's vocabulary is a veto, never a guess.
	ExpectVeto("SELECT rowid FROM t LIMIT 1");
	ExpectVeto("SELECT DISTINCT id FROM t LIMIT 1");
	ExpectVeto("SELECT id, count(*) FROM t GROUP BY id LIMIT 1");
	ExpectVeto("SELECT * EXCLUDE (id) FROM t LIMIT 1");
	ExpectVeto("SELECT id, id FROM t LIMIT 1");
	ExpectVeto("SELECT id FROM u LIMIT 1");
	ExpectVeto("SELECT t.id FROM t, t AS u LIMIT 1");
	ExpectVeto("WITH c AS (SELECT 1) SELECT id FROM t LIMIT 1");
	ExpectVeto("SELECT other.id FROM t LIMIT 1");

	// The gain rule: a node the catalog scan serves as well stays with it.
	ExpectGain("SELECT * FROM t", false);
	ExpectGain("SELECT id FROM t WHERE id = 1", false);
	ExpectGain("SELECT id FROM t ORDER BY id", true);
	ExpectGain("SELECT id FROM t LIMIT 1", true);

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
