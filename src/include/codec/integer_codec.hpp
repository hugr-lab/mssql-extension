//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/integer_codec.hpp
//
// Integer family: TDS TINYINT/SMALLINT/INT/BIGINT/INTN -> DuckDB
// TINYINT, UTINYINT, SMALLINT, USMALLINT, INTEGER, UINTEGER, BIGINT,
// UBIGINT, HUGEINT.
//
// Special handling within Integer:
//   - HUGEINT BCP encode forwards to codec::decimal::EncodeToBcp with
//     (precision=38, scale=0). HUGEINT DDL forwards to
//     codec::decimal::FormatDdlTypeName for CreateTable, throws for
//     CtasCreateTable (per existing MapLogicalTypeToCTAS behavior).
//   - UBIGINT BCP encode also forwards to codec::decimal::EncodeToBcp
//     with (precision=20, scale=0) because BCP wire has no UNSIGNED
//     BIGINT.
//   - UTINYINT / USMALLINT / UINTEGER widen on encode.
//
// Scan decode (DecodeFromTds) is parameterized by bytes.size() to
// handle TDS_TYPE_INTN's variable max_length (1/2/4/8 bytes ->
// u8/i16/i32/i64).
//
// Phase-2 stub. Function bodies land in Phase 3 (US1 MVP).
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
namespace integer {

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row);
void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, std::vector<uint8_t> &buf);
std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx);
std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig &cfg, DdlContext ctx);
size_t EstimateLiteralSize(const LogicalType &type);

}  // namespace integer
}  // namespace codec
}  // namespace duckdb
