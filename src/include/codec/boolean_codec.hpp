//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/boolean_codec.hpp
//
// Boolean family: TDS BIT / BITN <-> DuckDB BOOLEAN.
//
// Behavior parity vs pre-spec-045:
//   - DecodeFromTds mirrors TypeConverter::ConvertBoolean
//     (`!bytes.empty() && bytes[0] != 0`).
//   - EncodeToBcp mirrors BCPRowEncoder::EncodeBit (1-byte length prefix
//     0x01 + 1-byte value 0x00 or 0x01).
//   - FormatSqlLiteral renders "1" for true and "0" for false. The output
//     is identical for LiteralContext::Filter and LiteralContext::InsertValues
//     (FR-020 (b)).
//   - FormatDdlTypeName returns "BIT" — byte-identical for both DdlContext
//     values (FR-027 / FR-028). `cfg.text_type` is irrelevant for Boolean.
//   - EstimateLiteralSize returns 1 (single-character literal).
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
struct ColumnMetadata;
}  // namespace tds

namespace mssql {
struct BCPColumnMetadata;
struct CTASConfig;
}  // namespace mssql

namespace mssql {
namespace codec {
namespace boolean {

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row);
void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
// Overload for the single-Value path (BCPRowEncoder::EncodeValue public API).
void EncodeToBcp(const Value &value, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx);
std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig &cfg, DdlContext ctx);
size_t EstimateLiteralSize(const LogicalType &type);

}  // namespace boolean
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
