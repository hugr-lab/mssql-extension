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
// (catalog says VARCHAR but TDS returns UNIQUEIDENTIFIER).
//===----------------------------------------------------------------------===//

#include "codec/uuid_codec.hpp"

#include "copy/target_resolver.hpp"
#include "dml/ctas/mssql_ctas_config.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/uuid.hpp"
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
	FlatVector::GetData<hugeint_t>(out)[row] = guid;
}

//===----------------------------------------------------------------------===//
// Encode (DuckDB → TDS BCP)
//===----------------------------------------------------------------------===//

void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata & /*col*/, duckdb::vector<uint8_t> &buf) {
	auto uuid = FlatVector::GetData<hugeint_t>(in)[row];
	buf.push_back(GUID_LENGTH_PREFIX);
	const size_t start = buf.size();
	buf.resize(start + GUID_WIRE_SIZE);
	EncodeHugeIntToWire(uuid, buf.data() + start);
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
// Issue-#89 fallback — render wire bytes as canonical lowercase text.
//===----------------------------------------------------------------------===//

std::string RenderAsString(const std::vector<uint8_t> &bytes) {
	if (bytes.size() != GUID_WIRE_SIZE) {
		throw InvalidInputException("codec::uuid::RenderAsString: expected 16 wire bytes, got %zu", bytes.size());
	}
	auto guid = tds::encoding::GuidEncoding::ConvertGuid(bytes.data());
	return UUID::ToString(guid);
}

}  // namespace uuid
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
