//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/binary_codec.hpp
//
// Binary family: TDS BIGBINARY/BIGVARBINARY -> DuckDB BLOB.
//
// Also services DuckDB GEOMETRY: type_family.cpp routes
// LogicalTypeId::GEOMETRY to TypeFamily::Binary, so geometry/geography
// columns (mapped to LogicalType::GEOMETRY() in mssql_column_info) reuse
// the literal/DDL/encode paths here. The decode path is shared by
// physical-type storage: BLOB and GEOMETRY both use string_t, so
// DecodeFromTds writes correctly into either via AddStringOrBlob.
//
// EncodeToBcp handles PLP vs non-PLP via col.IsPLPType(). Literal
// format produces 0x<UPPERHEX> for both Filter and InsertValues contexts.
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
namespace binary {

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row);

//! Batch decode of a staged column whose wire bytes ARE the value (spec 055 D6):
//! VARBINARY, and single-byte CHAR/VARCHAR, whose collation handling lives in
//! the scan's SELECT list (spec 026) rather than here.
//!
//! One allocation and one copy for the whole column, in place of one of each per
//! value. Fixed-length CHAR is padded to its declared width by SQL Server and
//! the trailing spaces are stripped — that rule is a property of the TDS type,
//! so it is derived here from `col` rather than passed in.
void DecodeChunkFromStaging(const staging::ColumnStaging &st, idx_t count, const tds::ColumnMetadata &col, Vector &out);
// W1 (spec 054): format-threaded overload — fmt is built once per column per
// chunk by BCPRowEncoder::EncodeChunk. The (Vector, row) overload below
// wraps it for per-row callers (builds the format per call).
void EncodeToBcp(Vector &in, const UnifiedVectorFormat &fmt, idx_t row, const mssql::BCPColumnMetadata &col,
				 duckdb::vector<uint8_t> &buf);
void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
void EncodeToBcp(const Value &value, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx);
std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig &cfg, DdlContext ctx);
size_t EstimateLiteralSize(const LogicalType &type);

// Public helper: render raw bytes as the canonical 0x<UPPERHEX> literal text.
// Used by the issue-#89 VARCHAR-fallback path in TypeConverter::ConvertValue
// for binary TDS payloads landing in a VARCHAR-typed vector.
std::string RenderAsString(const uint8_t *bytes, size_t size);
std::string RenderAsString(const std::vector<uint8_t> &bytes);

}  // namespace binary
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
