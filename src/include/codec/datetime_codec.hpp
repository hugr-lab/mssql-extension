//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/datetime_codec.hpp
//
// DateTime family — covers every non-UDT SQL Server temporal type:
//
//   Wire (TDS COLMETADATA → ROW bytes):
//     TDS_TYPE_DATE           (0x28) 3 bytes
//     TDS_TYPE_TIME           (0x29) 3 / 4 / 5 bytes (scale 0-2 / 3-4 / 5-7)
//     TDS_TYPE_DATETIME       (0x3D) 8 bytes
//     TDS_TYPE_SMALLDATETIME  (0x3A) 4 bytes
//     TDS_TYPE_DATETIME2      (0x2A) 6 / 7 / 8 bytes
//     TDS_TYPE_DATETIMEN      (0x6F) 4 (smalldatetime) or 8 (datetime)
//     TDS_TYPE_DATETIMEOFFSET (0x2B) 8 / 9 / 10 bytes
//
//   DuckDB peer:
//     date_t      ←→ DATE
//     dtime_t     ←→ TIME (μs since midnight, scale-independent in DuckDB)
//     timestamp_t ←→ TIMESTAMP / TIMESTAMP_MS / TIMESTAMP_NS / TIMESTAMP_SEC
//     timestamp_t ←→ TIMESTAMP_TZ (UTC; offset reads as 0 round-trip)
//
//   FR notes:
//     - FR-022: FormatSqlLiteral output is byte-identical for
//       LiteralContext::Filter and LiteralContext::InsertValues. Both
//       contexts emit the canonical CAST literal forms with scale-7
//       fractional seconds.
//     - FR-027/FR-028: FormatDdlTypeName is byte-identical in both
//       DdlContext values. New TIMESTAMP_MS / TIMESTAMP_NS / TIMESTAMP_SEC
//       arms map to DATETIME2(3) / DATETIME2(7) / DATETIME2(0).
//     - RenderAsString: helper used by issue-#89 fallback path in
//       TypeConverter::ConvertValue when catalog says VARCHAR but TDS
//       sends back a temporal type (typically via a view CAST).
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
namespace datetime {

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row);
void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
// Single-Value overload for the BCPRowEncoder::EncodeValue public path.
void EncodeToBcp(const Value &value, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf);
std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx);
std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig &cfg, DdlContext ctx);
size_t EstimateLiteralSize(const LogicalType &type);

// Issue #89 fallback: render TDS wire bytes as a raw text representation
// (no SQL quoting / CAST wrapper) for insertion into a VARCHAR vector.
// Dispatches on col.type_id + bytes.size() to cover every TDS temporal
// wire format.
std::string RenderAsString(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col);

}  // namespace datetime
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
