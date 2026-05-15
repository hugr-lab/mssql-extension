//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/literal_format.hpp  (CONTRACT — to be placed at src/include/codec/)
//
// Shared T-SQL literal formatter consumed by:
//   - src/table_scan/filter_encoder.cpp:ValueToSQLLiteral
//       -> codec::FormatSqlLiteral(value, type, LiteralContext::Filter)
//   - src/dml/insert/mssql_value_serializer.cpp:Serialize
//       -> codec::FormatSqlLiteral(value, type, LiteralContext::InsertValues)
//
// Body: switch (FamilyFromLogicalType(type)) with one arm per family,
// each calling codec::<family>::FormatSqlLiteral(v, type, ctx). The
// shared module owns NO per-type logic; it is a pure dispatcher.
//===----------------------------------------------------------------------===//

#pragma once

#include "codec/literal_context.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"

#include <string>

namespace duckdb {
namespace codec {

// Format a DuckDB Value as a T-SQL literal in the given context.
// NULL is handled at this top level and renders as "NULL" for both
// contexts. Per-family modules see only non-null Values.
std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx);

// Estimate the formatted-literal size in bytes (upper bound). Used by
// callers that pre-size their output buffers. Dispatches to per-family
// codec::<family>::EstimateLiteralSize.
size_t EstimateLiteralSize(const LogicalType &type);

}  // namespace codec
}  // namespace duckdb
