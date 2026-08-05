// test/cpp/test_insert_bulk_sql.cpp
//
// Unit tests for mssql::BuildInsertBulkSql (spec 063 D4).
//
// Why this file exists rather than a .test. The statement it builds is never
// visible from SQL — it is sent to the server and the server either accepts it
// or does not. Every end-to-end test of a load passes whether or not the hints
// are there, which is exactly how CTAS came to emit `WITH (TABLOCK)` and never
// `ROWS_PER_BATCH`: the server was told the batch size for a COPY and left to
// guess it for a CTAS, and nothing anywhere noticed for a release.
//
// So the drift is closed by construction — one builder for both — and asserted
// here, on the text.
//
// Links against the built extension archive; run with `make test-cpp`.

#include <iostream>
#include <string>

#include "copy/bulk_load_session.hpp"

using namespace duckdb;
using namespace duckdb::mssql;

static int g_failures = 0;

static void Check(const std::string &what, const std::string &actual, const std::string &expected) {
	if (actual != expected) {
		std::cerr << "FAIL: " << what << "\n  got:      " << actual << "\n  expected: " << expected << "\n";
		++g_failures;
	} else {
		std::cout << "ok: " << what << "\n     " << actual << "\n";
	}
}

// What this file asserts is the STRUCTURE the builder produces — bracketing,
// comma joining, the WITH clause, temp-table naming — and not how a column
// renders its own type. That rendering is GetSQLServerTypeDeclaration()'s
// contract and test_target_resolver's subject, so the expectations below compose
// it from the same call rather than restating it; otherwise this file would fail
// every time a type declaration legitimately changed.

int main() {
	std::cout << "== BuildInsertBulkSql unit tests (spec 063 D4) ==\n";

	BCPCopyTarget permanent("cat", "dbo", "Target");
	BCPCopyTarget local_temp("cat", "", "#stage");
	BCPCopyTarget global_temp("cat", "", "##stage");

	vector<BCPColumnMetadata> cols;
	cols.push_back(BCPColumnMetadata("id", LogicalType::BIGINT));
	cols.push_back(BCPColumnMetadata("v", LogicalType::VARCHAR));

	const std::string body =
		"([id] " + cols[0].GetSQLServerTypeDeclaration() + ", [v] " + cols[1].GetSQLServerTypeDeclaration() + ")";

	// The four hint combinations. ROWS_PER_BATCH is the one that was missing on
	// the CTAS side, so each row it appears in is a regression guard.
	Check("no hints", BuildInsertBulkSql(permanent, cols, false, 0), "INSERT BULK [dbo].[Target] " + body);
	Check("tablock only", BuildInsertBulkSql(permanent, cols, true, 0),
		  "INSERT BULK [dbo].[Target] " + body + " WITH (TABLOCK)");
	Check("rows_per_batch only", BuildInsertBulkSql(permanent, cols, false, 102400),
		  "INSERT BULK [dbo].[Target] " + body + " WITH (ROWS_PER_BATCH = 102400)");
	Check("both", BuildInsertBulkSql(permanent, cols, true, 102400),
		  "INSERT BULK [dbo].[Target] " + body + " WITH (TABLOCK, ROWS_PER_BATCH = 102400)");

	// A temp table is named WITHOUT its schema: `#stage` lives in tempdb, and
	// `[dbo].[#stage]` sends the server looking in the current database.
	Check("local temp is unqualified", BuildInsertBulkSql(local_temp, cols, false, 0), "INSERT BULK [#stage] " + body);
	Check("global temp is unqualified", BuildInsertBulkSql(global_temp, cols, false, 0),
		  "INSERT BULK [##stage] " + body);

	// Zero disables the hint rather than emitting `ROWS_PER_BATCH = 0`, which the
	// server would take as an instruction.
	Check("rows_per_batch 0 emits nothing", BuildInsertBulkSql(permanent, cols, true, 0),
		  "INSERT BULK [dbo].[Target] " + body + " WITH (TABLOCK)");

	if (g_failures == 0) {
		std::cout << "\nAll BuildInsertBulkSql tests passed.\n";
		return 0;
	}
	std::cerr << "\n" << g_failures << " BuildInsertBulkSql test(s) failed.\n";
	return 1;
}
