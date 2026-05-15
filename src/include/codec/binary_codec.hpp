//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/binary_codec.hpp
//
// Binary family: TDS BIGBINARY/BIGVARBINARY -> DuckDB BLOB.
//
// EncodeToBcp handles PLP vs non-PLP via col.IsPLPType(). Literal
// format produces 0x<hex> for both Filter and InsertValues contexts.
//
// Phase-2 stub. Function bodies land during Binary family migration
// (US3 sub-phase 5).
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
namespace binary {

void DecodeFromTds(const duckdb::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row);
void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx);
std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig &cfg, DdlContext ctx);
size_t EstimateLiteralSize(const LogicalType &type);

}  // namespace binary
}  // namespace codec
}  // namespace duckdb
