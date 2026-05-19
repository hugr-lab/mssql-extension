//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/binary_codec.hpp
//
// Binary family: TDS BIGBINARY/BIGVARBINARY -> DuckDB BLOB.
//
// Also services DuckDB GEOMETRY: type_family.cpp routes
// LogicalTypeId::GEOMETRY to TypeFamily::Binary, so geometry/geography
// columns (mapped to LogicalType::GEOMETRY() in mssql_column_info) reuse
// the literal/DDL/encode paths here. The decode path is shared by
// physical-type storage: BLOB and GEOMETRY both use string_t, so
// DecodeFromTds writes correctly into either via AddStringOrBlob.
//
// EncodeToBcp handles PLP vs non-PLP via col.IsPLPType(). Literal
// format produces 0x<UPPERHEX> for both Filter and InsertValues contexts.
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
namespace binary {

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row);
void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
void EncodeToBcp(const Value &value, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx);
std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig &cfg, DdlContext ctx);
size_t EstimateLiteralSize(const LogicalType &type);

// Public helper: render raw bytes as the canonical 0x<UPPERHEX> literal text.
// Used by the issue-#89 VARCHAR-fallback path in TypeConverter::ConvertValue
// for binary TDS payloads landing in a VARCHAR-typed vector.
std::string RenderAsString(const std::vector<uint8_t> &bytes);

}  // namespace binary
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
