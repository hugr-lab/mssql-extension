#include "tds/encoding/bcp_row_encoder.hpp"

#include "codec/datetime_codec.hpp"
#include "codec/decimal_codec.hpp"
#include "codec/string_codec.hpp"
#include "codec/uuid_codec.hpp"
#include "codec/write_column_ops.hpp"
#include "mssql_counters.hpp"

#include "tds/tds_types.hpp"

#include "codec/binary_codec.hpp"
#include "codec/boolean_codec.hpp"
#include "codec/datetime_codec.hpp"
#include "codec/decimal_codec.hpp"
#include "codec/float_codec.hpp"
#include "codec/integer_codec.hpp"
#include "codec/string_codec.hpp"
#include "codec/type_family.hpp"
#include "codec/uuid_codec.hpp"
#include "codec/vector_format.hpp"
#include "copy/target_resolver.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/vector.hpp"
#include "tds/encoding/utf16.hpp"

#include <cstring>
#include <limits>
#include <type_traits>

namespace duckdb {
namespace tds {
namespace encoding {

// Days from 0001-01-01 to 1970-01-01 (Unix epoch)
constexpr int32_t DAYS_FROM_0001_TO_EPOCH = 719162;

// Microseconds per day
constexpr int64_t MICROS_PER_DAY = 86400000000LL;

// Money family is decode-only (DuckDB has no MONEY LogicalType — SQL Server
// MONEY/SMALLMONEY/MONEYN tokens surface as DECIMAL on decode). The Money arm
// in BCP encode is a compile-required exhaustiveness placeholder + runtime
// assertion: FamilyFromLogicalType never returns Money, so reaching this
// throw indicates the LogicalType→family mapping has drifted.
[[noreturn]] static void ThrowMoneyUnreachable(const LogicalType &type) {
	throw InternalException(
		"BCP encoder reached Money family arm for DuckDB type '%s' — "
		"Money is decode-only and has no LogicalType mapping",
		type.ToString());
}

//===----------------------------------------------------------------------===//
// W1+W2 (spec 054): per-column encode state, hoisted once per chunk.
//===----------------------------------------------------------------------===//

namespace {

// TDS BCP ROW token (written by EncodeChunk, one per row).
constexpr uint8_t BCP_TOKEN_ROW = 0xD1;

// Family encoder signature — the format-threaded codec overloads.
using EncodeFn = void (*)(Vector &, const UnifiedVectorFormat &, idx_t, const mssql::BCPColumnMetadata &,
						  vector<uint8_t> &);

// Wire form of NULL for a column, resolved once (was an IsPLPType /
// IsVariableLengthUSHORT branch pair per cell).
enum class NullWireKind : uint8_t { Plp, VariableUShort, Fixed };

[[noreturn]] static void EncodeMoneyUnreachable(Vector &vec, const UnifiedVectorFormat &, idx_t,
												const mssql::BCPColumnMetadata &, vector<uint8_t> &) {
	ThrowMoneyUnreachable(vec.GetType());
}

// W2: resolve the family encoder once per column (was a 9-way switch per row).
EncodeFn ResolveEncoder(const mssql::BCPColumnMetadata &col) {
	mssql::codec::TypeFamily family;
	try {
		family = mssql::codec::FamilyFromLogicalType(col.duckdb_type);
	} catch (const NotImplementedException &) {
		throw NotImplementedException("MSSQL: Unsupported type for BCP encoding: %s", col.duckdb_type.ToString());
	}
	switch (family) {
	case mssql::codec::TypeFamily::Boolean:
		return mssql::codec::boolean::EncodeToBcp;
	case mssql::codec::TypeFamily::Integer:
		return mssql::codec::integer::EncodeToBcp;
	case mssql::codec::TypeFamily::Float:
		return mssql::codec::float_family::EncodeToBcp;
	case mssql::codec::TypeFamily::Decimal:
		return mssql::codec::decimal::EncodeToBcp;
	case mssql::codec::TypeFamily::String:
		// Spec 060: a UTF-8 target takes the bytes as they are; anything else is
		// transcoded to UTF-16. Resolved HERE, once per column, so the row loop
		// carries no test — the same reason the 9-way family switch moved here.
		// Measured: a per-value test cost the untouched nvarchar path ~20 ns.
		if (col.tds_type_token == TDS_TYPE_BIGVARCHAR) {
			return static_cast<EncodeFn>(mssql::codec::string::EncodeToBcpUtf8);
		}
		return static_cast<EncodeFn>(mssql::codec::string::EncodeToBcp);
	case mssql::codec::TypeFamily::Binary:
		return mssql::codec::binary::EncodeToBcp;
	case mssql::codec::TypeFamily::Uuid:
		return mssql::codec::uuid::EncodeToBcp;
	case mssql::codec::TypeFamily::DateTime:
		return mssql::codec::datetime::EncodeToBcp;
	case mssql::codec::TypeFamily::Money:
		return EncodeMoneyUnreachable;
	}
	return EncodeMoneyUnreachable;	// unreachable — switch is exhaustive
}

struct ColumnEncodeState {
	Vector *vec = nullptr;	// nullptr → source column missing: NULL every row
	UnifiedVectorFormat fmt;
	EncodeFn encode = nullptr;
	NullWireKind null_kind = NullWireKind::Fixed;
};

void EncodeNullOfKind(vector<uint8_t> &buffer, NullWireKind kind) {
	switch (kind) {
	case NullWireKind::Plp:
		BCPRowEncoder::EncodeNullPLP(buffer);
		return;
	case NullWireKind::VariableUShort:
		BCPRowEncoder::EncodeNullVariable(buffer);
		return;
	case NullWireKind::Fixed:
		BCPRowEncoder::EncodeNullFixed(buffer);
		return;
	}
}

// Build the hoisted per-column state. `format_count` is the row count the
// UnifiedVectorFormat is computed for (chunk size for EncodeChunk; row+1 for
// the per-row EncodeRow compatibility path).
void PrepareColumnStates(DataChunk &chunk, idx_t format_count, const vector<mssql::BCPColumnMetadata> &columns,
						 const vector<int32_t> *column_mapping, vector<ColumnEncodeState> &states) {
	states.resize(columns.size());
	for (idx_t target_idx = 0; target_idx < columns.size(); target_idx++) {
		auto &col = columns[target_idx];
		auto &state = states[target_idx];
		state.null_kind = col.IsPLPType()
							  ? NullWireKind::Plp
							  : (col.IsVariableLengthUSHORT() ? NullWireKind::VariableUShort : NullWireKind::Fixed);
		// If source_idx is -1 or out of range, the column stays NULL for every row.
		int32_t source_idx = column_mapping ? (*column_mapping)[target_idx] : static_cast<int32_t>(target_idx);
		if (source_idx < 0 || static_cast<idx_t>(source_idx) >= chunk.ColumnCount()) {
			continue;
		}
		state.vec = &chunk.data[source_idx];
		state.vec->ToUnifiedFormat(format_count, state.fmt);
		state.encode = ResolveEncoder(col);
	}
}

// Encode one row from prepared states (no 0xD1 token byte).
void EncodeRowFromStates(vector<uint8_t> &buffer, idx_t row_idx, const vector<mssql::BCPColumnMetadata> &columns,
						 vector<ColumnEncodeState> &states) {
	for (idx_t target_idx = 0; target_idx < columns.size(); target_idx++) {
		auto &state = states[target_idx];
		if (!state.vec || mssql::codec::FormatIsNull(state.fmt, row_idx)) {
			EncodeNullOfKind(buffer, state.null_kind);
			continue;
		}
		state.encode(*state.vec, state.fmt, row_idx, columns[target_idx], buffer);
	}
}

}  // namespace

//===----------------------------------------------------------------------===//
// Chunk- and Row-Level Encoding
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Columnar scatter for fixed-width, direct-copy columns (spec 057 step 3)
//
// The row loop below hoists per-column state (spec 054 W1+W2) but still appends
// one value at a time through push_back, which costs a capacity check per byte —
// measured at ~10 ns/value on the string path, more than the UTF-16 conversion
// itself. This path removes it for the case where it is removable without any
// conversion at all: every column fixed-width, and its wire bytes a plain
// little-endian copy of what the vector already stores.
//
// The shape is: size the buffer from METADATA and the validity masks (never from
// a value), resize once, then walk COLUMNS writing through a raw pointer. That
// is the direction measured in test/cpp/bench_materialize.cpp — 8.14 -> 0.53
// ns/value at one column, 7.83 -> 0.36 at 44 with blocking.
//
// Deliberately narrow for now. DECIMAL, temporal and GUID need transformation
// kernels and variable-width families need staging; both are later commits, and
// until they exist those chunks take the row loop unchanged.
//
// Little-endian is assumed, as it already is by the byte-wise appenders this
// replaces and by DecodeFromTds's memcpy. Every platform this ships to is LE.
//===----------------------------------------------------------------------===//

// Whether a column has a selection vector is a COLUMN constant, and it decides
// whether the source walk is unit-stride. Left as `sel->get_index(r)` inside the
// loop it is a branch plus a possible load per value, and — worse — the compiler
// can no longer see the access as affine, so nothing downstream vectorises.
//
// Measured: the microbenchmark walking FlatVector data directly reached 0.88-1.12
// ns/value on the same shape where production measured 2.24-3.00. That gap is
// this indirection, and it was large enough to hide the cache effect blocking is
// meant to fix (spec 057, prototype 2026-08-03).
template <int W, bool HAS_SEL>
inline void ScatterDirectTyped(uint8_t *dst, size_t stride, idx_t row_begin, idx_t rows, const uint8_t *src,
							   const SelectionVector *sel) {
	for (idx_t r = 0; r < rows; r++) {
		uint8_t *out = dst + r * stride;
		*out = W;
		const idx_t idx = HAS_SEL ? sel->get_index_unsafe(row_begin + r) : row_begin + r;
		std::memcpy(out + 1, src + idx * W, W);
	}
}

template <int W>
inline void ScatterDirect(uint8_t *dst, size_t stride, idx_t row_begin, idx_t rows, const UnifiedVectorFormat &fmt) {
	const uint8_t *src = reinterpret_cast<const uint8_t *>(fmt.data);
	if (fmt.sel->IsSet()) {
		ScatterDirectTyped<W, true>(dst, stride, row_begin, rows, src, fmt.sel);
	} else {
		ScatterDirectTyped<W, false>(dst, stride, row_begin, rows, src, nullptr);
	}
}

// An integer whose source width differs from the target's (issue #153).
//
// The pair is a COLUMN property, so both the read width and the write width are
// template parameters and the loop carries no type test — that is the whole
// difference from the row path, which re-dispatched on the source's PhysicalType
// for every value even though the answer could not change within a column.
//
// Two facts keep the check off the widening path, which is the common one:
//
//   * a widening cannot fail, and `sizeof` is known at compile time, so the
//     `if` below is folded away entirely for TINYINT -> int, INTEGER -> bigint
//     and friends. Those become a sign-extending load and a store;
//   * a narrowing needs the check, but not a BRANCH. `bad` is an OR reduction
//     over the block, so the loop still vectorises; only after it does anything
//     look at the result.
//
// Refusing rather than wrapping matches the row path and is the deliberate
// choice: an integer that does not fit is a different number, and SQL Server
// refuses the row itself. (Contrast the string path, where the column declares a
// bound and truncating to it is the stated intent.)
template <class SRC, class DST, bool HAS_SEL>
inline bool ScatterConvTyped(uint8_t *dst, size_t stride, idx_t row_begin, idx_t rows, const SRC *src,
							 const SelectionVector *sel) {
	constexpr int W = sizeof(DST);
	bool bad = false;
	for (idx_t r = 0; r < rows; r++) {
		uint8_t *out = dst + r * stride;
		*out = W;
		const idx_t idx = HAS_SEL ? sel->get_index_unsafe(row_begin + r) : row_begin + r;
		const SRC v = src[idx];
		const DST n = static_cast<DST>(v);
		// Compile-time on both counts, so a widening and every float pair fold it
		// away: a float narrowing loses precision by the column's own request and
		// must not be refused, unlike an integer that becomes a different number.
		if (std::is_integral<SRC>::value && sizeof(SRC) > sizeof(DST)) {
			// Round-trip: exact iff the value survived the narrowing. Handles the
			// unsigned target too — a negative into `tinyint` comes back positive.
			bad |= (static_cast<SRC>(n) != v);
		}
		std::memcpy(out + 1, &n, W);
	}
	return !bad;
}

// Cold. The fast loop knows THAT a value did not fit, not which one, so name it
// with a rescan — this runs only on the way to throwing.
template <class SRC>
void ThrowIntRange(const SRC *src, const SelectionVector *sel, idx_t row_begin, idx_t rows, int64_t lo, int64_t hi,
				   const mssql::BCPColumnMetadata &col, const ValidityMask *mask) {
	for (idx_t r = 0; r < rows; r++) {
		const idx_t idx = sel ? sel->get_index(row_begin + r) : row_begin + r;
		if (mask && !mask->RowIsValid(idx)) {
			continue;
		}
		const int64_t v = static_cast<int64_t>(src[idx]);
		if (v < lo || v > hi) {
			throw InvalidInputException(
				"MSSQL: integer value %lld in column \"%s\" is out of range for the target column (accepts %lld..%lld)",
				(long long)v, col.name, (long long)lo, (long long)hi);
		}
	}
	throw InternalException("BCPRowEncoder: integer range check disagreed with the rescan");
}

// SQL Server's `tinyint` is UNSIGNED 0..255 while DuckDB's TINYINT is signed, so
// a CONVERTED value must satisfy the server's range — which is why width 1 binds
// DST to uint8_t below. (An exact-width TINYINT source never reaches this arm; it
// is a direct copy, and keeps the pre-existing signed passthrough.)
//
// The two-level switch is explicit rather than a generic lambda because this tree
// is compiled as C++11 — see the ODR note in CLAUDE.md. It runs ONCE per column
// per block, so its cost is not on the value path at all.
template <class SRC, class DST>
inline void ScatterConvT(uint8_t *dst, size_t stride, idx_t row_begin, idx_t rows, const UnifiedVectorFormat &fmt,
						 const mssql::BCPColumnMetadata &col) {
	const SRC *src = reinterpret_cast<const SRC *>(fmt.data);
	const bool has_sel = fmt.sel->IsSet();
	const bool ok = has_sel ? ScatterConvTyped<SRC, DST, true>(dst, stride, row_begin, rows, src, fmt.sel)
							: ScatterConvTyped<SRC, DST, false>(dst, stride, row_begin, rows, src, nullptr);
	if (!ok) {
		ThrowIntRange<SRC>(src, has_sel ? fmt.sel : nullptr, row_begin, rows, (int64_t)std::numeric_limits<DST>::min(),
						   (int64_t)std::numeric_limits<DST>::max(), col, nullptr);
	}
}

template <class DST>
inline void ScatterConvDst(uint8_t *dst, size_t stride, idx_t row_begin, idx_t rows, const UnifiedVectorFormat &fmt,
						   const mssql::BCPColumnMetadata &col, uint8_t src_width) {
	switch (src_width) {
	case 1:
		ScatterConvT<int8_t, DST>(dst, stride, row_begin, rows, fmt, col);
		return;
	case 2:
		ScatterConvT<int16_t, DST>(dst, stride, row_begin, rows, fmt, col);
		return;
	case 4:
		ScatterConvT<int32_t, DST>(dst, stride, row_begin, rows, fmt, col);
		return;
	default:
		ScatterConvT<int64_t, DST>(dst, stride, row_begin, rows, fmt, col);
		return;
	}
}

inline void ScatterConv(uint8_t *dst, size_t stride, idx_t row_begin, idx_t rows, const UnifiedVectorFormat &fmt,
						const mssql::codec::WriteColumnOps &op, const mssql::BCPColumnMetadata &col) {
	switch (op.wire_width) {
	case 1:
		ScatterConvDst<uint8_t>(dst, stride, row_begin, rows, fmt, col, op.src_width);
		return;
	case 2:
		ScatterConvDst<int16_t>(dst, stride, row_begin, rows, fmt, col, op.src_width);
		return;
	case 4:
		ScatterConvDst<int32_t>(dst, stride, row_begin, rows, fmt, col, op.src_width);
		return;
	default:
		ScatterConvDst<int64_t>(dst, stride, row_begin, rows, fmt, col, op.src_width);
		return;
	}
}

// FLOAT <-> DOUBLE. Only two pairs exist, and neither can fail — the range check
// inside ScatterConvTyped is compiled out for a non-integral source, so this
// needs no error path at all.
inline void ScatterConvFloat(uint8_t *dst, size_t stride, idx_t row_begin, idx_t rows, const UnifiedVectorFormat &fmt,
							 const mssql::codec::WriteColumnOps &op) {
	const bool has_sel = fmt.sel->IsSet();
	if (op.wire_width == 4) {
		const double *src = reinterpret_cast<const double *>(fmt.data);
		has_sel ? ScatterConvTyped<double, float, true>(dst, stride, row_begin, rows, src, fmt.sel)
				: ScatterConvTyped<double, float, false>(dst, stride, row_begin, rows, src, nullptr);
		return;
	}
	const float *src = reinterpret_cast<const float *>(fmt.data);
	has_sel ? ScatterConvTyped<float, double, true>(dst, stride, row_begin, rows, src, fmt.sel)
			: ScatterConvTyped<float, double, false>(dst, stride, row_begin, rows, src, nullptr);
}

// The NULL-bearing peer. Same compile-time pair, position from the cursor.
template <class SRC, class DST, bool HAS_SEL>
inline bool CursorConvTyped(uint8_t *dst, size_t *cursor, idx_t r0, idx_t rend, const SRC *src,
							const SelectionVector *sel, const ValidityMask &mask) {
	constexpr int W = sizeof(DST);
	bool bad = false;
	for (idx_t r = r0; r < rend; r++) {
		uint8_t *out = dst + cursor[r];
		const idx_t idx = HAS_SEL ? sel->get_index_unsafe(r) : r;
		if (!mask.RowIsValid(idx)) {
			*out = 0x00;
			cursor[r] += 1;
			continue;
		}
		const SRC v = src[idx];
		const DST n = static_cast<DST>(v);
		if (std::is_integral<SRC>::value && sizeof(SRC) > sizeof(DST)) {
			bad |= (static_cast<SRC>(n) != v);
		}
		*out = W;
		std::memcpy(out + 1, &n, W);
		cursor[r] += 1 + W;
	}
	return !bad;
}

template <class SRC, class DST>
inline void CursorConvT(uint8_t *dst, size_t *cursor, idx_t r0, idx_t rend, const UnifiedVectorFormat &fmt,
						const mssql::BCPColumnMetadata &col) {
	const SRC *src = reinterpret_cast<const SRC *>(fmt.data);
	const bool has_sel = fmt.sel->IsSet();
	const bool ok = has_sel ? CursorConvTyped<SRC, DST, true>(dst, cursor, r0, rend, src, fmt.sel, fmt.validity)
							: CursorConvTyped<SRC, DST, false>(dst, cursor, r0, rend, src, nullptr, fmt.validity);
	if (!ok) {
		ThrowIntRange<SRC>(src, has_sel ? fmt.sel : nullptr, r0, rend - r0, (int64_t)std::numeric_limits<DST>::min(),
						   (int64_t)std::numeric_limits<DST>::max(), col, &fmt.validity);
	}
}

template <class DST>
inline void CursorConvDst(uint8_t *dst, size_t *cursor, idx_t r0, idx_t rend, const UnifiedVectorFormat &fmt,
						  const mssql::BCPColumnMetadata &col, uint8_t src_width) {
	switch (src_width) {
	case 1:
		CursorConvT<int8_t, DST>(dst, cursor, r0, rend, fmt, col);
		return;
	case 2:
		CursorConvT<int16_t, DST>(dst, cursor, r0, rend, fmt, col);
		return;
	case 4:
		CursorConvT<int32_t, DST>(dst, cursor, r0, rend, fmt, col);
		return;
	default:
		CursorConvT<int64_t, DST>(dst, cursor, r0, rend, fmt, col);
		return;
	}
}

inline void CursorConv(uint8_t *dst, size_t *cursor, idx_t r0, idx_t rend, const UnifiedVectorFormat &fmt,
					   const mssql::codec::WriteColumnOps &op, const mssql::BCPColumnMetadata &col) {
	switch (op.wire_width) {
	case 1:
		CursorConvDst<uint8_t>(dst, cursor, r0, rend, fmt, col, op.src_width);
		return;
	case 2:
		CursorConvDst<int16_t>(dst, cursor, r0, rend, fmt, col, op.src_width);
		return;
	case 4:
		CursorConvDst<int32_t>(dst, cursor, r0, rend, fmt, col, op.src_width);
		return;
	default:
		CursorConvDst<int64_t>(dst, cursor, r0, rend, fmt, col, op.src_width);
		return;
	}
}

inline void CursorConvFloat(uint8_t *dst, size_t *cursor, idx_t r0, idx_t rend, const UnifiedVectorFormat &fmt,
							const mssql::codec::WriteColumnOps &op) {
	const bool has_sel = fmt.sel->IsSet();
	if (op.wire_width == 4) {
		const double *src = reinterpret_cast<const double *>(fmt.data);
		has_sel ? CursorConvTyped<double, float, true>(dst, cursor, r0, rend, src, fmt.sel, fmt.validity)
				: CursorConvTyped<double, float, false>(dst, cursor, r0, rend, src, nullptr, fmt.validity);
		return;
	}
	const float *src = reinterpret_cast<const float *>(fmt.data);
	has_sel ? CursorConvTyped<float, double, true>(dst, cursor, r0, rend, src, fmt.sel, fmt.validity)
			: CursorConvTyped<float, double, false>(dst, cursor, r0, rend, src, nullptr, fmt.validity);
}

// One block of rows, all columns.
//
// PRECONDITION: only callable when the chunk has NO variable-length column.
// `col_off += 1 + widths[c]` below must track the planner's
// `stride += 1 + widths[c]`, and the planner adds `stride += 2` for a variable
// column while setting its `widths[c] = 0` — so with one present the two walks
// diverge and this one runs past the row. The guard is `all_valid &&
// !has_variable` at the call site, three hundred lines from the arithmetic it
// protects, which is why it is written down here as well.
//
// Extracted so the blocked and whole-chunk cases are the same code with a
// different block size.
inline void ScatterBlock(uint8_t *bdst, size_t stride, idx_t row_begin, idx_t rows, idx_t ncols,
						 const vector<mssql::BCPColumnMetadata> &columns, vector<ColumnEncodeState> &states,
						 const vector<mssql::codec::WriteColumnOps> &ops, const vector<uint32_t> &widths) {
	size_t col_off = 1;
	for (idx_t c = 0; c < ncols; c++) {
		auto &st = states[c];
		// Dispatch OUTSIDE the row loop: each arm owns its own loop, so the inner
		// body carries no switch and stays vectorisable.
		switch (ops[c].arm) {
		case mssql::codec::ScatterArm::NullOnly:
			for (idx_t r = 0; r < rows; r++) {
				bdst[r * stride + col_off] = 0x00;
			}
			break;
		case mssql::codec::ScatterArm::Decimal:
			// One call per column per block. The loop lives in decimal_codec.cpp
			// where it specialises on the source width and inlines the per-value
			// step; a per-value call across a TU boundary would be un-inlinable,
			// which is the cost spec 058 identified as the one that measures.
			mssql::codec::decimal::ScatterChunkStrided(bdst + col_off, stride, row_begin, rows, *st.vec, st.fmt,
													   columns[c]);
			break;
		case mssql::codec::ScatterArm::Guid:
			mssql::codec::uuid::ScatterChunkStrided(bdst + col_off, stride, row_begin, rows, *st.vec, st.fmt,
													columns[c]);
			break;
		case mssql::codec::ScatterArm::Datetime:
			mssql::codec::datetime::ScatterChunkStrided(bdst + col_off, stride, row_begin, rows, *st.vec, st.fmt,
														columns[c]);
			break;
		// Width is a column constant too, so it belongs in the type rather than
		// in a variable the loop re-reads.
		case mssql::codec::ScatterArm::DirectCopy1:
			ScatterDirect<1>(bdst + col_off, stride, row_begin, rows, st.fmt);
			break;
		case mssql::codec::ScatterArm::DirectCopy2:
			ScatterDirect<2>(bdst + col_off, stride, row_begin, rows, st.fmt);
			break;
		case mssql::codec::ScatterArm::DirectCopy4:
			ScatterDirect<4>(bdst + col_off, stride, row_begin, rows, st.fmt);
			break;
		case mssql::codec::ScatterArm::DirectCopy8:
			ScatterDirect<8>(bdst + col_off, stride, row_begin, rows, st.fmt);
			break;
		case mssql::codec::ScatterArm::IntConvert:
			ScatterConv(bdst + col_off, stride, row_begin, rows, st.fmt, ops[c], columns[c]);
			break;
		case mssql::codec::ScatterArm::FloatConvert:
			ScatterConvFloat(bdst + col_off, stride, row_begin, rows, st.fmt, ops[c]);
			break;
		default:
			throw InternalException("BCPRowEncoder: unreachable scatter arm");
		}
		col_off += 1 + widths[c];
	}
}

// One block of rows on the NULL-bearing path. Same four properties as
// ScatterBlock — arm dispatched outside the row loop, width in the type,
// selection test hoisted — with the position coming from the cursor instead of a
// stride, and the cursor advanced by what was actually written.
template <int W, bool HAS_SEL>
inline void CursorDirectTyped(uint8_t *dst, size_t *cursor, idx_t r0, idx_t rend, const uint8_t *src,
							  const SelectionVector *sel, const ValidityMask &mask) {
	for (idx_t r = r0; r < rend; r++) {
		uint8_t *out = dst + cursor[r];
		const idx_t idx = HAS_SEL ? sel->get_index_unsafe(r) : r;
		if (!mask.RowIsValid(idx)) {
			*out = 0x00;
			cursor[r] += 1;
			continue;
		}
		*out = W;
		std::memcpy(out + 1, src + idx * W, W);
		cursor[r] += 1 + W;
	}
}

template <int W>
inline void CursorDirect(uint8_t *dst, size_t *cursor, idx_t r0, idx_t rend, const UnifiedVectorFormat &fmt) {
	const uint8_t *src = reinterpret_cast<const uint8_t *>(fmt.data);
	if (fmt.sel->IsSet()) {
		CursorDirectTyped<W, true>(dst, cursor, r0, rend, src, fmt.sel, fmt.validity);
	} else {
		CursorDirectTyped<W, false>(dst, cursor, r0, rend, src, nullptr, fmt.validity);
	}
}

inline void ScatterBlockCursor(uint8_t *dst, size_t *cursor, idx_t r0, idx_t rend, idx_t ncols,
							   const vector<mssql::BCPColumnMetadata> &columns, vector<ColumnEncodeState> &states,
							   const vector<mssql::codec::WriteColumnOps> &ops, const vector<uint32_t> &widths,
							   const vector<mssql::codec::string::StringColumnPlan> &plans) {
	for (idx_t c = 0; c < ncols; c++) {
		auto &st = states[c];
		const uint32_t w = widths[c];
		if (ops[c].IsVariable()) {
			mssql::codec::string::ScatterVarBlock(dst, cursor, r0, rend, st.fmt, columns[c], plans[c]);
			continue;
		}
		if (!st.vec) {
			for (idx_t r = r0; r < rend; r++) {
				dst[cursor[r]] = 0x00;
				cursor[r] += 1;
			}
			continue;
		}
		// An all-valid column inside a NULL-bearing chunk still writes every
		// payload — it just cannot use a stride, because OTHER columns moved the
		// rows apart. Its inner loop carries no validity test at all.
		const bool col_all_valid = st.fmt.validity.AllValid();
		switch (ops[c].arm) {
		case mssql::codec::ScatterArm::DirectCopy1:
			CursorDirect<1>(dst, cursor, r0, rend, st.fmt);
			break;
		case mssql::codec::ScatterArm::DirectCopy2:
			CursorDirect<2>(dst, cursor, r0, rend, st.fmt);
			break;
		case mssql::codec::ScatterArm::DirectCopy4:
			CursorDirect<4>(dst, cursor, r0, rend, st.fmt);
			break;
		case mssql::codec::ScatterArm::DirectCopy8:
			CursorDirect<8>(dst, cursor, r0, rend, st.fmt);
			break;
		case mssql::codec::ScatterArm::IntConvert:
			CursorConv(dst, cursor, r0, rend, st.fmt, ops[c], columns[c]);
			break;
		case mssql::codec::ScatterArm::FloatConvert:
			CursorConvFloat(dst, cursor, r0, rend, st.fmt, ops[c]);
			break;
		case mssql::codec::ScatterArm::Guid:
			// One call per COLUMN (spec 064 D1). This used to loop here and call
			// the strided kernel with `rows = 1` per value — 2048 un-inlinable
			// calls across a TU boundary, for no reason in the data: the width is
			// fixed and the offsets were already in `cursor`. The kernel now takes
			// the cursor itself and handles NULLs, because on this path a NULL is
			// what makes the rows differ in length.
			mssql::codec::uuid::ScatterChunkCursor(dst, cursor, r0, rend - r0, *st.vec, st.fmt, columns[c],
												   col_all_valid);
			break;
		case mssql::codec::ScatterArm::Decimal:
		case mssql::codec::ScatterArm::Datetime: {
			// Still per value; their cursor entry points are the next step of D1.
			const auto arm = ops[c].arm;
			for (idx_t r = r0; r < rend; r++) {
				uint8_t *out = dst + cursor[r];
				const idx_t idx = st.fmt.sel->get_index(r);
				if (!col_all_valid && !st.fmt.validity.RowIsValid(idx)) {
					*out = 0x00;
					cursor[r] += 1;
					continue;
				}
				if (arm == mssql::codec::ScatterArm::Decimal) {
					mssql::codec::decimal::ScatterChunkStrided(out, 0, r, 1, *st.vec, st.fmt, columns[c]);
				} else {
					mssql::codec::datetime::ScatterChunkStrided(out, 0, r, 1, *st.vec, st.fmt, columns[c]);
				}
				cursor[r] += 1 + w;
			}
			break;
		}
		default:
			throw InternalException("BCPRowEncoder: unreachable cursor arm");
		}
	}
}

//! Encode the whole chunk as a columnar scatter. Returns false if any column
//! lacks a scatter arm, having written nothing.
bool TryEncodeChunkColumnar(vector<uint8_t> &buffer, idx_t row_count, const vector<mssql::BCPColumnMetadata> &columns,
							vector<ColumnEncodeState> &states) {
	const idx_t ncols = columns.size();
	if (ncols == 0) {
		return false;
	}

	vector<mssql::codec::WriteColumnOps> ops(ncols);
	vector<uint32_t> widths(ncols);
	// Planned per variable column, so the wire can be sized before a byte is
	// written. Indexed by column; empty for fixed-width ones.
	vector<mssql::codec::string::StringColumnPlan> plans(ncols);
	size_t stride = 1;	// the 0xD1 ROW token
	bool all_valid = true;
	bool has_variable = false;
	for (idx_t c = 0; c < ncols; c++) {
		// A missing source column is NULL every row: no payload, so it scatters
		// for free. Everything else answers to ResolveWriteColumnOps, which is
		// the single place that decides what this path can do.
		if (!states[c].vec) {
			ops[c].arm = mssql::codec::ScatterArm::NullOnly;
			widths[c] = 0;
			stride += 1;
			continue;
		}
		ops[c] = mssql::codec::ResolveWriteColumnOps(states[c].vec->GetType(), columns[c]);
		if (!ops[c].CanScatter()) {
			if (mssql::CountersEnabled()) {
				auto &pc = mssql::GetEncodePathCounters();
				pc.chunks_row_major.fetch_add(1, std::memory_order_relaxed);
				pc.fallback_unsupported_pair.fetch_add(1, std::memory_order_relaxed);
				pc.last_fallback_column.store(c, std::memory_order_relaxed);
				pc.last_fallback_arm.store(static_cast<uint64_t>(ops[c].arm), std::memory_order_relaxed);
			}
			return false;
		}
		if (ops[c].IsVariable()) {
			// Measure it now: PlanColumn walks the column once and keeps the
			// per-value payload length that the per-value encoder used to compute
			// and throw away. A column it cannot plan (invalid UTF-8, an
			// unhandled wire form) drops the whole chunk to the row path, which
			// reproduces the legacy bytes exactly.
			if (!mssql::codec::string::PlanColumn(*states[c].vec, states[c].fmt, 0, row_count, columns[c], plans[c])) {
				if (mssql::CountersEnabled()) {
					auto &pc = mssql::GetEncodePathCounters();
					pc.chunks_row_major.fetch_add(1, std::memory_order_relaxed);
					pc.fallback_string_plan.fetch_add(1, std::memory_order_relaxed);
					pc.last_fallback_column.store(c, std::memory_order_relaxed);
				}
				return false;
			}
			has_variable = true;
			widths[c] = 0;
			stride += 2;  // the length prefix; the payload is per row
			continue;
		}
		// uint32_t, matching wire_width, so this cannot narrow. It was a uint8_t
		// cast, safe only by coincidence of the current arm set — every fixed arm
		// today is <= 17 bytes, and the several `wire_width = target.max_length`
		// assignments that could exceed 255 all land on arms CanScatter() already
		// rejected. Add one strided fixed-width arm for binary(n)/char(n) — a
		// natural next step, and max_length is already assigned in those branches
		// — and the cast truncates silently, stride comes out short, and the
		// kernel writes the full width into it. Widening costs nothing: widths is
		// indexed per column, not per value.
		widths[c] = ops[c].wire_width;
		stride += 1 + widths[c];
		if (states[c].vec && !states[c].fmt.validity.AllValid()) {
			all_valid = false;
		}
	}

	// Sizing reads metadata and the masks only — never a value. A NULL keeps its
	// 1-byte marker and drops its payload, so it can only make a row shorter.
	const size_t base = buffer.size();
	vector<size_t> row_at;
	size_t total;
	if (all_valid && !has_variable) {
		total = row_count * stride;
	} else {
		// Per COLUMN, not per row x column. The old shape ran `rows * ncols`
		// iterations with a validity lookup and a branch in every one; this runs
		// one pass per column that HAS a NULL, and a column with none contributes
		// a constant that is folded into the base. Same answer, and the work is
		// proportional to the number of nullable columns rather than to all of
		// them.
		row_at.resize(row_count);
		size_t base_row = 1;  // the 0xD1 token
		for (idx_t c = 0; c < ncols; c++) {
			// A variable column contributes NOTHING to the constant part: its plan
			// carries the total wire bytes per row, framing included, and those
			// are added below. Splitting framing between here and there is what
			// produced a one-byte-short row the first time, visible only as the
			// server reporting "premature end-of-message".
			base_row += ops[c].IsVariable() ? 0 : (1 + widths[c]);
		}
		for (idx_t r = 0; r < row_count; r++) {
			row_at[r] = base_row;
		}
		for (idx_t c = 0; c < ncols; c++) {
			auto &st = states[c];
			if (ops[c].IsVariable()) {
				// Framing included — one number per row, whatever the wire form.
				const auto &wire = plans[c].wire;
				for (idx_t r = 0; r < row_count; r++) {
					row_at[r] += wire[r];
				}
				continue;
			}
			if (!st.vec) {
				// Absent column: NULL on every row, so its payload never appears.
				for (idx_t r = 0; r < row_count; r++) {
					row_at[r] -= widths[c];
				}
				continue;
			}
			if (st.fmt.validity.AllValid()) {
				continue;  // contributes its full width to every row already
			}
			const uint32_t w = widths[c];
			if (st.fmt.sel->IsSet()) {
				const auto *sel = st.fmt.sel;
				for (idx_t r = 0; r < row_count; r++) {
					row_at[r] -= st.fmt.validity.RowIsValid(sel->get_index_unsafe(r)) ? 0 : w;
				}
			} else {
				for (idx_t r = 0; r < row_count; r++) {
					row_at[r] -= st.fmt.validity.RowIsValid(r) ? 0 : w;
				}
			}
		}
		// Prefix-sum the per-row sizes into offsets, in place.
		size_t acc = 0;
		for (idx_t r = 0; r < row_count; r++) {
			const size_t sz = row_at[r];
			row_at[r] = acc;
			acc += sz;
		}
		total = acc;
	}

	buffer.resize(base + total);
	uint8_t *const dst = buffer.data() + base;

	// One variable-width column makes every row a different length, so the
	// constant-stride path is unavailable for EVERY column in the chunk — NULLs
	// or not.
	if (mssql::CountersEnabled()) {
		auto &pc = mssql::GetEncodePathCounters();
		if (all_valid && !has_variable) {
			pc.chunks_strided.fetch_add(1, std::memory_order_relaxed);
		} else {
			pc.chunks_cursor.fetch_add(1, std::memory_order_relaxed);
		}
	}
	if (all_valid && !has_variable) {
		// Rows in BLOCKS; within a block, walk the columns. A full per-column pass
		// over a wide row touches a distinct cache line per store and then walks
		// the same lines again for the next column: measured 0.40 ns/value at 4
		// columns but 1.12 at 16 and 0.88 at 44, while blocked stays 0.38-0.42
		// flat across every width (spec 057 prototype, 2026-08-03). The block's
		// slice of the wire and the columns' sources stay resident together.
		//
		// Block size is a plateau from 32 to 256; 128 sits in the middle of it.
		// Row-major assembly was measured WORSE (1.14-1.29 at width) — it makes
		// the write pattern ideal and the read pattern terrible, so the cache
		// problem moves rather than disappearing. Do not retry it.
		constexpr idx_t SCATTER_BLOCK_ROWS = 128;
		for (idx_t r0 = 0; r0 < row_count; r0 += SCATTER_BLOCK_ROWS) {
			const idx_t rend = MinValue<idx_t>(r0 + SCATTER_BLOCK_ROWS, row_count);
			const idx_t block_rows = rend - r0;
			uint8_t *const bdst = dst + r0 * stride;
			for (idx_t r = 0; r < block_rows; r++) {
				bdst[r * stride] = BCP_TOKEN_ROW;
			}
			ScatterBlock(bdst, stride, r0, block_rows, ncols, columns, states, ops, widths);
		}
		return true;
	}

	// NULL-bearing rows are not a constant stride, so a per-row cursor carries the
	// position. That much is inherent: a NULL writes a 1-byte marker instead of
	// 1+W, the wire is packed, and there is no formula for where row r starts.
	//
	// What is NOT inherent is that this path used to be slow. Measured across NULL
	// density 0/1/10/30/50/70/90/100%, it cost 6.5-7.5 ns/value at EVERY non-zero
	// density against 1.49 at zero — a 4.4x cliff at the first NULL and then flat.
	// Density-shaped explanations are therefore all wrong (branch misprediction
	// would peak at 50%; at 100% we write nine times fewer bytes and it still cost
	// four times more). The cost was simply that this branch had received none of
	// the four things the all-valid branch had: blocking, the width in the type,
	// the selection test hoisted, and the arm dispatched outside the row loop.
	//
	// Going row-major instead would remove the cursor entirely, and was measured:
	// even blocked it is 1.6-2.7x WORSE, because a row touches one cache line per
	// column and uses an eighth of each. The cursor is the price of the right read
	// order, and it is one L1 load/add/store per value.
	for (idx_t r = 0; r < row_count; r++) {
		dst[row_at[r]] = BCP_TOKEN_ROW;
	}
	vector<size_t> cursor(row_count);
	for (idx_t r = 0; r < row_count; r++) {
		cursor[r] = row_at[r] + 1;
	}
	constexpr idx_t CURSOR_BLOCK_ROWS = 128;
	for (idx_t r0 = 0; r0 < row_count; r0 += CURSOR_BLOCK_ROWS) {
		const idx_t rend = MinValue<idx_t>(r0 + CURSOR_BLOCK_ROWS, row_count);
		ScatterBlockCursor(dst, cursor.data(), r0, rend, ncols, columns, states, ops, widths, plans);
	}
	return true;
}

void BCPRowEncoder::EncodeChunk(vector<uint8_t> &buffer, DataChunk &chunk,
								const vector<mssql::BCPColumnMetadata> &columns,
								const vector<int32_t> *column_mapping) {
	const idx_t row_count = chunk.size();
	if (row_count == 0) {
		return;
	}

	// W4 (spec 054): reserve the accumulator once per chunk from a cheap
	// per-row estimate, so the per-value appends below rarely reallocate.
	// Fixed-width columns are exact (+1 length byte); variable/PLP columns
	// use a modest payload guess — an under-estimate only costs a vector
	// grow, an exact max_length bound could over-reserve by orders of
	// magnitude (2048 rows x nvarchar(4000) = 16 MB).
	size_t per_row_estimate = 1;  // 0xD1 ROW token
	for (auto &col : columns) {
		if (col.IsPLPType()) {
			per_row_estimate += 8 + 4 + 64 + 4;
		} else if (col.IsVariableLengthUSHORT()) {
			per_row_estimate += 2 + MinValue<size_t>(col.max_length, 64);
		} else {
			per_row_estimate += 1 + col.max_length;
		}
	}
	// Amplify to at least 2x the current capacity: reserve() allocates
	// exactly what is requested, so asking for size+estimate every chunk
	// would make capacity track size and memcpy the whole accumulator per
	// chunk (quadratic across a large batch).
	const size_t needed = buffer.size() + row_count * per_row_estimate;
	if (needed > buffer.capacity()) {
		buffer.reserve(MaxValue<size_t>(needed, 2 * buffer.capacity()));
	}

	vector<ColumnEncodeState> states;
	PrepareColumnStates(chunk, row_count, columns, column_mapping, states);
	if (TryEncodeChunkColumnar(buffer, row_count, columns, states)) {
		return;
	}
	for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
		buffer.push_back(BCP_TOKEN_ROW);
		EncodeRowFromStates(buffer, row_idx, columns, states);
	}
}

void BCPRowEncoder::EncodeRow(vector<uint8_t> &buffer, DataChunk &chunk, idx_t row_idx,
							  const vector<mssql::BCPColumnMetadata> &columns, const vector<int32_t> *column_mapping) {
	vector<ColumnEncodeState> states;
	PrepareColumnStates(chunk, row_idx + 1, columns, column_mapping, states);
	EncodeRowFromStates(buffer, row_idx, columns, states);
}

void BCPRowEncoder::EncodeValue(vector<uint8_t> &buffer, const Value &value, const mssql::BCPColumnMetadata &col) {
	if (value.IsNull()) {
		if (col.IsPLPType()) {
			EncodeNullPLP(buffer);
		} else if (col.IsVariableLengthUSHORT()) {
			EncodeNullVariable(buffer);
		} else {
			EncodeNullFixed(buffer);
		}
		return;
	}

	// Family-level dispatch — see EncodeRow above for the same pattern.
	mssql::codec::TypeFamily family;
	try {
		family = mssql::codec::FamilyFromLogicalType(col.duckdb_type);
	} catch (const NotImplementedException &) {
		throw NotImplementedException("MSSQL: Unsupported type for BCP encoding: %s", col.duckdb_type.ToString());
	}
	switch (family) {
	case mssql::codec::TypeFamily::Boolean:
		mssql::codec::boolean::EncodeToBcp(value, col, buffer);
		break;
	case mssql::codec::TypeFamily::Integer:
		mssql::codec::integer::EncodeToBcp(value, col, buffer);
		break;
	case mssql::codec::TypeFamily::Float:
		mssql::codec::float_family::EncodeToBcp(value, col, buffer);
		break;
	case mssql::codec::TypeFamily::Decimal:
		mssql::codec::decimal::EncodeToBcp(value, col, buffer);
		break;
	case mssql::codec::TypeFamily::String:
		mssql::codec::string::EncodeToBcp(value, col, buffer);
		break;
	case mssql::codec::TypeFamily::Binary:
		mssql::codec::binary::EncodeToBcp(value, col, buffer);
		break;
	case mssql::codec::TypeFamily::Uuid:
		mssql::codec::uuid::EncodeToBcp(value, col, buffer);
		break;
	case mssql::codec::TypeFamily::DateTime:
		mssql::codec::datetime::EncodeToBcp(value, col, buffer);
		break;
	case mssql::codec::TypeFamily::Money:
		ThrowMoneyUnreachable(col.duckdb_type);
	}
}

//===----------------------------------------------------------------------===//
// Integer Types (INTNTYPE 0x26)
//===----------------------------------------------------------------------===//

void BCPRowEncoder::EncodeInt8(vector<uint8_t> &buffer, int8_t value) {
	buffer.push_back(1);  // length
	buffer.push_back(static_cast<uint8_t>(value));
}

void BCPRowEncoder::EncodeInt16(vector<uint8_t> &buffer, int16_t value) {
	buffer.push_back(2);  // length
	buffer.push_back(static_cast<uint8_t>(value & 0xFF));
	buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void BCPRowEncoder::EncodeInt32(vector<uint8_t> &buffer, int32_t value) {
	buffer.push_back(4);  // length
	for (int i = 0; i < 4; i++) {
		buffer.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
	}
}

void BCPRowEncoder::EncodeInt64(vector<uint8_t> &buffer, int64_t value) {
	buffer.push_back(8);  // length
	for (int i = 0; i < 8; i++) {
		buffer.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
	}
}

void BCPRowEncoder::EncodeUInt8(vector<uint8_t> &buffer, uint8_t value) {
	buffer.push_back(1);  // length
	buffer.push_back(value);
}

//===----------------------------------------------------------------------===//
// Bit Type (BITNTYPE 0x68)
//===----------------------------------------------------------------------===//

void BCPRowEncoder::EncodeBit(vector<uint8_t> &buffer, bool value) {
	buffer.push_back(1);  // length
	buffer.push_back(value ? 0x01 : 0x00);
}

//===----------------------------------------------------------------------===//
// Float Types (FLTNTYPE 0x6D)
//===----------------------------------------------------------------------===//

void BCPRowEncoder::EncodeFloat(vector<uint8_t> &buffer, float value) {
	buffer.push_back(4);  // length
	uint32_t bits;
	memcpy(&bits, &value, sizeof(bits));
	for (int i = 0; i < 4; i++) {
		buffer.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFF));
	}
}

void BCPRowEncoder::EncodeDouble(vector<uint8_t> &buffer, double value) {
	buffer.push_back(8);  // length
	uint64_t bits;
	memcpy(&bits, &value, sizeof(bits));
	for (int i = 0; i < 8; i++) {
		buffer.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFF));
	}
}

