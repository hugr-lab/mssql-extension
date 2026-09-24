// test/cpp/test_ddl_detect.cpp
//
// mssql::SqlMayChangeSchema (review of #382): the DDL test behind mssql_exec's
// cache invalidation and, inside a transaction, the transaction's "stop
// trusting the shared cache" mark. It was a substring match, so a literal, a
// column name or a comment mentioning a keyword wiped the transaction's cache.
//
// Header-only: builds with `-I src/include` and no DuckDB.

#include <iostream>
#include <string>

#include "query/mssql_ddl_detect.hpp"

using duckdb::mssql::SqlMayChangeSchema;

static int g_failures = 0;

static void Expect(const std::string &sql, bool want) {
	const bool got = SqlMayChangeSchema(sql);
	if (got != want) {
		std::cerr << "FAIL: " << (want ? "DDL" : "not DDL") << " expected for: " << sql << std::endl;
		g_failures++;
	}
}

int main() {
	// DDL, in every form that can change schema.
	Expect("CREATE TABLE dbo.t (id int)", true);
	Expect("  drop table dbo.t", true);
	Expect("ALTER TABLE t ADD c int", true);
	Expect("TRUNCATE TABLE t", true);
	Expect("EXEC dbo.p", true);
	Expect("execute sp_who", true);
	Expect("EXEC sp_rename 'a', 'b'", true);
	Expect("sp_rename 'a', 'b'", true);
	Expect("INSERT INTO t VALUES (1); CREATE INDEX ix ON t(c)", true);
	Expect("/* setup */ CREATE TABLE t (id int)", true);
	// Dynamic SQL called without EXEC as a batch's first statement: its DDL is
	// inside a literal, so the call itself has to count.
	Expect("sp_executesql N'DROP TABLE t'", true);

	// Not DDL: the keyword is inside something else.
	Expect("UPDATE dbo.Orders SET status = 'created'", false);
	Expect("UPDATE t SET s = N'drop it'", false);
	Expect("SELECT dropped_at, created_by FROM t", false);
	Expect("SELECT [create] FROM t", false);
	Expect("SELECT \"alter\" FROM t", false);
	Expect("SELECT 1 -- we used to drop here", false);
	Expect("SELECT 1 /* exec /* nested */ create */", false);
	Expect("SELECT 'it''s created'", false);
	Expect("DELETE FROM executions WHERE id = 1", false);
	Expect("INSERT INTO t VALUES ('CREATE TABLE x')", false);
	Expect("SELECT @exec, #create_tmp FROM t", false);

	if (g_failures > 0) {
		std::cerr << g_failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "All DDL detection tests passed" << std::endl;
	return 0;
}
