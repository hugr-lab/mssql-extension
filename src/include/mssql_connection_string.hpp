#pragma once

//===----------------------------------------------------------------------===//
// INTERNAL to the connection info (mssql_connection_info.cpp): a connection
// string's or URI's parameters, read into one map under their canonical names.
// The public interface is MSSQLConnectionInfo (mssql_storage.hpp).
//===----------------------------------------------------------------------===//

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/string.hpp"

namespace duckdb {

//! Whether `str` is the URI form (mssql://...).
bool IsUriFormatImpl(const string &str);

//! The parameters of either form: a URI's (mssql://user:password@host:port/database?param=value), or an
//! ADO.NET connection string's (Server=host,port;Database=db;...), each key under its canonical name.
case_insensitive_map_t<string> ParseConnectionParameters(const string &connection_string);

}  // namespace duckdb
