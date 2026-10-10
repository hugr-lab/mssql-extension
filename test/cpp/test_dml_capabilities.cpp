// test/cpp/test_dml_capabilities.cpp
//
// Unit tests for DmlCapabilities::Resolve (spec 080 D0): which DML forms each
// platform takes, and when rung 3 may use IS NOT DISTINCT FROM.
//
// Header-only, no server, no linking. A wrong answer here is silent on the
// server side -- a form a platform lacks fails the statement, a missing
// operator fails it on an old server -- so the table is pinned case by case.
//
// Run:
//   make test-dml-capabilities

#include <iostream>

#include "dml/mssql_dml_capabilities.hpp"

using namespace duckdb::mssql;

static int g_failures = 0;

#define CHECK(cond, what)                                                                              \
	do {                                                                                               \
		if (!(cond)) {                                                                                 \
			std::cerr << "  FAIL: " << what << " (" << #cond << ") at line " << __LINE__ << std::endl; \
			g_failures++;                                                                              \
		}                                                                                              \
	} while (0)

int main() {
	// SQL Server 2022: every form, the operator.
	auto s22 = DmlCapabilities::Resolve(DmlPlatform::SqlServer, 3, 16);
	CHECK(s22.output_into_table && s22.update_from_join && s22.merge && s22.stage_bulk && s22.keyless_dml,
		  "SQL Server takes every form");
	CHECK(s22.null_safe_operator, "SQL Server 2022 has IS NOT DISTINCT FROM");
	CHECK(!s22.IsSynapse(), "SQL Server is not Synapse");

	// SQL Server 2019: no operator.
	auto s19 = DmlCapabilities::Resolve(DmlPlatform::SqlServer, 3, 15);
	CHECK(!s19.null_safe_operator, "SQL Server 2019 takes the INTERSECT form");
	CHECK(s19.update_from_join && s19.stage_bulk, "SQL Server 2019 still takes the join and the stage");

	// Azure SQL Database (5) / Managed Instance (8) report version 12.
	CHECK(DmlCapabilities::Resolve(DmlPlatform::SqlServer, 5, 12).null_safe_operator, "Azure SQL DB has the operator");
	CHECK(DmlCapabilities::Resolve(DmlPlatform::SqlServer, 8, 12).null_safe_operator, "Managed Instance has it");

	// Unread properties: the INTERSECT form, correct everywhere.
	auto unknown = DmlCapabilities::Resolve(DmlPlatform::SqlServer, -1, -1);
	CHECK(!unknown.null_safe_operator, "unread properties take the INTERSECT form");
	CHECK(unknown.update_from_join && unknown.keyless_dml, "unread properties keep the SQL Server forms");

	// Fabric: MERGE only, until fabric-probe/ settles the rest. EngineEdition 11
	// is what Synapse serverless reports too; it must not change the answer.
	auto fabric = DmlCapabilities::Resolve(DmlPlatform::Fabric, 11, 12);
	CHECK(fabric.merge, "Fabric has MERGE (GA)");
	CHECK(!fabric.update_from_join, "Fabric documents no UPDATE/DELETE FROM … JOIN");
	CHECK(!fabric.output_into_table && !fabric.stage_bulk && !fabric.null_safe_operator,
		  "Fabric's probe rows are off until the probe");
	CHECK(!fabric.keyless_dml, "keyless DML on Fabric waits for p18");
	CHECK(!fabric.IsSynapse(), "Fabric is not Synapse");
	CHECK(!DmlCapabilities::Resolve(DmlPlatform::Fabric, 5, 16).null_safe_operator,
		  "Fabric never takes the operator from the edition or the version");

	// Synapse: nothing.
	auto syn = DmlCapabilities::Resolve(DmlPlatform::Synapse, 6, 10);
	CHECK(syn.IsSynapse(), "Synapse is Synapse");
	CHECK(!syn.output_into_table && !syn.update_from_join && !syn.merge && !syn.stage_bulk && !syn.null_safe_operator &&
			  !syn.keyless_dml,
		  "Synapse takes none of the new forms");

	// A dedicated pool under a non-Synapse host name (a former SQL DW at
	// *.database.windows.net, a private-link alias): EngineEdition 6 decides.
	auto dw = DmlCapabilities::Resolve(DmlPlatform::SqlServer, 6, 10);
	CHECK(dw.IsSynapse(), "EngineEdition 6 is Synapse dedicated whatever the host");
	CHECK(!dw.keyless_dml && !dw.stage_bulk, "…and takes none of the new forms");
	CHECK(!DmlCapabilities::Resolve(DmlPlatform::Fabric, 6, 12).IsSynapse(), "the host test's Fabric stands");

	if (g_failures) {
		std::cerr << g_failures << " failure(s)" << std::endl;
		return 1;
	}
	std::cout << "DmlCapabilities: all cases passed" << std::endl;
	return 0;
}
