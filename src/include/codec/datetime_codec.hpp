//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/datetime_codec.hpp
//
// DateTime family: TDS DATE/TIME/DATETIME/SMALLDATETIME/DATETIME2/
// DATETIMEN/DATETIMEOFFSET -> DuckDB DATE, TIME, TIMESTAMP,
// TIMESTAMP_NS, TIMESTAMP_MS, TIMESTAMP_SEC, TIMESTAMP_TZ.
//
// FormatDdlTypeName diverges between CreateTable (DATETIME2(6)) and
// CtasCreateTable (DATETIME2(7)) — see research.md R8.
//
// Phase-2 stub. Function bodies land during DateTime family migration
// (US3 sub-phase 6).
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
namespace datetime {

void DecodeFromTds(const duckdb::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row);
void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx);
std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig &cfg, DdlContext ctx);
size_t EstimateLiteralSize(const LogicalType &type);

}  // namespace datetime
}  // namespace codec
}  // namespace duckdb
