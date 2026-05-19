//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/uuid_codec.hpp
//
// Uuid family: TDS UNIQUEIDENTIFIER -> DuckDB UUID.
//
// Decode/encode preserve SQL Server's middle-endian byte-order per the
// low-level helper in tds/encoding/guid_encoding.cpp (Data1 LE, Data2 LE,
// Data3 LE, Data4 BE). FormatSqlLiteral produces
// 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx' (lowercase, single-quoted) for
// both Filter and InsertValues contexts — FR-022 byte-identity.
// FormatDdlTypeName returns the literal string "UNIQUEIDENTIFIER" for
// both CreateTable and CtasCreateTable contexts — FR-027/FR-028
// byte-identity. No length, precision, or scale parameters apply.
//===----------------------------------------------------------------------===//

#pragma once

#include "codec/literal_context.hpp"
#include "codec/type_family.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {

namespace tds {
struct ColumnMetadata;
}  // namespace tds

namespace mssql {
class BCPColumnMetadata;
struct CTASConfig;
}  // namespace mssql

namespace mssql {
namespace codec {
namespace uuid {

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row);
void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
void EncodeToBcp(const Value &value, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx);
std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig &cfg, DdlContext ctx);
size_t EstimateLiteralSize(const LogicalType &type);

// Public helper: render 16 raw TDS GUID bytes as the canonical lowercase
// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" form (no quotes). Used by the
// issue-#89 VARCHAR-fallback path in TypeConverter::ConvertValue for
// UNIQUEIDENTIFIER TDS payloads landing in a VARCHAR-typed vector.
std::string RenderAsString(const std::vector<uint8_t> &bytes);

}  // namespace uuid
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
