//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/uuid_codec.hpp
//
// Uuid family: TDS UNIQUEIDENTIFIER -> DuckDB UUID.
//
// Decode/encode preserve SQL Server's middle-endian byte-order per the
// low-level helper in tds/encoding/guid_encoding.cpp (Data1 LE, Data2 LE,
// Data3 LE, Data4 BE). FormatSqlLiteral produces
// 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx' (lowercase, single-quoted) for
// both Filter and InsertValues contexts — FR-022 byte-identity.
// FormatDdlTypeName returns the literal string "UNIQUEIDENTIFIER" for
// both CreateTable and CtasCreateTable contexts — FR-027/FR-028
// byte-identity. No length, precision, or scale parameters apply.
//===----------------------------------------------------------------------===//

#pragma once

#include "codec/literal_context.hpp"
#include "codec/staging/column_staging.hpp"
#include "codec/type_family.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {

struct UnifiedVectorFormat;

namespace tds {
struct ColumnMetadata;
}  // namespace tds

namespace mssql {
struct BCPColumnMetadata;
struct CTASConfig;
}  // namespace mssql

namespace mssql {
namespace codec {
namespace uuid {

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row);

//! Batch decode of a staged UNIQUEIDENTIFIER column (spec 055 D6).
//!
//! Runs over EVERY row, NULLs included, with no validity test. Staging is
//! positional for fixed-width columns — row N is always at N * 16 — and the byte
//! swap is total over any 16 bytes, so a NULL row produces a value the mask
//! discards. A branch to skip it would cost more than the swap it saves.
void DecodeChunkFromStaging(const staging::ColumnStaging &st, idx_t count, const tds::ColumnMetadata &col, Vector &out);
// W1 (spec 054): format-threaded overload — fmt is built once per column per
// chunk by BCPRowEncoder::EncodeChunk. The (Vector, row) overload below
// wraps it for per-row callers (builds the format per call).
//! Scatter a whole COLUMN at a constant row stride — one call per column. See
//! decimal_codec.hpp's peer for why this is a batch entry point.
void ScatterChunkStrided(uint8_t *dst, size_t stride, idx_t row_begin, idx_t rows, Vector &in,
						 const UnifiedVectorFormat &fmt, const mssql::BCPColumnMetadata &col);

//! Write the PAYLOAD (no length prefix) at a pointer. Spec 057 step 3: the
//! appending overload below is derived from this, so the row path and the
//! columnar scatter share one implementation.
void ScatterToBcp(uint8_t *out, Vector &in, const UnifiedVectorFormat &fmt, idx_t row,
				  const mssql::BCPColumnMetadata &col);

void EncodeToBcp(Vector &in, const UnifiedVectorFormat &fmt, idx_t row, const mssql::BCPColumnMetadata &col,
				 duckdb::vector<uint8_t> &buf);
void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
void EncodeToBcp(const Value &value, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx);
std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig &cfg, DdlContext ctx);
size_t EstimateLiteralSize(const LogicalType &type);

// Public helper: render 16 raw TDS GUID bytes as the canonical lowercase
// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" form (no quotes). Used by the
// issue-#89 VARCHAR-fallback path in TypeConverter::ConvertValue for
// UNIQUEIDENTIFIER TDS payloads landing in a VARCHAR-typed vector.
std::string RenderAsString(const uint8_t *bytes, size_t size);
std::string RenderAsString(const std::vector<uint8_t> &bytes);

}  // namespace uuid
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