//===----------------------------------------------------------------------===//
// Decimal Type (DECIMALNTYPE 0x6A)
//===----------------------------------------------------------------------===//

void BCPRowEncoder::EncodeDecimal(vector<uint8_t> &buffer, const hugeint_t &value, uint8_t precision, uint8_t scale) {
	// Determine the byte size based on precision
	uint8_t byte_size = GetDecimalByteSize(precision);
	buffer.push_back(byte_size);

	// Determine sign and absolute value
	bool is_negative = value.upper < 0;
	hugeint_t abs_value = is_negative ? Hugeint::Negate(value) : value;

	// Write sign byte: 0x00 = negative, 0x01 = non-negative
	buffer.push_back(is_negative ? 0x00 : 0x01);

	// Write mantissa as little-endian
	// For decimal(p,s), mantissa size is byte_size - 1 (excluding sign byte)
	uint8_t mantissa_size = byte_size - 1;

	// Convert hugeint to bytes (little-endian)
	for (uint8_t i = 0; i < mantissa_size; i++) {
		uint8_t byte_val;
		if (i < 8) {
			byte_val = static_cast<uint8_t>((abs_value.lower >> (i * 8)) & 0xFF);
		} else {
			byte_val = static_cast<uint8_t>((static_cast<uint64_t>(abs_value.upper) >> ((i - 8) * 8)) & 0xFF);
		}
		buffer.push_back(byte_val);
	}
}

