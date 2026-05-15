//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/string_codec.hpp
//
// String family: TDS BIGCHAR / BIGVARCHAR / NCHAR / NVARCHAR / XML <->
// DuckDB VARCHAR. Plus INTERVAL on the DDL/encode/literal side (rendered
// as NVARCHAR(50) per FR-026, post-spec-045).
//
// Behavior parity vs pre-spec-045:
//   - DecodeFromTds mirrors TypeConverter::ConvertString (UTF-16LE decode
//     for NCHAR/NVARCHAR/XML, single-byte passthrough for BIGCHAR/BIGVARCHAR,
//     trailing-space trim for fixed-length CHAR/NCHAR).
//   - EncodeToBcp mirrors BCPRowEncoder::EncodeNVarchar / EncodeNVarcharPLP.
//     **NEW (FR-023, issue #91 client-side fix)**: for a non-PLP NVARCHAR
//     column, the UTF-16LE byte length is validated against col.max_length
//     before encoding. Over-length values throw `InvalidInputException`
//     naming the column AND the observed-vs-allowed UCS-2 code-unit counts
//     (instead of the opaque server-side "Received an invalid column length"
//     error).
//   - FormatSqlLiteral renders N'<escaped>' for both LiteralContext::Filter
//     and LiteralContext::InsertValues (FR-020 — same shape in both).
//     INTERVAL values use the canonical Value::IntervalToString form inside
//     the N-quoted literal (was: filter rendered via default arm; INSERT
//     threw).
//   - FormatDdlTypeName returns identical output for both DdlContext values
//     (FR-027 / FR-028): VARCHAR -> "VARCHAR(MAX)" or "NVARCHAR(MAX)"
//     depending on `cfg.text_type`; INTERVAL -> "NVARCHAR(50)" per FR-026
//     (replaces pre-spec-045 NVARCHAR(100) in CreateTable and the
//     NotImplementedException thrown by CtasCreateTable).
//   - EstimateLiteralSize returns the per-type wrapper overhead for VARCHAR
//     and INTERVAL (callers add the value-specific 2*GetString().size()
//     part — see MSSQLValueSerializer::EstimateSerializedSize).
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
namespace string {

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row);
void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
// Overload for the single-Value path (BCPRowEncoder::EncodeValue public API).
void EncodeToBcp(const Value &value, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx);
std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig &cfg, DdlContext ctx);
size_t EstimateLiteralSize(const LogicalType &type);

// Helper used by callers that need to count UTF-16LE bytes / UCS-2 code
// units without re-running the simdutf encode pass twice. Exposed because
// `MSSQLValueSerializer::EstimateSerializedSize` benefits from the same
// computation. Implementation forwards to `encoding::Utf16LEByteLength`.
size_t Utf16ByteLength(const std::string &utf8);

// SQL Server N'...' literal-escape: doubles every `'` so the wrapped value
// is a syntactically valid N-string. Used by `FormatSqlLiteral` and by the
// LIKE-pattern emitter in `FilterEncoder` (which builds N'%pattern%' shells
// from user input). Lives here because escaping single quotes is
// String-family text logic.
std::string EscapeSqlSingleQuotes(const std::string &str);

}  // namespace string
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
