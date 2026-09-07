//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/binary_codec.cpp
//
// Binary family implementation. See codec/binary_codec.hpp.
//
// Behavior parity (vs pre-spec-045 baseline):
//   - DecodeFromTds mirrors TypeConverter::ConvertBinary —
//     StringVector::AddStringOrBlob copies the raw payload bytes into
//     the destination vector. Works for any string_t-backed type
//     (BLOB, GEOMETRY, VARCHAR fallback).
//   - EncodeToBcp dispatches on col.IsPLPType() and delegates to the
//     legacy BCPRowEncoder helpers (EncodeBinary / EncodeBinaryPLP).
//     Wire layout byte-identical to pre-spec-045 (FR-014).
//   - FormatSqlLiteral produces "0x<UPPERHEX>" for both LiteralContext
//     values (FR-022). Pre-spec-045 both dispatch sites already
//     produced this exact text; consolidation removes duplication.
//   - FormatDdlTypeName returns "VARBINARY(MAX)" for both DdlContext
//     values (FR-027/FR-028).
//   - EstimateLiteralSize: a fixed upper bound (16386) since DuckDB
//     BLOB/GEOMETRY have no inherent size; matches pre-spec-045
//     buffer pre-sizing behaviour (filter buffers grow on demand).
//
// Also defines codec::binary::RenderAsString — a public helper used by
// the issue-#89 VARCHAR-fallback path in TypeConverter::ConvertValue.
// Same rendering as the literal path.
//===----------------------------------------------------------------------===//

#include "codec/binary_codec.hpp"

#include "codec/utf8_guard.hpp"
#include "codec/vector_format.hpp"
#include "copy/target_resolver.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "tds/encoding/bcp_row_encoder.hpp"
#include "tds/tds_column_metadata.hpp"