//===----------------------------------------------------------------------===//
// Binary Data (BIGVARBINARYTYPE 0xA5)
//===----------------------------------------------------------------------===//

void BCPRowEncoder::EncodeBinary(vector<uint8_t> &buffer, const string_t &value) {
	uint16_t len = static_cast<uint16_t>(value.GetSize());

	// Write length as USHORT (little-endian)
	buffer.push_back(static_cast<uint8_t>(len & 0xFF));
	buffer.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));

	// Write bytes
	const uint8_t *data = reinterpret_cast<const uint8_t *>(value.GetData());
	buffer.insert(buffer.end(), data, data + len);
}

//===----------------------------------------------------------------------===//
// PLP (Partially Length-prefixed) Encoding for MAX types
//===----------------------------------------------------------------------===//

void BCPRowEncoder::EncodeBinaryPLP(vector<uint8_t> &buffer, const string_t &value) {
	// Use UNKNOWN_PLP_LEN (0xFFFFFFFFFFFFFFFE) instead of actual length
	// This is how Microsoft BCP and FreeTDS handle varbinary(max) in bulk load
	constexpr uint64_t UNKNOWN_PLP_LEN = 0xFFFFFFFFFFFFFFFEULL;
	for (int i = 0; i < 8; i++) {
		buffer.push_back(static_cast<uint8_t>((UNKNOWN_PLP_LEN >> (i * 8)) & 0xFF));
	}

	// Handle empty binary: PLP with no chunks, just terminator
	// PLP chunks must have length > 0, so empty binary = no chunks
	uint32_t chunk_len = static_cast<uint32_t>(value.GetSize());
	if (chunk_len > 0) {
		// Write single chunk: chunk length (4 bytes) + data
		for (int i = 0; i < 4; i++) {
			buffer.push_back(static_cast<uint8_t>((chunk_len >> (i * 8)) & 0xFF));
		}
		const uint8_t *data = reinterpret_cast<const uint8_t *>(value.GetData());
		buffer.insert(buffer.end(), data, data + chunk_len);
	}

	// Write terminator (4 bytes of 0x00) to signal end of PLP chunks
	buffer.push_back(0x00);
	buffer.push_back(0x00);
	buffer.push_back(0x00);
	buffer.push_back(0x00);
}

