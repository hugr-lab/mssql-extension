//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/staging/column_ops.cpp — spec 055 D4.
//===----------------------------------------------------------------------===//

#include "codec/staging/column_ops.hpp"

#include <algorithm>
#include <initializer_list>

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
		// Raw bytes into any string_t-backed target: a raw scan's `.STAsBinary()`
		// read into the GEOMETRY its column_types name, say.
		return target_type.InternalType() == PhysicalType::VARCHAR;
	case tds::TDS_TYPE_UDT:
		// geometry / geography (spec 093 T5) into a GEOMETRY of either label: the
		// CRS is a name on the same WKB (a geography read as a plain GEOMETRY by
		// column_types, say).
		return column.IsSpatialUdt() && target_type.id() == LogicalTypeId::GEOMETRY;
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
FinalizeKernel ResolveKernel(const ColumnOps &ops, const tds::ColumnMetadata &column, const LogicalType &target_type) {
	if (ops.vector_kernel) {
		return FinalizeKernel::Vector;
	}
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
		// Spec 093 T5: a geometry / geography UDT stages its bytes on the PLP binary
		// arm and is decoded into WKB -- always into a GEOMETRY, into a BLOB unless
		// geometry_as 'native' asks for the stored bytes.
		if (column.IsSpatialUdt() &&
			(target_type.id() == LogicalTypeId::GEOMETRY || column.geometry_as != tds::GeometryAs::Native)) {
			return FinalizeKernel::Spatial;
		}
		// Spec 093 T4: a hierarchyid UDT stages its bytes on the PLP binary arm and is decoded to text; a
		// user CLR type stays bytes.
		if (column.IsHierarchyIdUdt()) {
			return FinalizeKernel::HierarchyId;
		}
		// The binary arms carry VARBINARY / IMAGE as well; code_page is set on
		// single-byte text only, and only when its catalog transcodes.
		return column.code_page != nullptr ? FinalizeKernel::CodePage : FinalizeKernel::Binary;
	case AppendArm::P1StageDecimal:
		return FinalizeKernel::Decimal;
	case AppendArm::P4StageVariant:
		return FinalizeKernel::Variant;
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

//! Is `type_id` one of `types`? A list rather than a switch, so the families below read as the lists
//! they are.
bool IsOneOf(uint8_t type_id, std::initializer_list<uint8_t> types) {
	return std::find(types.begin(), types.end(), type_id) != types.end();
}

//! The column claimed as a variable-length one staged on `arm`. `max_value_bytes` is the most one value
//! costs in the staging buffer, 0 for a type with no bound (PLP / MAX, the legacy LOBs).
bool ClaimVar(ColumnOps &ops, AppendArm arm, uint32_t max_value_bytes = 0) {
	ops.kind = StagingKind::Var;
	ops.arm = arm;
	ops.max_value_bytes = max_value_bytes;
	return true;
}

//! The column claimed as a fixed-width one staged on `arm`, `stride` bytes a value.
bool ClaimFixed(ColumnOps &ops, AppendArm arm, uint32_t stride) {
	ops.kind = StagingKind::Fixed;
	ops.arm = arm;
	ops.stride = stride;
	return true;
}

// ResolveAppend asks the steps below in order, and the first that claims the column decides its arm:
// each returns true with `ops` filled, or false with `ops` untouched. The order is part of the answer
// (the vector step before the divergence test, the fixed-width steps before the variable ones).

//! Spec 093 T3 (D4): a vector column into FLOAT[n], in whichever form the session streams it --
//! VECTORTYPE, framed as a bounded varbinary, or the JSON text, a varchar(max) / nvarchar(max) PLP --
//! so before the divergence test, which would take the text form for a VARCHAR and render it.
bool StageVector(const tds::ColumnMetadata &column, const LogicalType &target_type, ColumnOps &ops) {
	if (target_type.id() != LogicalTypeId::ARRAY || ArrayType::GetChildType(target_type).id() != LogicalTypeId::FLOAT) {
		return false;
	}
	if (column.IsVectorWire()) {
		ops.vector_kernel = true;
		return ClaimVar(ops, AppendArm::P2StageBinary, column.max_length);
	}
	if (column.IsVectorText() && column.IsPLPType()) {
		ops.vector_kernel = true;
		return ClaimVar(ops, AppendArm::PlpStageBinary);
	}
	return false;
}

//! Does the wire type disagree with the vector being written into? A view with an
//! inline CAST can make the catalog promise VARCHAR while TDS delivers
//! something else (issue #89); the scan can also CAST VARCHAR to NVARCHAR
//! itself (spec 026).
//!
//! Resolving that once, here, keeps the check off the per-cell path. The
//! column still stages by its WIRE framing — the bytes are the bytes — and
//! only the finalize kernel changes, to one that renders each value as text.
//! What it must NOT do is write wire bytes straight into a vector of a
//! different physical type, so a divergent column never takes a direct write.
//!
//! False when the wire type is one GetDuckDBType does not know: such a type cannot
//! be framed either. Initialize() already threw on it while building the column
//! list, so this is unreachable from the stream — but it must not silently pick a
//! framing.
bool WireTypeDiverges(const tds::ColumnMetadata &column, const LogicalType &target_type, bool &diverges) {
	LogicalType wire_type;
	try {
		wire_type = tds::encoding::TypeConverter::GetDuckDBType(column);
	} catch (const std::exception &) {
		return false;
	}
	// Spec 060: two VARCHARs never diverge from each other, whatever annotations
	// they carry. LogicalType equality compares the aux info too, so a catalog
	// column reporting the declared nvarchar(n) bound would otherwise "diverge"
	// from the plain VARCHAR the wire produces and render every value as text.
	// The guard's real question — is the catalog claiming a DIFFERENT type than
	// the wire sends — is unaffected: VARCHAR against INTEGER still differs by id.
	const bool same_type = wire_type == target_type ||
						   (wire_type.id() == LogicalTypeId::VARCHAR && target_type.id() == LogicalTypeId::VARCHAR);
	diverges = !same_type && !IsSupportedRetarget(column, target_type);
	return true;
}

//! A direct write stores wire bytes into the output vector at `width` stride,
//! so the wire width MUST equal the width DuckDB reserves per slot. The two
//! come from different files (FixedWireWidth here, GetDuckDBType in the type
//! converter) and today they agree for every direct type — TINYINT/BIT are one
//! byte in both, REAL/FLOAT four, and so on. Proving it here, once per column,
//! means a future remapping degrades to staging instead of writing 1 byte into
//! a 2-byte slot. Never for a diverging column.
bool StageDirect(const tds::ColumnMetadata &column, const LogicalType &target_type, uint32_t width, ColumnOps &ops) {
	if (ops.needs_value_fallback || width == 0 || !IsDirectWritable(column, width) ||
		width != GetTypeIdSize(target_type.InternalType())) {
		return false;
	}
	ops.kind = DirectKindForWidth(width);
	ops.arm = DirectArm(column.type_id, width);
	ops.stride = width;
	ops.direct_write = true;
	return true;
}

//! DECIMAL / NUMERIC of a precision the kernel takes: a sign byte and a mantissa sized by the precision.
bool StageDecimal(const tds::ColumnMetadata &column, ColumnOps &ops) {
	if (!IsOneOf(column.type_id, {tds::TDS_TYPE_DECIMAL, tds::TDS_TYPE_NUMERIC}) || column.precision == 0 ||
		column.precision > 38) {
		return false;
	}
	return ClaimFixed(ops, AppendArm::P1StageDecimal, DecimalWireWidth(column.precision));
}

//! Fixed-width families with a batch kernel: UNIQUEIDENTIFIER, the temporal types
//! and money. DATETIME, SMALLDATETIME, MONEY and SMALLMONEY arrive bare; everything
//! else carries the one-byte length prefix.
bool StageFixedFamily(const tds::ColumnMetadata &column, uint32_t width, ColumnOps &ops) {
	const uint32_t fixed_width = width > 0 ? width : TemporalWireWidth(column);
	if (fixed_width == 0 || !IsStageableFixedWidth(fixed_width)) {
		return false;
	}
	const bool bare = IsOneOf(column.type_id, {tds::TDS_TYPE_DATETIME, tds::TDS_TYPE_SMALLDATETIME, tds::TDS_TYPE_MONEY,
											   tds::TDS_TYPE_SMALLMONEY});
	const bool prefixed =
		IsOneOf(column.type_id,
				{tds::TDS_TYPE_UNIQUEIDENTIFIER, tds::TDS_TYPE_DATE, tds::TDS_TYPE_TIME, tds::TDS_TYPE_DATETIME2,
				 tds::TDS_TYPE_DATETIMEOFFSET, tds::TDS_TYPE_DATETIMEN, tds::TDS_TYPE_MONEYN});
	if (!bare && !prefixed) {
		return false;
	}
	return ClaimFixed(ops, bare ? AppendArm::RawStageFixed : AppendArm::P1StageFixed, fixed_width);
}

//! Any other fixed-width column, which is always claimed: staged, or Unsupported.
//!
//! A DIVERGING column reaches here (without a divergence a fixed-width type
//! is either direct-written or claimed by the staged-fixed families above),
//! and so does any *N variant whose declared width is not a width the staged
//! append can copy. It still needs an arm — leaving it Unsupported made the
//! issue-#89 fallback throw for exactly the integer, float and BIT types it
//! exists to rescue, instead of rendering them as text the way the pre-055
//! path did. The Text kernel reads Fixed staging positionally, so Fixed is
//! the right shape.
//!
//! The width has to be one AppendStagedFixed actually has a case for.
//! Without this test it took `stride` on trust and fell into the switch's
//! default, which copies SEVENTEEN bytes: a DATETIMEN declaring 52 then had
//! 17 bytes read out of a one-byte NULL value. No conforming server sends
//! such a width, so there is nothing to rescue — say so instead, and let the
//! error name the column when a value arrives.
bool StageOtherFixed(const tds::ColumnMetadata &column, uint32_t width, ColumnOps &ops) {
	if (width == 0) {
		return false;
	}
	if (!IsStageableFixedWidth(width)) {
		ops.arm = AppendArm::Unsupported;
		return true;
	}
	return ClaimFixed(ops, HasOneBytePrefix(column.type_id) ? AppendArm::P1StageFixed : AppendArm::RawStageFixed,
					  width);
}

//! SQL_VARIANT (spec 093 T1): the envelope as it arrives, at most SQL_VARIANT_MAX_BYTES a value.
bool StageVariant(const tds::ColumnMetadata &column, const LogicalType &target_type, ColumnOps &ops) {
	if (column.type_id != tds::TDS_TYPE_SQL_VARIANT || target_type.id() != LogicalTypeId::VARIANT) {
		return false;
	}
	return ClaimVar(ops, AppendArm::P4StageVariant, tds::SQL_VARIANT_MAX_BYTES);
}

//! A CLR type, always PLP (spec 093 T4, T5): hierarchyid into VARCHAR (its text), a user CLR type into
//! BLOB (its bytes), geometry / geography into a GEOMETRY (the WKB) or a BLOB (the WKB, or the stored
//! bytes under geometry_as 'native'). All stage the assembled bytes; ResolveKernel picks the kernel.
bool StageClr(const tds::ColumnMetadata &column, const LogicalType &target_type, ColumnOps &ops) {
	const auto target = target_type.id();
	const bool hierarchyid = column.IsHierarchyIdUdt() && target == LogicalTypeId::VARCHAR;
	const bool user_type = column.IsUserClrUdt() && target == LogicalTypeId::BLOB;
	const bool spatial = column.IsSpatialUdt() && (target == LogicalTypeId::GEOMETRY || target == LogicalTypeId::BLOB);
	if (!hierarchyid && !user_type && !spatial) {
		return false;
	}
	return ClaimVar(ops, AppendArm::PlpStageBinary);
}

//! The legacy LOBs. A catalog scan casts them server-side and never produces
//! them (spec 055 D9), but raw mssql_scan has no metadata to cast from — so
//! they are decoded natively too rather than left as the one shape the
//! extension cannot read. TEXT is single-byte like CHAR/VARCHAR: same bytes,
//! same kernels as IMAGE's.
bool StageLegacyLob(const tds::ColumnMetadata &column, ColumnOps &ops) {
	if (column.type_id == tds::TDS_TYPE_NTEXT) {
		return ClaimVar(ops, AppendArm::LobStageString);
	}
	if (IsOneOf(column.type_id, {tds::TDS_TYPE_TEXT, tds::TDS_TYPE_IMAGE})) {
		return ClaimVar(ops, AppendArm::LobStageBinary);
	}
	return false;
}

//! UTF-16 string columns take the batch decode (D5), MAX forms included: the
//! chunk list is assembled incrementally into the same staged layout.
//!
//! NCHAR is included: its trailing-space trim moves to the OUTPUT, where it is
//! equally correct on invalid UTF-16. The kernel derives that from the TDS
//! type itself, so the rule lives with the family that applies it.
bool StageUtf16(const tds::ColumnMetadata &column, ColumnOps &ops) {
	if (!IsOneOf(column.type_id, {tds::TDS_TYPE_NVARCHAR, tds::TDS_TYPE_NCHAR, tds::TDS_TYPE_XML})) {
		return false;
	}
	if (column.IsPLPType()) {
		// Same staged layout and the same finalize kernel as the bounded form;
		// only the framing differs, because the value arrives as a chunk list.
		return ClaimVar(ops, AppendArm::PlpStageString);
	}
	if (column.max_length == 0) {
		return false;
	}
	// +2 for the U+0000 the batch decode splits on: it is staged with the
	// value and is part of what one value COSTS in the buffer. Without it a
	// full chunk of maximum-length values overruns the "provable worst case"
	// by exactly two bytes per row, so the column both grows and keeps
	// claiming it cannot — which is what the D10 grow counter reported.
	return ClaimVar(ops, AppendArm::P2StageString, static_cast<uint32_t>(column.max_length) + 2);
}

//! Single-byte CHAR/VARCHAR and BINARY/VARBINARY: staged as they are, back to back
//! with no delimiter. ResolveKernel decides what publishes a CHAR/VARCHAR: the
//! code-page kernel (spec 092), or one allocation and one copy of the bytes, as for
//! VARBINARY, which has no conversion at all. The MAX forms are raw bytes either
//! way, so the same kernel as their bounded forms.
bool StageBytes(const tds::ColumnMetadata &column, ColumnOps &ops) {
	if (!IsOneOf(column.type_id, {tds::TDS_TYPE_BIGCHAR, tds::TDS_TYPE_BIGVARCHAR, tds::TDS_TYPE_BIGVARBINARY,
								  tds::TDS_TYPE_BIGBINARY})) {
		return false;
	}
	if (column.IsPLPType()) {
		return ClaimVar(ops, AppendArm::PlpStageBinary);
	}
	if (column.max_length == 0) {
		return false;
	}
	return ClaimVar(ops, AppendArm::P2StageBinary, static_cast<uint32_t>(column.max_length));
}

//! A column of fixed wire width (`width`, 0 for a decimal or a temporal type, which have their own).
bool StageFixedWidth(const tds::ColumnMetadata &column, const LogicalType &target_type, ColumnOps &ops) {
	const uint32_t width = FixedWireWidth(column);
	return StageDirect(column, target_type, width, ops) || StageDecimal(column, ops) ||
		   StageFixedFamily(column, width, ops) || StageOtherFixed(column, width, ops);
}

//! A column whose values vary in length.
bool StageVariableWidth(const tds::ColumnMetadata &column, const LogicalType &target_type, ColumnOps &ops) {
	return StageVariant(column, target_type, ops) || StageClr(column, target_type, ops) ||
		   StageLegacyLob(column, ops) || StageUtf16(column, ops) || StageBytes(column, ops);
}

//! The arm/kind/stride half of the resolution; ResolveColumnOps adds the kernel.
ColumnOps ResolveAppend(const tds::ColumnMetadata &column, const LogicalType &target_type) {
	ColumnOps ops;	// Unsupported until a step claims the column
	if (StageVector(column, target_type, ops)) {
		return ops;
	}
	if (!WireTypeDiverges(column, target_type, ops.needs_value_fallback)) {
		return ops;
	}
	// Nothing claiming it leaves it Unsupported. The types that end there are the
	// legacy LOBs (TEXT / NTEXT / IMAGE) when they arrive without the catalog's
	// server-side CAST, plus UDT and a SQL_VARIANT not read as a VARIANT — none of
	// which the shipped row reader can decode either. Say so instead of inventing a
	// framing.
	(void)(StageFixedWidth(column, target_type, ops) || StageVariableWidth(column, target_type, ops));
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
	case FinalizeKernel::CodePage:
		return "code_page";
	case FinalizeKernel::Variant:
		return "variant";
	case FinalizeKernel::Spatial:
		return "spatial";
	case FinalizeKernel::HierarchyId:
		return "hierarchyid";
	case FinalizeKernel::Vector:
		return "vector";
	}
	return "?";
}

ColumnOps ResolveColumnOps(const tds::ColumnMetadata &column, const LogicalType &target_type) {
	ColumnOps ops = ResolveAppend(column, target_type);
	ops.kernel = ResolveKernel(ops, column, target_type);
	return ops;
}

}  // namespace staging
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
