//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/staging/column_ops.cpp — spec 055 D4.
//===----------------------------------------------------------------------===//

#include "codec/staging/column_ops.hpp"

#include "tds/encoding/type_converter.hpp"
#include "tds/tds_types.hpp"

namespace duckdb {
namespace mssql {
namespace codec {
namespace staging {

using duckdb::tds::ColumnMetadata;

namespace {

//! Width of a fixed-width TDS value in bytes, or 0 if the type is not fixed
//! width. `max_length` carries the width for the nullable *N variants.
uint32_t FixedWireWidth(const tds::ColumnMetadata &column) {
	switch (column.type_id) {
	case tds::TDS_TYPE_TINYINT:
	case tds::TDS_TYPE_BIT:
		return 1;
	case tds::TDS_TYPE_SMALLINT:
		return 2;
	case tds::TDS_TYPE_INT:
	case tds::TDS_TYPE_REAL:
	case tds::TDS_TYPE_SMALLMONEY:
		return 4;
	case tds::TDS_TYPE_BIGINT:
	case tds::TDS_TYPE_FLOAT:
	case tds::TDS_TYPE_MONEY:
	case tds::TDS_TYPE_DATETIME:
		return 8;
	case tds::TDS_TYPE_SMALLDATETIME:
		return 4;
	case tds::TDS_TYPE_INTN:
	case tds::TDS_TYPE_FLOATN:
	case tds::TDS_TYPE_BITN:
	case tds::TDS_TYPE_MONEYN:
	case tds::TDS_TYPE_DATETIMEN:
		// Nullable variants: the declared width lives in max_length. The value
		// still arrives length-prefixed, but every non-NULL value is exactly
		// this wide.
		return static_cast<uint32_t>(column.max_length);
	case tds::TDS_TYPE_UNIQUEIDENTIFIER:
		return 16;
	default:
		// DECIMAL/NUMERIC, DATE, TIME, DATETIME2, DATETIMEOFFSET are fixed width
		// per column but their width depends on precision/scale, and they all
		// need a conversion pass regardless — they are classified Fixed below
		// via the codec's own knowledge, not here.
		return 0;
	}
}

//! Is this TDS type's wire representation byte-identical to how DuckDB stores
//! the corresponding logical type?
//!
//! Only integers and IEEE-754 floats qualify: TDS sends them little-endian
//! two's complement / IEEE-754, which is exactly DuckDB's in-memory layout on
//! every platform this builds for.
//!
//! BIT qualifies too: TDS defines it as exactly 0 or 1 (NULL travels in the
//! length prefix of BITN, not in the value byte), which is precisely DuckDB's
//! one-byte bool. There is no third value to normalise away.
bool IsDirectWritable(const tds::ColumnMetadata &column, uint32_t width) {
	switch (column.type_id) {
	case tds::TDS_TYPE_TINYINT:
		return width == 1;
	case tds::TDS_TYPE_SMALLINT:
		return width == 2;
	case tds::TDS_TYPE_INT:
		return width == 4;
	case tds::TDS_TYPE_BIGINT:
		return width == 8;
	case tds::TDS_TYPE_REAL:
		return width == 4;
	case tds::TDS_TYPE_FLOAT:
		return width == 8;
	case tds::TDS_TYPE_BIT:
	case tds::TDS_TYPE_BITN:
		return width == 1;
	case tds::TDS_TYPE_INTN:
		return width == 1 || width == 2 || width == 4 || width == 8;
	case tds::TDS_TYPE_FLOATN:
		return width == 4 || width == 8;
	default:
		return false;
	}
}

//! Does this type carry a one-byte length prefix on the wire?
//!
//! Every fixed-width type FixedWireWidth answers for is classified here, not
//! just the direct-writable ones. The nullable *N variants and
//! UNIQUEIDENTIFIER are length-prefixed; the non-nullable forms — TINYINT, BIT,
//! SMALLINT, INT, BIGINT, REAL, FLOAT, MONEY, SMALLMONEY, DATETIME,
//! SMALLDATETIME — are sent as bare value bytes.
//!
//! MONEYN, DATETIMEN and UNIQUEIDENTIFIER were missing. That was invisible while
//! the only caller was the direct-writable path — they never reach it — and then
//! the issue-#89 fallback below started asking too and was told a DATETIMEN is
//! bare, so the walk read a prefixed value as raw bytes while the parser
//! consumed its one-byte NULL prefix. Found by the spec-059 fuzz harness; in a
//! release build that is a heap over-read and every column after it decodes from
//! the wrong offset.
//!
//! The width guard in that fallback is what actually closes the hole, and with
//! it these three cases are unreachable again. They are corrected anyway because
//! the answer was simply WRONG about the wire format, and the next caller to ask
//! would walk into the same trap.
bool HasOneBytePrefix(uint8_t type_id) {
	switch (type_id) {
	case tds::TDS_TYPE_INTN:
	case tds::TDS_TYPE_BITN:
	case tds::TDS_TYPE_FLOATN:
	case tds::TDS_TYPE_MONEYN:
	case tds::TDS_TYPE_DATETIMEN:
	case tds::TDS_TYPE_UNIQUEIDENTIFIER:
		return true;
	default:
		return false;
	}
}

//! Wire width of a temporal column, or 0 if it is not one.
//!
//! DATE is always three bytes; TIME/DATETIME2/DATETIMEOFFSET carry a
//! scale-dependent time field with a fixed date (and offset) suffix. Every one
//! of them is fixed for a given COLUMN, which is what makes them stageable.
//! Wire width of a BARE fixed type — one whose value carries no length on the
//! wire and whose width is fixed by the type id, not by the declared metadata
//! width. The codec kernel reads that width unconditionally: DATETIME's
//! ConvertDatetime always reads 8 bytes, SMALLDATETIME's ConvertSmallDatetime 4,
//! neither consulting the staged length. So the staged stride MUST come from the
//! type here, not from `column.max_length`. A stream that declares DATETIME with
//! a 4-byte width otherwise staged 4 bytes under an 8-byte read — a heap
//! overflow on the constant path AND a strided over-read on the batch path
//! (fuzz_row_stager, PR #267). MONEY/SMALLMONEY dispatch on length and were
//! already safe; canonicalising them keeps the stride==wire-width invariant
//! true for every bare type rather than most of them.
uint32_t BareFixedWireWidth(uint8_t type_id) {
	switch (type_id) {
	case tds::TDS_TYPE_DATETIME:
		return 8;
	case tds::TDS_TYPE_SMALLDATETIME:
		return 4;
	case tds::TDS_TYPE_MONEY:
		return 8;
	case tds::TDS_TYPE_SMALLMONEY:
		return 4;
	default:
		return 0;
	}
}

uint32_t TemporalWireWidth(const tds::ColumnMetadata &column) {
	const uint32_t time_len = column.scale <= 2 ? 3 : (column.scale <= 4 ? 4 : 5);
	switch (column.type_id) {
	case tds::TDS_TYPE_DATE:
		return 3;
	case tds::TDS_TYPE_TIME:
		return time_len;
	case tds::TDS_TYPE_DATETIME2:
		return 3 + time_len;
	case tds::TDS_TYPE_DATETIMEOFFSET:
		return 5 + time_len;
	default:
		return 0;
	}
}

//! Widths the staged-fixed append can copy with a compile-time size. A column
//! whose width is not here stays on the per-value path rather than paying for a
//! runtime-sized memcpy, which is a libc call.
bool IsStageableFixedWidth(uint32_t width) {
	switch (width) {
	case 1:
	case 2:
	case 3:
	case 4:
	case 5:
	case 6:
	case 7:
	case 8:
	case 9:
	case 10:
	case 13:
	case 16:
	case 17:
		return true;
	default:
		return false;
	}
}

//! Wire width of a DECIMAL/NUMERIC column: one sign byte plus a mantissa sized
//! by the declared precision.
uint32_t DecimalWireWidth(uint8_t precision) {
	if (precision <= 9) {
		return 5;
	}
	if (precision <= 19) {
		return 9;
	}
	if (precision <= 28) {
		return 13;
	}
	return 17;
}

//! Is a wire/target pair that is NOT an exact match still one a kernel handles?
//!
//! Two of them exist, and both would otherwise be misread as the stale catalog
//! of issue #89 — sending every datetime2(7) and every spatial column down the
//! render-as-text path.
//!
//! Type equality is the wrong test here, and physical equality alone is not
//! enough either: DECIMAL(18,2) and DECIMAL(18,4) share a physical type and
//! mean different numbers. So the exemptions are named one by one.
bool IsSupportedRetarget(const tds::ColumnMetadata &column, const LogicalType &target_type) {
	switch (column.type_id) {
	case tds::TDS_TYPE_DATETIME2:
		// The kernel converts into the target's own unit, so every TIMESTAMP
		// variant is a destination it can write.
		switch (target_type.id()) {
		case LogicalTypeId::TIMESTAMP_SEC:
		case LogicalTypeId::TIMESTAMP_MS:
		case LogicalTypeId::TIMESTAMP:
		case LogicalTypeId::TIMESTAMP_NS:
			return true;
		default:
			return false;
		}
	case tds::TDS_TYPE_BIGBINARY:
	case tds::TDS_TYPE_BIGVARBINARY:
		// Raw bytes into any string_t-backed target. Spatial columns arrive this
		// way: the scan auto-projects STAsBinary(), so the wire says VARBINARY
		// while the catalog says GEOMETRY.
		return target_type.InternalType() == PhysicalType::VARCHAR;
	default:
		return false;
	}
}

AppendArm DirectArm(uint8_t type_id, uint32_t width) {
	const bool prefixed = HasOneBytePrefix(type_id);
	switch (width) {
	case 1:
		return prefixed ? AppendArm::P1Direct1 : AppendArm::RawDirect1;
	case 2:
		return prefixed ? AppendArm::P1Direct2 : AppendArm::RawDirect2;
	case 4:
		return prefixed ? AppendArm::P1Direct4 : AppendArm::RawDirect4;
	default:
		return prefixed ? AppendArm::P1Direct8 : AppendArm::RawDirect8;
	}
}

//! Which kernel publishes the column, given the arm the walk will use.
//!
//! One switch in one place, so a new staging arm cannot be added without saying
//! how it finalizes: there is no `default`, and -Wswitch makes the omission a
//! build failure rather than a column that silently publishes nothing.
FinalizeKernel ResolveKernel(const ColumnOps &ops, const tds::ColumnMetadata &column) {
	if (ops.needs_value_fallback) {
		// Wins over the family: the bytes staged are the WIRE type's, and the
		// output vector is a different type entirely (issue #89).
		return FinalizeKernel::Text;
	}
	switch (ops.arm) {
	case AppendArm::RawDirect1:
	case AppendArm::RawDirect2:
	case AppendArm::RawDirect4:
	case AppendArm::RawDirect8:
	case AppendArm::P1Direct1:
	case AppendArm::P1Direct2:
	case AppendArm::P1Direct4:
	case AppendArm::P1Direct8:
	case AppendArm::Unsupported:
	case AppendArm::Skip:
		return FinalizeKernel::None;
	case AppendArm::P2StageString:
	case AppendArm::PlpStageString:
	case AppendArm::LobStageString:
		return FinalizeKernel::String;
	case AppendArm::P2StageBinary:
	case AppendArm::PlpStageBinary:
	case AppendArm::LobStageBinary:
		return FinalizeKernel::Binary;
	case AppendArm::P1StageDecimal:
		return FinalizeKernel::Decimal;
	case AppendArm::P1StageFixed:
	case AppendArm::RawStageFixed:
		// Three families share the staged-fixed framing; the TDS type says which.
		if (column.type_id == tds::TDS_TYPE_UNIQUEIDENTIFIER) {
			return FinalizeKernel::Uuid;
		}
		if (column.type_id == tds::TDS_TYPE_MONEY || column.type_id == tds::TDS_TYPE_SMALLMONEY ||
			column.type_id == tds::TDS_TYPE_MONEYN) {
			return FinalizeKernel::Money;
		}
		return FinalizeKernel::Datetime;
	}
	return FinalizeKernel::None;
}

//! The arm/kind/stride half of the resolution; ResolveColumnOps adds the kernel.
ColumnOps ResolveAppend(const tds::ColumnMetadata &column, const LogicalType &target_type) {
	ColumnOps ops;

	// Does the wire type agree with the vector being written into? A view with an
	// inline CAST can make the catalog promise VARCHAR while TDS delivers
	// something else (issue #89); the scan can also CAST VARCHAR to NVARCHAR
	// itself (spec 026).
	//
	// Resolving that once, here, keeps the check off the per-cell path. The
	// column still stages by its WIRE framing — the bytes are the bytes — and
	// only the finalize kernel changes, to one that renders each value as text.
	// What it must NOT do is write wire bytes straight into a vector of a
	// different physical type, so a divergent column never takes a direct write.
	LogicalType wire_type;
	try {
		wire_type = tds::encoding::TypeConverter::GetDuckDBType(column);
	} catch (const std::exception &) {
		// A type GetDuckDBType does not know cannot be framed either. Initialize()
		// already threw on it while building the column list, so this is
		// unreachable from the stream — but it must not silently pick a framing.
		ops.arm = AppendArm::Unsupported;
		return ops;
	}

	// Spec 060: two VARCHARs never diverge from each other, whatever annotations
	// they carry. LogicalType equality compares the aux info too, so a catalog
	// column reporting the declared nvarchar(n) bound would otherwise "diverge"
	// from the plain VARCHAR the wire produces and render every value as text.
	// The guard's real question — is the catalog claiming a DIFFERENT type than
	// the wire sends — is unaffected: VARCHAR against INTEGER still differs by id.
	const bool same_type = wire_type == target_type ||
						   (wire_type.id() == LogicalTypeId::VARCHAR && target_type.id() == LogicalTypeId::VARCHAR);
	const bool diverges = !same_type && !IsSupportedRetarget(column, target_type);
	ops.needs_value_fallback = diverges;

	const uint32_t width = FixedWireWidth(column);
	// A direct write stores wire bytes into the output vector at `width` stride,
	// so the wire width MUST equal the width DuckDB reserves per slot. The two
	// come from different files (FixedWireWidth here, GetDuckDBType in the type
	// converter) and today they agree for every direct type — TINYINT/BIT are one
	// byte in both, REAL/FLOAT four, and so on. Proving it here, once per column,
	// means a future remapping degrades to staging instead of writing 1 byte into
	// a 2-byte slot.
	if (!diverges && width > 0 && IsDirectWritable(column, width) &&
		width == GetTypeIdSize(target_type.InternalType())) {
		ops.kind = DirectKindForWidth(width);
		ops.arm = DirectArm(column.type_id, width);
		ops.stride = width;
		ops.direct_write = true;
		return ops;
	}
	if ((column.type_id == tds::TDS_TYPE_DECIMAL || column.type_id == tds::TDS_TYPE_NUMERIC) && column.precision > 0 &&
		column.precision <= 38) {
		ops.kind = StagingKind::Fixed;
		ops.arm = AppendArm::P1StageDecimal;
		ops.stride = DecimalWireWidth(column.precision);
		return ops;
	}

	// Fixed-width families with a batch kernel: UNIQUEIDENTIFIER and the temporal
	// types. DATETIME and SMALLDATETIME arrive bare; everything else carries the
	// one-byte length prefix.
	// A bare type's width comes from its type id, never the declared width — the
	// kernel reads the type's width regardless of what the metadata claimed.
	const uint32_t bare_wire_width = BareFixedWireWidth(column.type_id);
	const uint32_t fixed_width = bare_wire_width > 0 ? bare_wire_width : (width > 0 ? width : TemporalWireWidth(column));
	if (fixed_width > 0 && IsStageableFixedWidth(fixed_width)) {
		const bool staged_family =
			column.type_id == tds::TDS_TYPE_UNIQUEIDENTIFIER || column.type_id == tds::TDS_TYPE_DATE ||
			column.type_id == tds::TDS_TYPE_TIME || column.type_id == tds::TDS_TYPE_DATETIME2 ||
			column.type_id == tds::TDS_TYPE_DATETIMEOFFSET || column.type_id == tds::TDS_TYPE_DATETIME ||
			column.type_id == tds::TDS_TYPE_SMALLDATETIME || column.type_id == tds::TDS_TYPE_DATETIMEN ||
			column.type_id == tds::TDS_TYPE_MONEY || column.type_id == tds::TDS_TYPE_SMALLMONEY ||
			column.type_id == tds::TDS_TYPE_MONEYN;
		if (staged_family) {
			const bool bare = column.type_id == tds::TDS_TYPE_DATETIME ||
							  column.type_id == tds::TDS_TYPE_SMALLDATETIME || column.type_id == tds::TDS_TYPE_MONEY ||
							  column.type_id == tds::TDS_TYPE_SMALLMONEY;
			ops.kind = StagingKind::Fixed;
			ops.arm = bare ? AppendArm::RawStageFixed : AppendArm::P1StageFixed;
			ops.stride = fixed_width;
			return ops;
		}
	}
	if (width > 0) {
		// A DIVERGING column reaches here (without a divergence a fixed-width type
		// is either direct-written or claimed by the staged-fixed families above),
		// and so does any *N variant whose declared width is not a width the staged
		// append can copy. It still needs an arm — leaving it Unsupported made the
		// issue-#89 fallback throw for exactly the integer, float and BIT types it
		// exists to rescue, instead of rendering them as text the way the pre-055
		// path did. The Text kernel reads Fixed staging positionally, so Fixed is
		// the right shape.
		//
		// The width has to be one AppendStagedFixed actually has a case for.
		// Without this test it took `stride` on trust and fell into the switch's
		// default, which copies SEVENTEEN bytes: a DATETIMEN declaring 52 then had
		// 17 bytes read out of a one-byte NULL value. No conforming server sends
		// such a width, so there is nothing to rescue — say so instead, and let the
		// error name the column when a value arrives.
		if (!IsStageableFixedWidth(width)) {
			ops.arm = AppendArm::Unsupported;
			return ops;
		}
		ops.kind = StagingKind::Fixed;
		ops.stride = width;
		ops.arm = HasOneBytePrefix(column.type_id) ? AppendArm::P1StageFixed : AppendArm::RawStageFixed;
		return ops;
	}

	const bool binary = column.type_id == tds::TDS_TYPE_BIGVARBINARY || column.type_id == tds::TDS_TYPE_BIGBINARY;

	// The legacy LOBs. A catalog scan casts them server-side and never produces
	// them (spec 055 D9), but raw mssql_scan has no metadata to cast from — so
	// they are decoded natively too rather than left as the one shape the
	// extension cannot read.
	if (column.type_id == tds::TDS_TYPE_NTEXT) {
		ops.kind = StagingKind::Var;
		ops.arm = AppendArm::LobStageString;
		return ops;
	}
	if (column.type_id == tds::TDS_TYPE_TEXT || column.type_id == tds::TDS_TYPE_IMAGE) {
		// TEXT is single-byte like CHAR/VARCHAR: collation handling lives in the
		// scan's SELECT list, not here.
		ops.kind = StagingKind::Var;
		ops.arm = AppendArm::LobStageBinary;
		return ops;
	}

	// UTF-16 string columns take the batch decode (D5), MAX forms included: the
	// chunk list is assembled incrementally into the same staged layout.
	//
	// NCHAR is included: its trailing-space trim moves to the OUTPUT, where it is
	// equally correct on invalid UTF-16. The kernel derives that from the TDS
	// type itself, so the rule lives with the family that applies it.
	const bool utf16_string = column.type_id == tds::TDS_TYPE_NVARCHAR || column.type_id == tds::TDS_TYPE_NCHAR ||
							  column.type_id == tds::TDS_TYPE_XML;
	if (utf16_string && column.IsPLPType()) {
		// Same staged layout and the same finalize kernel as the bounded form;
		// only the framing differs, because the value arrives as a chunk list.
		ops.kind = StagingKind::Var;
		ops.arm = AppendArm::PlpStageString;
		return ops;
	}
	if (utf16_string && !column.IsPLPType() && column.max_length > 0) {
		ops.kind = StagingKind::Var;
		ops.arm = AppendArm::P2StageString;
		// +2 for the U+0000 the batch decode splits on: it is staged with the
		// value and is part of what one value COSTS in the buffer. Without it a
		// full chunk of maximum-length values overruns the "provable worst case"
		// by exactly two bytes per row, so the column both grows and keeps
		// claiming it cannot — which is what the D10 grow counter reported.
		ops.max_value_bytes = static_cast<uint32_t>(column.max_length) + 2;
		return ops;
	}

	// Single-byte CHAR/VARCHAR: the wire bytes go into the VARCHAR vector as they
	// are — collation handling lives in the scan's SELECT list (spec 026), not
	// here — so this is the same one-allocation-one-copy shape as VARBINARY.
	const bool single_byte_char = column.type_id == tds::TDS_TYPE_BIGCHAR || column.type_id == tds::TDS_TYPE_BIGVARCHAR;
	if ((single_byte_char || binary) && column.IsPLPType()) {
		// VARCHAR(MAX) and VARBINARY(MAX): raw bytes either way, so the same
		// kernel as their bounded forms.
		ops.kind = StagingKind::Var;
		ops.arm = AppendArm::PlpStageBinary;
		return ops;
	}
	if (single_byte_char && !column.IsPLPType() && column.max_length > 0) {
		ops.kind = StagingKind::Var;
		ops.arm = AppendArm::P2StageBinary;
		ops.max_value_bytes = static_cast<uint32_t>(column.max_length);
		return ops;
	}

	// VARBINARY: no conversion at all, so the batch path is one allocation and
	// one copy per column instead of one of each per value.
	if (binary && !column.IsPLPType() && column.max_length > 0) {
		ops.kind = StagingKind::Var;
		ops.arm = AppendArm::P2StageBinary;
		ops.max_value_bytes = static_cast<uint32_t>(column.max_length);
		return ops;
	}

	// Nothing above claimed it. The types that reach here are the legacy LOBs
	// (TEXT / NTEXT / IMAGE) when they arrive without the catalog's server-side
	// CAST, plus UDT and SQL_VARIANT — none of which the shipped row reader can
	// decode either. Say so instead of inventing a framing.
	ops.arm = AppendArm::Unsupported;
	return ops;
}

}  // namespace

const char *FinalizeKernelName(FinalizeKernel kernel) {
	switch (kernel) {
	case FinalizeKernel::None:
		return "none";
	case FinalizeKernel::String:
		return "string";
	case FinalizeKernel::Binary:
		return "binary";
	case FinalizeKernel::Uuid:
		return "uuid";
	case FinalizeKernel::Decimal:
		return "decimal";
	case FinalizeKernel::Money:
		return "money";
	case FinalizeKernel::Datetime:
		return "datetime";
	case FinalizeKernel::Text:
		return "text";
	}
	return "?";
}

ColumnOps ResolveColumnOps(const tds::ColumnMetadata &column, const LogicalType &target_type) {
	ColumnOps ops = ResolveAppend(column, target_type);
	ops.kernel = ResolveKernel(ops, column);
	return ops;
}

}  // namespace staging
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