//===----------------------------------------------------------------------===//
// GUID (GUIDTYPE 0x24)
//===----------------------------------------------------------------------===//

void BCPRowEncoder::EncodeGUID(vector<uint8_t> &buffer, const hugeint_t &uuid) {
	// Write length (always 16 for GUID)
	buffer.push_back(16);

	// DuckDB stores UUID with high bit flipped for sortability
	// We need to unflip it first
	uint64_t upper = static_cast<uint64_t>(uuid.upper) ^ (uint64_t(1) << 63);
	uint64_t lower = uuid.lower;

	// Convert big-endian UUID to TDS mixed-endian GUID format
	// Standard UUID (big-endian): bytes 0-3=Data1, 4-5=Data2, 6-7=Data3, 8-15=Data4
	// TDS GUID (mixed-endian): Data1 LE, Data2 LE, Data3 LE, Data4 BE

	// Extract bytes in big-endian order
	uint8_t be_bytes[16];
	for (int i = 0; i < 8; i++) {
		be_bytes[i] = static_cast<uint8_t>((upper >> (56 - i * 8)) & 0xFF);
		be_bytes[i + 8] = static_cast<uint8_t>((lower >> (56 - i * 8)) & 0xFF);
	}

	// Write Data1 (bytes 0-3) as little-endian
	buffer.push_back(be_bytes[3]);
	buffer.push_back(be_bytes[2]);
	buffer.push_back(be_bytes[1]);
	buffer.push_back(be_bytes[0]);

	// Write Data2 (bytes 4-5) as little-endian
	buffer.push_back(be_bytes[5]);
	buffer.push_back(be_bytes[4]);

	// Write Data3 (bytes 6-7) as little-endian
	buffer.push_back(be_bytes[7]);
	buffer.push_back(be_bytes[6]);

	// Write Data4 (bytes 8-15) as-is (big-endian)
	for (int i = 8; i < 16; i++) {
		buffer.push_back(be_bytes[i]);
	}
}

