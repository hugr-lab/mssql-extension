//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/float_codec.hpp
//
// Float family: TDS REAL / FLOAT / FLOATN <-> DuckDB FLOAT / DOUBLE.
//
// Namespace name `float_family` is used because `float` is a C++
// keyword and cannot be a namespace identifier.
//
// Behavior parity vs pre-spec-045 baseline:
//   - DecodeFromTds mirrors TypeConverter::ConvertFloat (size-dispatched
//     on bytes.size(): 4 -> float, 8 -> double).
//   - EncodeToBcp mirrors BCPRowEncoder::EncodeFloat / EncodeDouble
//     (length-prefixed LE IEEE 754).
//   - FormatSqlLiteral produces output that is BYTE-IDENTICAL across
//     LiteralContext::Filter and LiteralContext::InsertValues (FR-020 (b)).
//     The unified form follows the pre-spec-045 InsertValues semantics
//     (std::ostringstream with setprecision(9) for float / setprecision(17)
//     for double, `.0` suffix on integer-valued floats, client-side
//     NaN/Inf rejection via InvalidInputException). Pre-spec-045 the
//     Filter context used `value.ToString()` which produced different
//     rendering and silently allowed NaN/Inf strings into the WHERE
//     clause (which SQL Server then rejected with an opaque parse
//     error); the unified form catches those client-side, mirroring
//     spec 045 Phase 5's FR-023 NVARCHAR-length hardening.
//   - FormatDdlTypeName is byte-identical across both DdlContext values
//     (FR-027 / FR-028): FLOAT -> "REAL", DOUBLE -> "FLOAT".
//     `cfg.text_type` is irrelevant for Float DDL.
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
class BCPColumnMetadata;
struct CTASConfig;
}  // namespace mssql

namespace mssql {
namespace codec {
namespace float_family {

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row);
void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
// Overload for the single-Value path (BCPRowEncoder::EncodeValue public API).
void EncodeToBcp(const Value &value, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx);
std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig &cfg, DdlContext ctx);
size_t EstimateLiteralSize(const LogicalType &type);

}  // namespace float_family
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
