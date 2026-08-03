//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/write_column_ops.hpp
//
// Per-column dispatch resolution for the BCP write path (spec 057 step 3).
//
// The mirror of codec/staging/column_ops.hpp on the read side, named to match so
// one reader can hold both. Resolved ONCE per column per chunk, so the per-value
// path carries no type dispatch at all.
//
// Two decisions carried over from the read side deliberately:
//
//   * The arm is an ENUM, not a function pointer. An arm selected by a small
//     enum inside one loop can be inlined and vectorised; an indirect call per
//     value can be neither, which would give back most of what the columnar
//     shape buys. (The first draft of this design proposed function pointers.)
//
//   * `max_value_bytes` — the most bytes ONE value can occupy — so a chunk's
//     worst case is preallocable from METADATA alone, never from the data.
//
// And one that is specific to writing, and is the point of the whole file:
// **the append form is derived from the arm, not written separately.** For a
// fixed-width column, appending is `resize(n + 1 + w); buf[n] = w;` and then the
// same arm that the columnar scatter uses. So the row-major path and the
// columnar path cannot produce different bytes — not because a test says so, but
// because there is one implementation.
//
// That property is the whole reason this exists. `IsTypeCompatible` (a table of
// type NAMES in target_resolver.cpp) and the family encoders (what can actually
// be executed) disagreed for two releases: every widening the table advertised
// died in the encoder with an INTERNAL Error, while a decimal scale mismatch was
// waved through and silently moved the decimal point (issue #153). Adding a
// third parallel list would have made that worse. Compatibility is meant to
// become `arm != Unsupported`, so a conversion that cannot be executed cannot be
// advertised.
//===----------------------------------------------------------------------===//

#pragma once

#include "copy/target_resolver.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {
namespace mssql {
namespace codec {

//! How the value's PAYLOAD is produced. The length prefix is framing and belongs
//! to WireKind, not here.
//!
//! DirectCopyN means the wire bytes ARE what the vector already stores, so the
//! arm is a memcpy of N bytes — the write-side peer of the read side's
//! `direct_write` bypass. Little-endian is assumed, as it already is by the
//! byte-wise appenders this replaces and by DecodeFromTds's memcpy.
enum class ScatterArm : uint8_t {
	//! No kernel for this (source, target) pair. This IS the compatibility
	//! answer — see the file comment.
	Unsupported = 0,
	//! The source column is absent from the chunk: every row is a NULL marker,
	//! so there is no payload to produce and the column costs one byte per row.
	NullOnly,
	DirectCopy1,
	DirectCopy2,
	DirectCopy4,
	DirectCopy8,
	//! An integer whose SOURCE width differs from the target's: read at the
	//! source's width, range-check against the target's, write at the target's.
	//! This is issue #153 as a resolved arm — the pair is fixed for the whole
	//! column, so nothing about it belongs in the per-value path.
	IntConvert,
	//! FLOAT <-> DOUBLE where the source's width differs from the target's. Its
	//! own arm rather than IntConvert's because narrowing here is NOT an error:
	//! losing precision is what the declared column asked for, so no value is
	//! ever refused.
	FloatConvert,
	//! Sign byte plus little-endian magnitude, width from the target's precision.
	//! MONEY and SMALLMONEY map to DECIMAL and arrive here too.
	Decimal,
	//! DATE (3 bytes) and the DATETIME2 family (time 3/4/5 + date 3). TIME and
	//! DATETIMEOFFSET are not here yet and take the row path.
	Datetime,
	//! The 16 wire bytes of a UNIQUEIDENTIFIER (mixed-endian by the spec, which
	//! is why it is a kernel and not a copy).
	Guid,
	//! A variable-length string with a 2-byte length prefix — nvarchar(n), or a
	//! varchar(n) whose UTF-8 bytes pass through unconverted. Its payload width
	//! depends on the value, so a chunk containing one can never take the
	//! constant-stride path; it is planned per column first (string::PlanColumn)
	//! and then written with the cursor.
	VarString,
	//! Resolvable, but no columnar kernel yet — DECIMAL, temporal, GUID and the
	//! string families still go through the row-major append path. Distinct from
	//! Unsupported: this pair IS encodable, just not by a scatter, so it must not
	//! be read as an incompatibility.
	RowFallback,
};

//! The value's framing on the BCP wire, which also decides the NULL form.
enum class WireKind : uint8_t {
	//! One length byte, 0 meaning NULL.
	Fixed,
	//! Two length bytes, 0xFFFF meaning NULL.
	VariableUShort,
	//! PLP: an 8-byte total, then chunks.
	Plp,
};

//! Everything the per-value path needs, resolved once per column per chunk.
struct WriteColumnOps {
	ScatterArm arm = ScatterArm::Unsupported;
	WireKind kind = WireKind::Fixed;
	//! Payload bytes per value for a Fixed column; 0 when the width depends on
	//! the value. Excludes the length prefix.
	uint32_t wire_width = 0;

	//! IntConvert / FloatConvert only: bytes the SOURCE stores per value. Zero
	//! otherwise. Signedness comes from the source type, not from this.
	uint8_t src_width = 0;

	//! Does this column's width depend on the data? A single one of these makes
	//! every row in the chunk a different length, so the constant-stride path is
	//! unavailable for ALL columns, NULLs or not.
	bool IsVariable() const {
		return arm == ScatterArm::VarString;
	}

	//! Can this column take the columnar scatter?
	bool CanScatter() const {
		return arm != ScatterArm::Unsupported && arm != ScatterArm::RowFallback;
	}
};

//! Resolve one column.
//!
//! `source` is the type of the vector the values come from, which the target is
//! under no obligation to match — that mismatch is exactly what issue #153 was
//! about, and a pair this function cannot execute must come back as
//! ScatterArm::Unsupported rather than as something that reads the wrong width.
WriteColumnOps ResolveWriteColumnOps(const LogicalType &source, const mssql::BCPColumnMetadata &target);

}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