//===----------------------------------------------------------------------===//
// Date/Time Types
//===----------------------------------------------------------------------===//

void BCPRowEncoder::EncodeDate(vector<uint8_t> &buffer, date_t value) {
	// DATE: 3 bytes unsigned little-endian, days since 0001-01-01
	// Convert from DuckDB date_t (days since 1970-01-01)
	int32_t days = value.days + DAYS_FROM_0001_TO_EPOCH;

	// Write length
	buffer.push_back(3);

	// Write 3 bytes little-endian
	buffer.push_back(static_cast<uint8_t>(days & 0xFF));
	buffer.push_back(static_cast<uint8_t>((days >> 8) & 0xFF));
	buffer.push_back(static_cast<uint8_t>((days >> 16) & 0xFF));
}

void BCPRowEncoder::EncodeTime(vector<uint8_t> &buffer, dtime_t value, uint8_t scale) {
	// TIME: 3-5 bytes depending on scale, stored as 10^(-scale) seconds since midnight
	// DuckDB dtime_t is microseconds since midnight

	// Convert microseconds to the appropriate scale
	int64_t scaled_value;
	if (scale <= 6) {
		// ROUND, and saturate at the last tick of the day — not truncate.
		//
		// This truncated, while the columnar kernel rounds, so the same TIME value
		// loaded as .123 or .124 depending on whether some OTHER column in the
		// chunk happened to force the row path. Spec 057 taught datetime2 to round
		// (rounding is what SQL Server does: CAST('12:34:56.600' AS time(0)) is
		// 12:34:57) and this peer was missed.
		//
		// The saturation is the half a TIME cannot solve by carrying, having no
		// date field: CAST('23:59:59.999999' AS time(0)) is 23:59:59 on the
		// server, so rounding must stop at per_day - 1 rather than reach 86400.
		int64_t divisor = 1;
		for (int i = 0; i < 6 - scale; i++) {
			divisor *= 10;
		}
		scaled_value = (value.micros + divisor / 2) / divisor;
		int64_t per_day = Interval::SECS_PER_DAY;
		for (int i = 0; i < scale; i++) {
			per_day *= 10;
		}
		if (scaled_value >= per_day) {
			scaled_value = per_day - 1;
		}
	} else {
		// Multiply for scale 7 (100ns units)
		scaled_value = value.micros * 10;
	}

	uint8_t byte_size = GetTimeByteSize(scale);
	buffer.push_back(byte_size);

	// Write little-endian bytes
	for (uint8_t i = 0; i < byte_size; i++) {
		buffer.push_back(static_cast<uint8_t>((scaled_value >> (i * 8)) & 0xFF));
	}
}

