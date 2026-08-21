//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/uuid_codec.cpp
//
// Uuid family — TDS UNIQUEIDENTIFIER (0x24) <-> DuckDB UUID (hugeint_t with
// upper-half bit-63 XOR for sortability).
//
// Wire format: 16 bytes in TDS mixed-endian order
//   bytes 0-3  : Data1 (little-endian uint32)
//   bytes 4-5  : Data2 (little-endian uint16)
//   bytes 6-7  : Data3 (little-endian uint16)
//   bytes 8-15 : Data4 (big-endian, as-is)
//
// Standard UUID textual form xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx is
// big-endian for all four groups; the byte-order swap on bytes 0-7 is
// performed by the low-level helper tds/encoding/guid_encoding.cpp,
// which we reuse on both decode and encode paths.
//
// Also defines codec::uuid::RenderAsString — a public helper used by
// TypeConverter::WriteAsStringFallback for the issue-#89 path
// (catalog says VARCHAR but TDS returns UNIQUEIDENTIFIER) — and
// DecodeChunkFromStaging, the family's batch decode for the staged read
// path (spec 055 D6).
//===----------------------------------------------------------------------===//

#include "codec/uuid_codec.hpp"

#include "codec/scatter_position.hpp"

#include "codec/vector_format.hpp"
#include "copy/target_resolver.hpp"
#include "dml/ctas/mssql_ctas_config.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "tds/encoding/guid_encoding.hpp"
#include "tds/tds_column_metadata.hpp"

#include <cstdint>
#include <cstring>

