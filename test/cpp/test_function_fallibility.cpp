// test/cpp/test_function_fallibility.cpp
//
// Spec 069 cluster F: a scalar function that throws at execution time must
// declare FunctionErrors::CAN_THROW_RUNTIME_ERROR (ScalarFunction::SetFallible).
// DuckDB 2.0 converts the throw of an undeclared function into
//
//   INTERNAL Error: Attempting to execute ... the function is not marked as
//   fallible
//
// which turns every honest server-side error (login failed, unknown context,
// T-SQL syntax error) into an internal error the user cannot act on.
//
// The contract is invisible to the compiler. Spec 069 names
// `test/sql/tds_connection/ping.test` as the one test that caught it during the
// migration -- that test and the mssql_open / mssql_close / mssql_ping
// functions it exercised no longer exist in the tree. Of the registrations that
// remain, the azure (2), kerberos (3) and winsspi (3) test functions all sit
// behind `require azure` / `require-env` skips, so in CI eight of thirteen are
// never executed at all and a missing SetFallible() on one of them ships.
//
// So check it structurally instead of by execution: walk the catalog, take
// every scalar function the extension registered, and read GetErrorMode() off
// each overload. duckdb_functions() has no fallibility column (checked against
// the pinned 2.0 submodule), which is why this is C++ and not a .test file.
//
// Sibling of test/sql/mssql_function_docs.test, which fails for a function
// registered without documentation. This one fails for a function registered
// without fallibility -- including a function that does not exist yet.
//
// No SQL Server: registration happens at database startup.

#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb_static_extension.h"

using namespace duckdb;

// A statically linked extension is registered with the engine through its
// generated describe function, and DuckDB 2.0 only loads what was registered
// BEFORE the database was opened (extension/loader/static_extension_loader.c.in
// -- the shell gets the generated loader, a standalone test does not, so the
// archive alone registers nothing).
extern "C" int32_t duckdb_extension_mssql_describe(duckdb_extension_descriptor *descriptor);

namespace {

// Functions that cannot throw at execution time, and therefore must NOT be
// marked fallible: marking an infallible function costs DuckDB the optimisations
// the flag turns off. Keep this list short and justified -- the default answer
// for anything that reaches a server, reads a secret or validates an argument
// is `SetFallible()`, not an entry here.
//
//   mssql_version(): returns the MSSQL_VERSION compile definition. No argument,
//                    no connection, no failure mode.
const std::set<std::string> INFALLIBLE = {"mssql_version"};

// A floor on what the walk must find, so a build whose extension did not
// register (or a catalog walk that silently visits nothing) fails here instead
// of passing with zero checks. 13 scalar functions carry SetFallible() today
// plus mssql_version; the floor is deliberately below that so adding a function
// never requires touching this number.
constexpr size_t MIN_FUNCTIONS = 10;

}  // namespace

int main() {
	size_t checked = 0;
	size_t failures = 0;
	std::vector<std::string> found;

	if (duckdb_register_static_extension(duckdb_extension_mssql_describe) != 0) {
		std::cerr << "FAIL: could not register the statically linked mssql extension\n";
		return 1;
	}

	DuckDB db(nullptr);
	Connection con(db);
	con.BeginTransaction();
	auto &context = *con.context;

	// The extension's functions live in the system catalog, registered at
	// database startup by the statically linked extension loader.
	auto &system_catalog = Catalog::GetSystemCatalog(context);
	system_catalog.ScanSchemas(context, [&](SchemaCatalogEntry &schema) {
		schema.Scan(context, CatalogType::SCALAR_FUNCTION_ENTRY, [&](CatalogEntry &entry) {
			if (!entry.name.StartsWith("mssql_")) {
				return;
			}
			auto &function_entry = entry.Cast<ScalarFunctionCatalogEntry>();
			const std::string name = entry.name.GetIdentifierName();
			found.push_back(name);
			const bool want_fallible = INFALLIBLE.find(name) == INFALLIBLE.end();

			// Every overload separately: the overloads of one name are
			// registered from distinct ScalarFunction values (mssql_exec_params
			// builds two in a loop, mssql_invalidate_cache three), so one can
			// carry the flag while its sibling does not.
			idx_t overload = 0;
			for (auto &function : function_entry.functions.functions) {
				const bool fallible = function->GetErrorMode() == FunctionErrors::CAN_THROW_RUNTIME_ERROR;
				checked++;
				if (fallible == want_fallible) {
					overload++;
					continue;
				}
				failures++;
				if (want_fallible) {
					std::cerr << "FAIL: " << name << " overload " << overload
							  << " is not marked fallible: add SetFallible() where it is registered, or list it in "
								 "INFALLIBLE here with the reason it cannot throw.\n";
				} else {
					std::cerr << "FAIL: " << name << " overload " << overload
							  << " is marked fallible but INFALLIBLE here says it cannot throw: drop SetFallible() or "
								 "remove it from that list.\n";
				}
				overload++;
			}
		});
	});
	con.Commit();

	if (found.size() < MIN_FUNCTIONS) {
		std::cerr << "FAIL: found only " << found.size() << " mssql_* scalar functions in the catalog (expected at "
				  << "least " << MIN_FUNCTIONS << "): the extension did not register, so nothing was checked.\n";
		failures++;
	}

	if (failures > 0) {
		std::cerr << failures << " failure(s) over " << checked << " overload(s) of " << found.size()
				  << " function(s)\n";
		return 1;
	}
	std::cout << "test_function_fallibility: " << checked << " overload(s) of " << found.size()
			  << " mssql_* scalar function(s) declare their error mode correctly\n";
	return 0;
}