void BCPRowEncoder::EncodeDatetime2Raw(vector<uint8_t> &buffer, uint64_t time_value, uint32_t date_value,
									   uint8_t scale) {
	uint8_t time_size = GetTimeByteSize(scale);
	uint8_t total_size = time_size + 3;

	buffer.push_back(total_size);

	// Write time portion (little-endian)
	for (uint8_t i = 0; i < time_size; i++) {
		buffer.push_back(static_cast<uint8_t>((time_value >> (i * 8)) & 0xFF));
	}

	// Write date portion (3 bytes little-endian)
	buffer.push_back(static_cast<uint8_t>(date_value & 0xFF));
	buffer.push_back(static_cast<uint8_t>((date_value >> 8) & 0xFF));
	buffer.push_back(static_cast<uint8_t>((date_value >> 16) & 0xFF));
}

void BCPRowEncoder::EncodeDatetime2(vector<uint8_t> &buffer, timestamp_t ts, uint8_t scale) {
	uint64_t time_value;
	uint32_t date_value;
	TimestampToDatetime2Components(ts, scale, time_value, date_value);
	EncodeDatetime2Raw(buffer, time_value, date_value, scale);
}

void BCPRowEncoder::EncodeDatetimeOffsetRaw(vector<uint8_t> &buffer, uint64_t time_value, uint32_t date_value,
											int16_t offset_minutes, uint8_t scale) {
	uint8_t time_size = GetTimeByteSize(scale);
	uint8_t total_size = time_size + 3 + 2;

	buffer.push_back(total_size);

	for (uint8_t i = 0; i < time_size; i++) {
		buffer.push_back(static_cast<uint8_t>((time_value >> (i * 8)) & 0xFF));
	}
	buffer.push_back(static_cast<uint8_t>(date_value & 0xFF));
	buffer.push_back(static_cast<uint8_t>((date_value >> 8) & 0xFF));
	buffer.push_back(static_cast<uint8_t>((date_value >> 16) & 0xFF));
	buffer.push_back(static_cast<uint8_t>(offset_minutes & 0xFF));
	buffer.push_back(static_cast<uint8_t>((offset_minutes >> 8) & 0xFF));
}

