//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_function_common.hpp
//
// Spec 085: what the backup file functions share -- the path argument (one file, a glob, or a list of
// every family of a media set) and the record form of a catalog's datetime values, converted the one
// way TYPE mssql converts them. [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/value.hpp"

#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

//! The paths of a function's first argument (VARCHAR or LIST(VARCHAR)); NULL, a NULL element or an
//! empty list is an InvalidInputException naming `function`.
vector<std::string> PathsOf(const Value &value, const char *function);

//! A datetime as a record stores it (u32 ticks of 1/300 s, then i32 days since 1900-01-01) as TYPE
//! mssql reads one off the wire (days, then ticks: DateTimeEncoding::ConvertDatetime). NULL TIMESTAMP
//! unless 8 bytes.
Value RecordDatetimeValue(const std::string &record_form);
//! A smalldatetime as a record stores it (u16 minutes, then u16 days) through ConvertSmallDatetime.
//! NULL TIMESTAMP unless 4 bytes.
Value RecordSmallDatetimeValue(const std::string &record_form);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
