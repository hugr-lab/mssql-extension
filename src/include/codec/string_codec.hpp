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
#include "codec/staging/column_staging.hpp"
#include "codec/type_family.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector.hpp"

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
namespace string {

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row);

//! Batch UTF-16LE -> UTF-8 decode of a staged column (spec 055 D5/T10).
//!
//! WHAT THE PER-VALUE PATH DOES, PER VALUE
//!   1. Utf8LengthFromUtf16LEView  — a full pass that both validates and counts
//!   2. StringVector::EmptyString  — an allocation in the vector's string heap
//!   3. Utf16LEDecodeValidInto     — a second pass that converts
//! Step 1 costs MORE than step 3 (measured 36.6 vs ~21 ns at 200 characters),
//! and it exists only to size step 2.
//!
//! WHAT THIS DOES
//! One conversion call for the entire column, into one allocation. simdutf's
//! checked converter validates and converts in the same pass and returns the
//! byte count, so the length pass disappears rather than being made faster.
//!
//! The count also answers, for free, whether the column was pure ASCII: if the
//! output is exactly as many BYTES as the input had code UNITS, every unit
//! produced one byte. Value boundaries in the output are then the staged byte
//! offsets halved — no delimiter search, no per-value bookkeeping.
//!
//! Measured against the per-value path, ns/value (2048-row chunks, real Vector):
//!
//!   chars   per-value   one alloc + no length pass   + single call
//!       4        14.8                          7.0            3.1
//!      16        18.0                          7.1            2.2
//!     200        58.0                         20.1           11.5
//!
//! `st` must hold raw UTF-16LE wire bytes appended with AppendVarDelimited,
//! `count` rows staged, and validity in its mask. NULL rows are left untouched —
//! their offset/length slots are undefined by ColumnStaging's contract.
//!
//! Invalid UTF-16 (unpaired surrogates are legal in a UCS-2 collation) is
//! handled in bulk runs with standard U+FFFD substitution; there is no per-value
//! step on that path either. Fixed-length NCHAR's trailing-space trim is a
//! property of the TDS type, so it is derived from `col` rather than passed in.
void DecodeChunkFromStaging(const staging::ColumnStaging &st, idx_t count, const tds::ColumnMetadata &col, Vector &out);
// W1 (spec 054): format-threaded overload — fmt is built once per column per
// chunk by BCPRowEncoder::EncodeChunk. The (Vector, row) overload below
// wraps it for per-row callers (builds the format per call).
void EncodeToBcp(Vector &in, const UnifiedVectorFormat &fmt, idx_t row, const mssql::BCPColumnMetadata &col,
				 duckdb::vector<uint8_t> &buf);

//! Spec 060 / issue #225 (write side): the UTF-8 twin of EncodeToBcp, for a
//! target column that holds UTF-8 bytes. A SEPARATE entry point rather than a
//! branch inside EncodeToBcp — BCPRowEncoder resolves the encoder once per
//! column, so the row loop must not carry a test the column already answered.
//! Measured: the branch cost the untouched nvarchar path ~20 ns/value.
void EncodeToBcpUtf8(Vector &in, const UnifiedVectorFormat &fmt, idx_t row, const mssql::BCPColumnMetadata &col,
					 duckdb::vector<uint8_t> &buf);
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

//! Per-column measurement for the columnar scatter (spec 057 step 3).
//!
//! `lengths[r]` is the PAYLOAD byte count row r will occupy on the wire — UTF-16
//! bytes for an nvarchar target, source bytes for a UTF-8 one — with 0 for NULL.
//! Framing (the 2-byte prefix, or PLP's header and terminator) is the caller's.
//! Which of the three variable-length wire forms this column takes. They share
//! the framing (2-byte LE length, 0xFFFF for NULL) and differ in whether the
//! payload is converted and how an over-long value is refused — nvarchar and
//! varchar each raise with their own wording, which tests assert, and binary has
//! never checked at all.
enum class VarKind : uint8_t { NVarchar, Utf8Varchar, Binary };

struct StringColumnPlan {
	duckdb::vector<uint32_t> lengths;
	VarKind kind = VarKind::NVarchar;
};

//! Write one block of a planned variable-length column at the cursor positions.
void ScatterVarBlock(uint8_t *dst, size_t *cursor, idx_t r0, idx_t rend, const UnifiedVectorFormat &fmt,
					 const mssql::BCPColumnMetadata &col, const StringColumnPlan &plan);

//! Measure a column so the chunk's wire can be sized before anything is written.
//! Returns false when the column cannot be planned — invalid UTF-8, or a wire
//! form not handled yet — and the caller must fall back to the row path.
bool PlanColumn(Vector &in, const UnifiedVectorFormat &fmt, idx_t row_begin, idx_t rows,
				const mssql::BCPColumnMetadata &col, StringColumnPlan &plan);

}  // namespace string
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
