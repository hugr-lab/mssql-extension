//===----------------------------------------------------------------------===//
//                         DuckDB
//
// mssql_function_docs.hpp
//
// What duckdb_functions() reports for the extension's functions (issue #371)
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace mssql {

//! The documentation of one function as duckdb_functions() shows it. A client
//! connected to the database -- an agent above all -- can learn nothing else
//! about what an extension's functions do: the README is not reachable from SQL.
struct FunctionDoc {
	//! Positional parameter names of the function's LONGEST overload; a shorter
	//! overload takes the prefix it has, which holds because every overload here
	//! only adds optional trailing parameters. Scalar functions only, see below.
	vector<string> parameter_names;
	//! What the function does, in a sentence or two.
	string description;
	//! Runnable calls: a bare expression for a scalar function, a full
	//! `SELECT * FROM f(...)` for a table function (a table function used as an
	//! expression is a binder error).
	vector<string> examples;
	vector<string> categories;
};

//! Registers the overloads as ONE function carrying `doc`. The parameter names
//! go into each overload's signature: on the 2.0 line that is where
//! duckdb_functions() reads them from (FunctionDescription::parameter_names is
//! consulted only when the signature has no names), and the binder matches
//! named arguments against it, so `mssql_exec(context := 'db', sql := '...')`
//! binds -- the names are API and do not change lightly. One registration per
//! function, not one per overload: duckdb_functions() applies a function's ONLY
//! description to every overload, while with several it matches each overload
//! to a description by parameter types, and a description without types then
//! matches no overload that takes arguments.
void RegisterDocumentedFunction(ExtensionLoader &loader, vector<ScalarFunction> overloads, const FunctionDoc &doc);

//! Registers a table function set carrying `doc`. Its positional parameters
//! stay `col0`, `col1`, ... in duckdb_functions() whatever is set: the
//! table-function extractor names them by position and ignores both the
//! signature and the description. Named parameters (`prepared`) are listed by
//! their own names. `doc.parameter_names` is therefore not read here.
void RegisterDocumentedFunction(ExtensionLoader &loader, TableFunctionSet set, const FunctionDoc &doc);

}  // namespace mssql
}  // namespace duckdb
