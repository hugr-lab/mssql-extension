//===----------------------------------------------------------------------===//
// mssql_compat.hpp — DuckDB API shape adapters (spec 051)
//
// Lets a single source tree compile against multiple DuckDB SHAs by hiding
// header relocations and signature shape changes behind preprocessor guards.
// Detection uses __has_include on a target-SHA-only header
// (duckdb/common/vector/flat_vector.hpp); features that landed together in
// DuckDB main share that sentinel.
//
// Migrations covered (spec 051):
//   M2 — FlatVector header moved to <duckdb/common/vector/flat_vector.hpp>.
//   M4 — bind_scalar_function_t signature changed to single-arg
//        (BindScalarFunctionInput &). The MSSQL_BIND_SCALAR_SIG macro hides
//        the per-SHA signature; MSSQL_BIND_SCALAR_PROLOGUE unpacks the new
//        struct on the new SHA and is a no-op on the legacy SHA.
//
// M1 (ClientContext::IsInterrupted) and M3 (ScalarFunction::SetNullHandling)
// exist on both currently-supported SHAs and are renamed in-place at call
// sites, not via this header.
//===----------------------------------------------------------------------===//

#pragma once

#if __has_include(<duckdb/common/vector/flat_vector.hpp>)
#include <duckdb/common/vector/flat_vector.hpp>
#define MSSQL_DUCKDB_HAS_NEW_BIND_INPUT 1
#else
#include <duckdb/common/types/vector.hpp>
#endif

#ifdef MSSQL_DUCKDB_HAS_NEW_BIND_INPUT

#define MSSQL_BIND_SCALAR_SIG(name)                                                                                    \
	static duckdb::unique_ptr<duckdb::FunctionData> name(duckdb::BindScalarFunctionInput &input)

#define MSSQL_BIND_SCALAR_PROLOGUE                                                                                     \
	auto &context = input.GetClientContext();                                                                          \
	auto &arguments = input.GetArguments();                                                                            \
	(void)context;                                                                                                     \
	(void)arguments;

#else

#define MSSQL_BIND_SCALAR_SIG(name)                                                                                    \
	static duckdb::unique_ptr<duckdb::FunctionData> name(                                                              \
	    duckdb::ClientContext &context, duckdb::ScalarFunction & /*bound_function*/,                                   \
	    duckdb::vector<duckdb::unique_ptr<duckdb::Expression>> &arguments)

#define MSSQL_BIND_SCALAR_PROLOGUE /* no-op on legacy SHA */

#endif
