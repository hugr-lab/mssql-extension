//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/staging/column_ops.hpp
//
// Per-column dispatch resolution for the staged read path (spec 055 D4).
//
// Resolved ONCE per column after COLMETADATA, so the per-value path carries no
// type dispatch at all — today's `TypeConverter::ConvertValue` runs a switch
// over the TDS type for every cell (type_converter.cpp:263-345). Same shape as
// the shipped write-path W2 (`BCPRowEncoder::ResolveEncoder`).
//===----------------------------------------------------------------------===//

#pragma once

#include "codec/staging/column_staging.hpp"
#include "duckdb/common/types.hpp"
#include "tds/tds_column_metadata.hpp"

namespace duckdb {
namespace mssql {
namespace codec {
namespace staging {

//! The single per-value dispatch of the staged row walk.
//!
//! Wire framing and destination handling are FUSED into one selector rather than
//! kept as two nested switches. Both are column-invariant, so either way the
//! predictor is right every time — but two switches mean two indirect jumps per
//! value, and the whole per-value budget is a couple of nanoseconds.
//!
//! The prefix in the name is the framing:
//!   Raw — the value has no length prefix (TDS's non-nullable fixed types).
//!   P1  — one length byte, 0 meaning NULL (the *N nullable variants).
//! The suffix is the width in bytes.
//!
//! Values are positional on purpose and deliberately NOT written out: Convert
//! and Skip must stay last (see below), and every new staging arm otherwise
//! renumbers them for no reason.
enum class AppendArm : uint8_t {
	RawDirect1,
	RawDirect2,
	RawDirect4,
	RawDirect8,
	P1Direct1,
	P1Direct2,
	P1Direct4,
	P1Direct8,
	//! P2 — two length bytes, 0xFFFF meaning NULL: TDS's variable-length forms.
	//! Staged raw into the column buffer and decoded in one batch at finalize
	//! (spec 055 D5). Only UTF-16 string columns take this arm today.
	P2StageString,
	//! Same framing, but the wire bytes ARE the value: VARBINARY needs no
	//! conversion, only one allocation and one copy for the whole column.
	P2StageBinary,
	//! Fixed-width value staged for a batch kernel, with (P1) and without (Raw) a
	//! one-byte length prefix. The width lives in ColumnOps::stride and the
	//! kernel is chosen at finalize from the column's TDS type — both are
	//! column-invariant, so the row loop carries neither.
	P1StageFixed,
	RawStageFixed,
	//! MAX-typed columns (NVARCHAR(MAX), VARCHAR(MAX), VARBINARY(MAX), XML). The
	//! value arrives as a chunk list, possibly with its total length declared
	//! UNKNOWN, so it is assembled incrementally into the same Var slot the
	//! non-MAX forms use — and finalizes through the very same kernels.
	PlpStageString,
	PlpStageBinary,
	//! The pre-2005 LOBs (TEXT, NTEXT, IMAGE). Their row framing is unlike any
	//! other — a text pointer and a timestamp ahead of a 4-byte length — but the
	//! bytes behind it are ordinary, so they stage into the same Var slot and
	//! finalize through the same two kernels.
	LobStageString,
	LobStageBinary,
	//! DECIMAL/NUMERIC. Its own arm because it is the one fixed-width family
	//! whose wire value may legitimately be SHORTER than the declared width;
	//! the append zero-extends instead of rejecting it.
	P1StageDecimal,
	//! The wire type has no staged framing at all, so no value of it can be read.
	//! TEXT/NTEXT/IMAGE via raw mssql_scan, UDT and SQL_VARIANT land here; the
	//! shipped row reader cannot decode them either. It throws when a value
	//! ARRIVES rather than at column resolution, so a query that selects such a
	//! column but returns no rows keeps succeeding, as it does today.
	//!
	//! Ordered AFTER every staging arm: "does this column stage?" is one
	//! comparison.
	Unsupported,
	//! Parsed for its length only; the value is discarded. Columns the query does
	//! not project (COUNT(*), a narrower output chunk) still have to be walked to
	//! find where the next column starts.
	Skip
};

//! Everything the staged path needs to know about one column, decided once.
struct ColumnOps {
	//! The per-value arm. Set by ResolveColumnOps; the caller downgrades it to
	//! Skip for columns it does not intend to fill.
	AppendArm arm = AppendArm::Unsupported;
	//! Which append arm the row reader uses for this column.
	StagingKind kind = StagingKind::Var;
	//! Bytes per value for Direct / Fixed; 0 for Var.
	uint32_t stride = 0;
	//! True when the wire bytes ARE the DuckDB representation, so the value can
	//! be stored into the output vector with no conversion whatsoever. This is
	//! the "direct-write bypass": no staging buffer, no finalize kernel.
	bool direct_write = false;
	//! The catalog-declared type and the type on the wire disagree — a view with
	//! an inline CAST does that (issue #89). The column still stages by its WIRE
	//! framing like any other; only the kernel changes, to one that renders each
	//! value as text. That rendering has no bulk primitive, but it is still one
	//! loop per column with the dispatch resolved once, and the row walk keeps a
	//! single shape.
	bool needs_value_fallback = false;
	//! Fixed-length CHAR/NCHAR: SQL Server pads to the declared width, and the
	//! shipped decode strips those trailing spaces. Applied to the OUTPUT here,
	//! which is exactly equivalent — a 0x20 byte in UTF-8 can only be U+0020,
	//! never a continuation byte — and unlike an input-side trim it is equally
	//! correct when the payload turns out to be invalid UTF-16.
	bool trim_trailing_spaces = false;
	//! Var only: the declared upper bound on ONE value's wire size, in bytes, or
	//! 0 when the type has no bound (PLP / MAX). Lets the staging buffer be
	//! preallocated to a chunk's provable worst case, so a narrow column never
	//! resizes at all.
	uint32_t max_value_bytes = 0;
};

//! Resolve a column.
//!
//! `target_type` is the type of the vector the values will land in, which is NOT
//! always what the wire metadata implies: the scan can CAST (spec 026 VARCHAR ->
//! NVARCHAR) and views can diverge (issue #89). When they disagree the column is
//! marked `needs_value_fallback` and never takes a direct or batch path, so a
//! divergence degrades to today's behaviour instead of writing wrong bytes into
//! a vector of a different physical type.
ColumnOps ResolveColumnOps(const tds::ColumnMetadata &column, const LogicalType &target_type);

}  // namespace staging
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
