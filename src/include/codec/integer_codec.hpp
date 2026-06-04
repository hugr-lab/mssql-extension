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
//   - HUGEINT and UHUGEINT BCP encode are deferred to the Decimal family
//     migration (spec 045 Phase 6); arms throw NotImplementedException
//     for now, preserving the pre-spec-045 default-arm behavior.
//   - UBIGINT BCP encode delegates to BCPRowEncoder::EncodeDecimal with
//     (precision=20, scale=0) because BCP wire has no UNSIGNED BIGINT.
//   - UTINYINT / USMALLINT / UINTEGER widen on encode.
//   - FormatSqlLiteral output is identical for LiteralContext::Filter and
//     LiteralContext::InsertValues (FR-020 (b) — HUGEINT correctness fix).
//   - FormatDdlTypeName output is byte-identical for DdlContext::CreateTable
//     and DdlContext::CtasCreateTable (FR-025 / FR-028 — DDL unification).
//     HUGEINT and UHUGEINT both map to DECIMAL(38,0) in both contexts.
//
// Scan decode (DecodeFromTds) is parameterized by bytes.size() to
// handle TDS_TYPE_INTN's variable max_length (1/2/4/8 bytes ->
// u8/i16/i32/i64).
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
namespace integer {

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row);
void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
// Overload for the single-Value path (BCPRowEncoder::EncodeValue public API).
void EncodeToBcp(const Value &value, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx);
std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig &cfg, DdlContext ctx);
size_t EstimateLiteralSize(const LogicalType &type);

}  // namespace integer
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
