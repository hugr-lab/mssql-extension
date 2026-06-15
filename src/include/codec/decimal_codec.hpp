//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/decimal_codec.hpp
//
// Decimal family: TDS DECIMAL/NUMERIC -> DuckDB DECIMAL.
//
// EncodeToBcp dispatches internally on PhysicalType (INT16/INT32/
// INT64/INT128) per DuckDB's decimal storage. FormatSqlLiteral uses
// GetValueUnsafe<T>() per PhysicalType for precision preservation.
// HUGEINT (owned by Integer family) routes here for BCP / literal /
// DDL (FR-025).
//
// RenderAsString is a public helper used by both FormatSqlLiteral
// (literal path) and the issue-#89 VARCHAR-fallback in
// TypeConverter::ConvertValue dispatcher.
//===----------------------------------------------------------------------===//

#pragma once

#include "codec/literal_context.hpp"
#include "codec/type_family.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {

namespace tds {
struct ColumnMetadata;
}  // namespace tds

namespace mssql {
struct BCPColumnMetadata;
struct CTASConfig;
}  // namespace mssql

namespace mssql {
namespace codec {
namespace decimal {

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row);
void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
void EncodeToBcp(const Value &value, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx);
std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig &cfg, DdlContext ctx);
size_t EstimateLiteralSize(const LogicalType &type);

// Render a TDS DECIMAL/NUMERIC wire payload (sign + LE mantissa) as a
// fixed-point string. Reused by the VARCHAR-fallback path in the
// dispatcher (issue #89) — same output as FormatSqlLiteral on the
// equivalent Value.
std::string RenderAsString(const std::vector<uint8_t> &bytes, uint8_t precision, uint8_t scale);

// Render a SQL-Server MONEY (8-byte) or SMALLMONEY (4-byte) wire
// payload as a fixed-point string with 4 decimal places. Used by the
// VARCHAR-fallback for MONEY/SMALLMONEY/MONEYN columns.
std::string RenderMoneyAsString(const std::vector<uint8_t> &bytes);

}  // namespace decimal
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
