//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/money_codec.hpp
//
// Money family: TDS MONEY/SMALLMONEY/MONEYN. **Scan-decode only.**
// DuckDB has no MONEY type — encode/literal/DDL paths route values
// through the Decimal family. Only DecodeFromTds will be defined;
// the other 3 ops are declared here for interface uniformity but
// left intentionally undefined (linker-fenced).
//
// Phase-2 stub. Function bodies (only DecodeFromTds) land during
// Money family migration (US3 sub-phase 4).
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

namespace mssql {
namespace codec {
namespace money {

void DecodeFromTds(const duckdb::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row);
void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx);
std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig &cfg, DdlContext ctx);
size_t EstimateLiteralSize(const LogicalType &type);

}  // namespace money
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