void BCPRowEncoder::EncodeDatetimeOffset(vector<uint8_t> &buffer, timestamp_t ts, int16_t offset_minutes,
										 uint8_t scale) {
	uint64_t time_value;
	uint32_t date_value;
	TimestampToDatetime2Components(ts, scale, time_value, date_value);
	EncodeDatetimeOffsetRaw(buffer, time_value, date_value, offset_minutes, scale);
}

//===----------------------------------------------------------------------===//
// NULL Encoding
//===----------------------------------------------------------------------===//

void BCPRowEncoder::EncodeNullFixed(vector<uint8_t> &buffer) {
	buffer.push_back(0x00);	 // length = 0 indicates NULL
}

void BCPRowEncoder::EncodeNullVariable(vector<uint8_t> &buffer) {
	buffer.push_back(0xFF);	 // 0xFFFF indicates NULL for USHORTLEN
	buffer.push_back(0xFF);
}

void BCPRowEncoder::EncodeNullGUID(vector<uint8_t> &buffer) {
	buffer.push_back(0x00);	 // length = 0 indicates NULL
}

void BCPRowEncoder::EncodeNullDateTime(vector<uint8_t> &buffer) {
	buffer.push_back(0x00);	 // length = 0 indicates NULL
}

void BCPRowEncoder::EncodeNullPLP(vector<uint8_t> &buffer) {
	// PLP NULL is 8 bytes of 0xFF
	for (int i = 0; i < 8; i++) {
		buffer.push_back(0xFF);
	}
}