#include <simdutf.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace duckdb {
namespace mssql {
namespace codec {
namespace binary {

namespace {

// Canonical 0x<UPPERHEX> renderer — shared between FormatSqlLiteral, the
// dispatcher-level fallback (RenderAsString), and any future caller. Pre-spec-045
// both filter_encoder and MSSQLValueSerializer::SerializeBlob produced identical
// text from this exact loop; FR-022 mandates byte-identity going forward.
std::string HexRender(const uint8_t *data, size_t length) {
	static constexpr char hex_chars[] = "0123456789ABCDEF";
	std::string result;
	result.reserve(2 + length * 2);
	result += "0x";
	for (size_t i = 0; i < length; i++) {
		uint8_t byte = data[i];
		result += hex_chars[byte >> 4];
		result += hex_chars[byte & 0x0F];
	}
	return result;
}

}  // namespace

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata & /*col*/, Vector &out, idx_t row) {
	// AddStringOrBlob copies the raw bytes into the vector's string heap.
	// Works for BLOB, GEOMETRY, and the VARCHAR fallback case (issue #89).
	FlatVector::GetDataMutableUnsafe<string_t>(out)[row] =
		StringVector::AddStringOrBlob(out, reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

//! Length of `data[0..len)` with trailing 0x20 bytes removed.
//!
//! Safe on UTF-8 as well as on single-byte text: 0x20 is never a continuation
//! byte, so a trailing space can only be U+0020 and never the tail of a
//! multi-byte character.
static inline uint32_t TrimTrailingSpaces(const char *data, uint32_t len) {
	while (len > 0 && data[len - 1] == ' ') {
		len--;
	}
	return len;
}

void DecodeChunkFromStaging(const staging::ColumnStaging &st, idx_t count, const tds::ColumnMetadata &col,
							Vector &out) {
	string_t *result = FlatVector::GetDataMutable<string_t>(out);
	const idx_t payload = st.PayloadSize();

	if (payload == 0) {
		// All-NULL or all-empty: an empty string_t is inlined, so it needs
		// neither storage nor a valid pointer, and nothing is allocated.
		static const char EMPTY[1] = {0};
		for (idx_t row = 0; row < count; row++) {
			if (st.IsValid(row)) {
				result[row] = string_t(EMPTY, 0);
			}
		}
		return;
	}

	// One allocation and one copy for the whole column, in place of one of each
	// per value. The size is exact — no upper bound to guess, because binary
	// output is its input.
	//
	// A payload of at most 12 bytes makes EmptyString inline its storage into
	// this local. That stays correct: every value is then shorter than the
	// inline threshold too, so each string_t below copies its bytes rather than
	// retaining a pointer into storage that dies with this frame.
	string_t blob_slot = StringVector::EmptyString(out, payload);
	char *const blob = blob_slot.GetDataWriteable();
	std::memcpy(blob, st.buffer.data(), payload);

	// Single-byte text (CHAR / VARCHAR / TEXT) reaches this BINARY kernel
	// because its bytes are copied verbatim, exactly like VARBINARY
	// (column_ops: P2StageBinary / PlpStageBinary / LobStageBinary). The
	// difference is the destination: those bytes land in a VARCHAR vector, and
	// DuckDB VARCHAR is UTF-8 by contract -- issue #224.
	//
	// The catalog path casts non-Unicode string columns to NVARCHAR server-side,
	// which is what column_ops means by "collation handling lives in the scan's
	// SELECT list" -- EXCEPT a declared VARCHAR(MAX) when mssql_convert_varchar_max
	// is off, which NeedsNVarcharConversion deliberately opts out of, so that one
	// configuration reaches this kernel through the catalog too and now errors
	// where it previously published a corrupt string. A raw mssql_scan() has no
	// CAST at all. VARBINARY and IMAGE are untouched: their bytes are meant to be
	// arbitrary, and they land in a BLOB.
	//
	// The fast path is ASCII, not a whole-payload validate_utf8. Staged values
	// are packed contiguously with no separator, so a lead byte ending one value
	// and continuation bytes starting the next form a valid sequence ACROSS the
	// boundary -- 0xC3 then a value beginning 0xA9 validates as U+00E9 although
	// neither value is valid alone, which would wave the corruption straight
	// through. All-ASCII carries no such case: every byte is a whole character.
	if (IsSingleByteTextColumn(col) && !IsAsciiRun(blob, payload)) {
		for (idx_t row = 0; row < count; row++) {
			if (!st.IsValid(row)) {
				continue;
			}
			const char *value = blob + st.offsets[row];
			if (!simdutf::validate_utf8(value, st.lengths[row])) {
				ThrowNonUtf8Column(col, static_cast<size_t>(row));
			}
		}
	}

	// Two loops rather than a test per value: whether the column is fixed-length
	// CHAR is decided by its type, not its data.
	if (col.type_id == tds::TDS_TYPE_BIGCHAR) {
		for (idx_t row = 0; row < count; row++) {
			if (!st.IsValid(row)) {
				continue;
			}
			const char *value = blob + st.offsets[row];
			result[row] = string_t(value, TrimTrailingSpaces(value, st.lengths[row]));
		}
		return;
	}
	for (idx_t row = 0; row < count; row++) {
		if (!st.IsValid(row)) {
			continue;
		}
		result[row] = string_t(blob + st.offsets[row], st.lengths[row]);
	}
}

//! Cut a BLOB to the column's declared bound, the way the columnar path's
//! ClampToBound does for VarKind::Binary.
//!
//! It did not, and the two paths disagreed: a varbinary(3) column took the first
//! 3 bytes when every column in the chunk resolved to a kernel, and the whole
//! value when some unrelated column dropped the chunk to row-major — where the
//! server then rejected the batch for an over-long value. Same statement, same
//! data, outcome decided by a neighbour.
//!
//! Binary has no character boundary to respect, so unlike the string bounds this
//! cut is exact. A PLP column has no bound at all — `max_length` there is the
//! 0xFFFF sentinel, not a length — which is why this is only reached on the
//! non-PLP arm.
static string_t ClampBlobToBound(const string_t &blob, const mssql::BCPColumnMetadata &col) {
	const uint32_t bound = col.max_length;
	if (bound == 0 || blob.GetSize() <= bound) {
		return blob;
	}
	return string_t(blob.GetData(), bound);
}

void EncodeToBcp(Vector &in, const UnifiedVectorFormat &fmt, idx_t row, const mssql::BCPColumnMetadata &col,
				 duckdb::vector<uint8_t> &buf) {
	// Format-based access (was FlatVector::GetData — wrong for dictionary/
	// constant inputs; fixed as part of the W1 hoisting, spec 054).
	(void)in;
	const string_t blob = FormatValue<string_t>(fmt, row);
	if (col.IsPLPType()) {
		tds::encoding::BCPRowEncoder::EncodeBinaryPLP(buf, blob);
	} else {
		tds::encoding::BCPRowEncoder::EncodeBinary(buf, ClampBlobToBound(blob, col));
	}
}

void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf) {
	EncodeToBcpViaFormat(EncodeToBcp, in, row, col, buf);
}

void EncodeToBcp(const Value &value, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf) {
	auto blob = string_t(value.GetValueUnsafe<std::string>());
	if (col.IsPLPType()) {
		tds::encoding::BCPRowEncoder::EncodeBinaryPLP(buf, blob);
	} else {
		tds::encoding::BCPRowEncoder::EncodeBinary(buf, ClampBlobToBound(blob, col));
	}
}

std::string FormatSqlLiteral(const Value &v, const LogicalType & /*type*/, LiteralContext /*ctx*/) {
	if (v.IsNull()) {
		return "NULL";
	}
	auto blob = v.GetValueUnsafe<string_t>();
	return HexRender(reinterpret_cast<const uint8_t *>(blob.GetData()), blob.GetSize());
}

std::string FormatDdlTypeName(const LogicalType & /*type*/, const mssql::CTASConfig & /*cfg*/, DdlContext /*ctx*/) {
	return "VARBINARY(MAX)";
}

size_t EstimateLiteralSize(const LogicalType & /*type*/) {
	// DuckDB BLOB / GEOMETRY have no inherent size bound; use a generous
	// upper estimate that matches pre-spec-045 buffer pre-sizing. Filter
	// pushdown buffers grow on demand anyway — this is purely a hint.
	return 2 + 8192 * 2;
}

std::string RenderAsString(const uint8_t *bytes, size_t size) {
	return HexRender(bytes, size);
}

std::string RenderAsString(const std::vector<uint8_t> &bytes) {
	return RenderAsString(bytes.data(), bytes.size());
}

}  // namespace binary
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
