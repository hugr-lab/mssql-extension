//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/decimal_codec.hpp
//
// Decimal family: TDS DECIMAL/NUMERIC -> DuckDB DECIMAL.
//
// EncodeToBcp dispatches internally on PhysicalType (INT16/INT32/INT64
// /INT128) per DuckDB's decimal storage. FormatSqlLiteral uses
// GetValueUnsafe<T>() per PhysicalType for precision preservation.
// HUGEINT (owned by Integer family) routes here for BCP/DDL.
//
// Phase-2 stub. Function bodies land during Decimal family migration
// (US3 sub-phase 3).
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
namespace decimal {

void DecodeFromTds(const duckdb::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row);
void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx);
std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig &cfg, DdlContext ctx);
size_t EstimateLiteralSize(const LogicalType &type);

}  // namespace decimal
}  // namespace codec
}  // namespace duckdb
