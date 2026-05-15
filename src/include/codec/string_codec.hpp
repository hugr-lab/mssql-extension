//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/string_codec.hpp
//
// String family: TDS BIGCHAR/BIGVARCHAR/NCHAR/NVARCHAR/XML -> DuckDB
// VARCHAR. Plus INTERVAL on the DDL side (renders as NVARCHAR(100)
// per current MapTypeToSQLServer behavior; no scan/encode/literal
// path).
//
// EncodeToBcp owns the NVARCHAR length validation fix for issue #91
// (FR-023). Pre-encode, the UTF-16LE byte length is checked against
// the column's max_length; over-length values throw a clear
// InvalidInputException naming the column and observed-vs-allowed
// UCS-2 code-unit counts.
//
// Phase-2 stub. Function bodies land during US5 (Phase 5).
//===----------------------------------------------------------------------===//

#pragma once

#include "codec/literal_context.hpp"
#include "codec/type_family.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {

namespace tds {
class ColumnMetadata;
}  // namespace tds

namespace mssql {
class BCPColumnMetadata;
struct CTASConfig;
}  // namespace mssql

namespace codec {
namespace string {

void DecodeFromTds(const duckdb::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row);
void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx);
std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig &cfg, DdlContext ctx);
size_t EstimateLiteralSize(const LogicalType &type);

}  // namespace string
}  // namespace codec
}  // namespace duckdb