//===----------------------------------------------------------------------===//
// Helper Methods
//===----------------------------------------------------------------------===//

vector<uint8_t> BCPRowEncoder::StringToUTF16LE(const string_t &str) {
	// Use existing UTF-16 encoding utility
	return Utf16LEEncode(str.GetString());
}

uint8_t BCPRowEncoder::GetTimeByteSize(uint8_t scale) {
	if (scale <= 2) {
		return 3;
	} else if (scale <= 4) {
		return 4;
	} else {
		return 5;
	}
}

uint8_t BCPRowEncoder::GetDecimalByteSize(uint8_t precision) {
	if (precision <= 9) {
		return 5;  // 1 sign + 4 mantissa
	} else if (precision <= 19) {
		return 9;  // 1 sign + 8 mantissa
	} else if (precision <= 28) {
		return 13;	// 1 sign + 12 mantissa
	} else {
		return 17;	// 1 sign + 16 mantissa
	}
}

void BCPRowEncoder::TimestampToDatetime2Components(timestamp_t ts, uint8_t scale, uint64_t &time_value,
												   uint32_t &date_value) {
	// DuckDB timestamp_t is microseconds since 1970-01-01 00:00:00
	int64_t total_micros = ts.value;

	// Split into days and time-of-day
	int32_t days;
	int64_t time_micros;

	if (total_micros >= 0) {
		days = static_cast<int32_t>(total_micros / MICROS_PER_DAY);
		time_micros = total_micros % MICROS_PER_DAY;
	} else {
		// Handle negative timestamps (before 1970)
		days = static_cast<int32_t>((total_micros - MICROS_PER_DAY + 1) / MICROS_PER_DAY);
		time_micros = total_micros - (static_cast<int64_t>(days) * MICROS_PER_DAY);
	}

	// Convert days to SQL Server format (days since 0001-01-01)
	date_value = static_cast<uint32_t>(days + DAYS_FROM_0001_TO_EPOCH);

	// Convert time to the appropriate scale
	if (scale <= 6) {
		// Divide for scales 0-6
		int64_t divisor = 1;
		for (int i = 0; i < 6 - scale; i++) {
			divisor *= 10;
		}
		time_value = static_cast<uint64_t>(time_micros / divisor);
	} else {
		// Multiply for scale 7 (100ns units)
		time_value = static_cast<uint64_t>(time_micros * 10);
	}
}

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb
