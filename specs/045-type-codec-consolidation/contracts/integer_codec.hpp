//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/integer_codec.hpp  (CONTRACT — to be placed at src/include/codec/)
//
// CANONICAL EXAMPLE of a per-family codec module header.
//
// All 9 family headers follow this shape — only the namespace and the
// set of types they own changes.
//
// Integer family owns: TINYINT, UTINYINT, SMALLINT, USMALLINT, INTEGER,
// UINTEGER, BIGINT, UBIGINT, HUGEINT.
//
// Special handling within Integer:
//   - HUGEINT BCP encode forwards to codec::decimal::EncodeToBcp with
//     (precision=38, scale=0). HUGEINT DDL forwards to
//     codec::decimal::FormatDdlTypeName in BOTH DdlContext values
//     (post-spec-045 unification: both contexts return
//     "DECIMAL(38,0)"; CtasCreateTable no longer throws). Runtime
//     overflow at BCP encode emits a warning and writes the
//     saturated value (FR-025).
//   - UBIGINT BCP encode also forwards to codec::decimal::EncodeToBcp
//     with (precision=20, scale=0) because BCP wire has no UNSIGNED
//     BIGINT.
//   - UTINYINT/USMALLINT/UINTEGER widen on encode (UINTEGER -> Int64 etc.).
//
// Scan decode (DecodeFromTds) is parameterized by value.size() to handle
// TDS_TYPE_INTN's variable max_length (1/2/4/8 bytes -> u8/i16/i32/i64).
//===----------------------------------------------------------------------===//

#pragma once

#include "codec/literal_context.hpp"
#include "codec/type_family.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector.hpp"

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

// Scan decode (TDS wire bytes -> DuckDB Vector slot).
// Handles TDS_TYPE_TINYINT/_SMALLINT/_INT/_BIGINT/_INTN. INTN dispatches
// on `bytes.size()` internally (1/2/4/8 -> u8/i16/i32/i64).
void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row);

// BCP encode (DuckDB Vector slot -> TDS BCP wire bytes).
// Dispatches on col.duckdb_type.id() for the Integer-family LogicalTypes.
// UBIGINT and HUGEINT forward to codec::decimal::EncodeToBcp.
void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, std::vector<uint8_t> &buf);

// T-SQL literal text (used by both Filter and InsertValues contexts).
// `ctx` is currently unused for Integer (both contexts agree
// post-spec-045 — HUGEINT literal correctness fix unifies the output)
// but the parameter is kept for interface uniformity across families.
std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx);

// T-SQL type name (used by both DDL and CTAS DDL contexts).
// HUGEINT in CtasCreateTable context throws NotImplementedException
// (preserves current MapLogicalTypeToCTAS behavior).
std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig &cfg, DdlContext ctx);

// Upper bound on FormatSqlLiteral output size in bytes.
// e.g. INT64_MIN as decimal = "-9223372036854775808" = 20 bytes; this
// function returns the constant 20 for BIGINT.
size_t EstimateLiteralSize(const LogicalType &type);

}  // namespace integer
}  // namespace codec
}  // namespace duckdb
