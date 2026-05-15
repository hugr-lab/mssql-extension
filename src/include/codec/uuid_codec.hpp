//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/uuid_codec.hpp
//
// Uuid family: TDS UNIQUEIDENTIFIER -> DuckDB UUID.
//
// Decode/encode preserve SQL Server's middle-endian byte-order per
// guid_encoding.cpp. FormatSqlLiteral produces
// 'XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX' for both contexts.
//
// Phase-2 stub. Function bodies land during Uuid family migration
// (US3 sub-phase 7).
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
namespace uuid {

void DecodeFromTds(const duckdb::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row);
void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx);
std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig &cfg, DdlContext ctx);
size_t EstimateLiteralSize(const LogicalType &type);

}  // namespace uuid
}  // namespace codec
}  // namespace duckdb
