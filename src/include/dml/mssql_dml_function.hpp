//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// dml/mssql_dml_function.hpp
//
// Spec 080 D1: mssql_dml, the vehicle a pushed UPDATE / DELETE / INSERT ...
// SELECT runs through. The remote-pushdown rewriter replaces the DML node with
// `SELECT * FROM mssql_dml(...)`; the function runs the T-SQL statement once,
// at InitGlobal, on the statement's connection, and returns the server's
// count as one BIGINT named Count -- the shape of a native DML's result.
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {

class ExtensionLoader;

void RegisterMSSQLDmlFunction(ExtensionLoader &loader);

}  // namespace duckdb