namespace duckdb {
namespace mssql {
namespace codec {
namespace uuid {

namespace {

constexpr size_t GUID_WIRE_SIZE = 16;
constexpr uint8_t GUID_LENGTH_PREFIX = 16;

// Encode a hugeint_t (DuckDB-stored UUID, bit-63-flipped for sortability)
// to the 16-byte TDS mixed-endian wire form, written into `out_bytes`
// (must point to a 16-byte buffer).
void EncodeHugeIntToWire(const hugeint_t &uuid, uint8_t *out_bytes) {
	// Reverse DuckDB's sortability XOR on the upper half.
	uint64_t upper = static_cast<uint64_t>(uuid.upper) ^ (uint64_t(1) << 63);
	uint64_t lower = uuid.lower;

	// Materialize the canonical big-endian form (matches what
	// GuidEncoding::ConvertGuid produces internally before XOR).
	uint8_t be_bytes[16];
	for (int i = 0; i < 8; i++) {
		be_bytes[i] = static_cast<uint8_t>((upper >> (56 - i * 8)) & 0xFF);
		be_bytes[i + 8] = static_cast<uint8_t>((lower >> (56 - i * 8)) & 0xFF);
	}

	// Data1 (bytes 0-3) → little-endian
	out_bytes[0] = be_bytes[3];
	out_bytes[1] = be_bytes[2];
	out_bytes[2] = be_bytes[1];
	out_bytes[3] = be_bytes[0];

	// Data2 (bytes 4-5) → little-endian
	out_bytes[4] = be_bytes[5];
	out_bytes[5] = be_bytes[4];

	// Data3 (bytes 6-7) → little-endian
	out_bytes[6] = be_bytes[7];
	out_bytes[7] = be_bytes[6];

	// Data4 (bytes 8-15) → unchanged big-endian
	std::memcpy(out_bytes + 8, be_bytes + 8, 8);
}

}  // namespace

//===----------------------------------------------------------------------===//
// Decode (TDS → DuckDB)
//===----------------------------------------------------------------------===//

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata & /*col*/, Vector &out, idx_t row) {
	if (bytes.size() != GUID_WIRE_SIZE) {
		throw InvalidInputException("codec::uuid::DecodeFromTds: expected 16 wire bytes, got %zu", bytes.size());
	}
	auto guid = tds::encoding::GuidEncoding::ConvertGuid(bytes.data());
	FlatVector::GetDataMutableUnsafe<hugeint_t>(out)[row] = guid;
}

//===----------------------------------------------------------------------===//
// Encode (DuckDB → TDS BCP)
//===----------------------------------------------------------------------===//

void EncodeToBcp(Vector &in, const UnifiedVectorFormat &fmt, idx_t row, const mssql::BCPColumnMetadata &col,
				 duckdb::vector<uint8_t> &buf) {
	// Format-based access (was FlatVector::GetData — wrong for dictionary/
	// constant inputs; fixed as part of the W1 hoisting, spec 054).
	(void)in;
	const size_t at = buf.size();
	buf.resize(at + 1 + GUID_WIRE_SIZE);
	buf[at] = GUID_LENGTH_PREFIX;
	ScatterToBcp(buf.data() + at + 1, in, fmt, row, col);
}

// One call per COLUMN — see decimal's peer and the read path's
// DecodeChunkFromStaging. The shuffle is inlined here rather than called across
// a translation-unit boundary per value.
// One implementation, two positionings (spec 064 D1). POS is StridePos or
// CursorPos; both are empty-ish structs whose members inline, so the strided
// instantiation compiles to what it always did.
//
// HAS_NULLS is a template parameter rather than a runtime flag so the all-valid
// instantiation — every strided caller, and a NULL-free column inside a cursor
// chunk — carries no validity test in the loop at all.
template <class POS, bool HAS_SEL, bool HAS_NULLS>
static inline void ScatterImpl(POS pos, idx_t row_begin, idx_t rows, const UnifiedVectorFormat &fmt) {
	const hugeint_t *src = reinterpret_cast<const hugeint_t *>(fmt.data);
	const SelectionVector *sel = fmt.sel;
	for (idx_t r = 0; r < rows; r++) {
		const idx_t idx = HAS_SEL ? sel->get_index_unsafe(row_begin + r) : row_begin + r;
		uint8_t *out = pos.At(r);
		if (HAS_NULLS && !fmt.validity.RowIsValid(idx)) {
			// A NULL is a zero length byte and no payload — so the row is SHORTER
			// by the width, which is the whole reason a NULL-bearing chunk cannot
			// use a constant stride.
			*out = 0x00;
			pos.Advance(r, 1);
			continue;
		}
		*out = GUID_LENGTH_PREFIX;
		EncodeHugeIntToWire(src[idx], out + 1);
		pos.Advance(r, 1 + GUID_LENGTH_PREFIX);
	}
}

template <class POS>
static inline void ScatterDispatch(POS pos, idx_t row_begin, idx_t rows, const UnifiedVectorFormat &fmt,
								   bool all_valid) {
	const bool has_sel = fmt.sel->IsSet();
	// Selection presence and validity hoisted out of the loop — see decimal's peer.
	if (all_valid) {
		has_sel ? ScatterImpl<POS, true, false>(pos, row_begin, rows, fmt)
				: ScatterImpl<POS, false, false>(pos, row_begin, rows, fmt);
	} else {
		has_sel ? ScatterImpl<POS, true, true>(pos, row_begin, rows, fmt)
				: ScatterImpl<POS, false, true>(pos, row_begin, rows, fmt);
	}
}

void ScatterChunkStrided(uint8_t *dst, size_t stride, idx_t row_begin, idx_t rows, Vector & /*in*/,
						 const UnifiedVectorFormat &fmt, const mssql::BCPColumnMetadata & /*col*/) {
	// Strided implies all-valid: a NULL would have made the rows differ in length.
	ScatterDispatch(StridePos{dst, stride}, row_begin, rows, fmt, /*all_valid=*/true);
}

void ScatterChunkCursor(uint8_t *dst, size_t *cursor, idx_t row_begin, idx_t rows, Vector & /*in*/,
						const UnifiedVectorFormat &fmt, const mssql::BCPColumnMetadata & /*col*/, bool all_valid) {
	ScatterDispatch(CursorPos{dst, cursor, row_begin}, row_begin, rows, fmt, all_valid);
}

// Write the 16 wire bytes at a pointer — the one implementation, from which the
// appending form above is derived (spec 057 step 3).
void ScatterToBcp(uint8_t *out, Vector & /*in*/, const UnifiedVectorFormat &fmt, idx_t row,
				  const mssql::BCPColumnMetadata & /*col*/) {
	EncodeHugeIntToWire(FormatValue<hugeint_t>(fmt, row), out);
}

void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf) {
	EncodeToBcpViaFormat(EncodeToBcp, in, row, col, buf);
}

void EncodeToBcp(const Value &value, const mssql::BCPColumnMetadata & /*col*/, duckdb::vector<uint8_t> &buf) {
	auto uuid = value.GetValue<hugeint_t>();
	buf.push_back(GUID_LENGTH_PREFIX);
	const size_t start = buf.size();
	buf.resize(start + GUID_WIRE_SIZE);
	EncodeHugeIntToWire(uuid, buf.data() + start);
}

//===----------------------------------------------------------------------===//
// SQL literal (FR-022 — Filter == InsertValues byte-identity)
//===----------------------------------------------------------------------===//

std::string FormatSqlLiteral(const Value &v, const LogicalType & /*type*/, LiteralContext /*ctx*/) {
	if (v.IsNull()) {
		return "NULL";
	}
	auto uuid = v.GetValue<hugeint_t>();
	return "'" + UUID::ToString(uuid) + "'";
}

//===----------------------------------------------------------------------===//
// DDL emit (FR-027 / FR-028 — CreateTable == CtasCreateTable byte-identity)
//===----------------------------------------------------------------------===//

std::string FormatDdlTypeName(const LogicalType & /*type*/, const mssql::CTASConfig & /*cfg*/, DdlContext /*ctx*/) {
	return "UNIQUEIDENTIFIER";
}

size_t EstimateLiteralSize(const LogicalType & /*type*/) {
	// "'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx'" = 32 hex + 4 dashes + 2 quotes = 38.
	return 38;
}

//===----------------------------------------------------------------------===//
// Batch decode from staging (spec 055 D6)
//===----------------------------------------------------------------------===//

void DecodeChunkFromStaging(const staging::ColumnStaging &st, idx_t count, const tds::ColumnMetadata & /*col*/,
							Vector &out) {
	hugeint_t *result = FlatVector::GetDataMutable<hugeint_t>(out);
	const uint8_t *src = st.buffer.data();
	for (idx_t row = 0; row < count; row++) {
		result[row] = tds::encoding::GuidEncoding::ConvertGuid(src + row * GUID_WIRE_SIZE);
	}
}

//===----------------------------------------------------------------------===//
// Issue-#89 fallback — render wire bytes as canonical lowercase text.
//===----------------------------------------------------------------------===//

std::string RenderAsString(const uint8_t *bytes, size_t size) {
	if (size != GUID_WIRE_SIZE) {
		throw InvalidInputException("codec::uuid::RenderAsString: expected 16 wire bytes, got %zu", size);
	}
	auto guid = tds::encoding::GuidEncoding::ConvertGuid(bytes);
	return UUID::ToString(guid);
}

std::string RenderAsString(const std::vector<uint8_t> &bytes) {
	return RenderAsString(bytes.data(), bytes.size());
}

}  // namespace uuid
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
