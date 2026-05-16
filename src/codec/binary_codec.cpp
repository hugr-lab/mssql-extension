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

#include "copy/target_resolver.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/vector.hpp"
#include "tds/encoding/bcp_row_encoder.hpp"

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
	FlatVector::GetData<string_t>(out)[row] =
		StringVector::AddStringOrBlob(out, reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf) {
	auto data = FlatVector::GetData<string_t>(in);
	const string_t &blob = data[row];
	if (col.IsPLPType()) {
		tds::encoding::BCPRowEncoder::EncodeBinaryPLP(buf, blob);
	} else {
		tds::encoding::BCPRowEncoder::EncodeBinary(buf, blob);
	}
}

void EncodeToBcp(const Value &value, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf) {
	auto blob = string_t(value.GetValueUnsafe<std::string>());
	if (col.IsPLPType()) {
		tds::encoding::BCPRowEncoder::EncodeBinaryPLP(buf, blob);
	} else {
		tds::encoding::BCPRowEncoder::EncodeBinary(buf, blob);
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

std::string RenderAsString(const std::vector<uint8_t> &bytes) {
	return HexRender(bytes.data(), bytes.size());
}

}  // namespace binary
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
