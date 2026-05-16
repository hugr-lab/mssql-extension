// test/cpp/codec/test_uuid_codec.cpp
// Unit tests for codec::uuid (spec 045, US3 sub-phase 7 — Phase 6).
//
// Does NOT require a running SQL Server instance.
//
// Covers:
//   - DecodeFromTds — 16-byte TDS mixed-endian wire → DuckDB hugeint_t UUID.
//     Spot-checks nil/max/RFC4122 example/mixed-byte pattern, and a
//     sequential GUID whose high bit lands inside the upper 64-bit half
//     (exercises the DuckDB UUID sortability XOR mask).
//   - EncodeToBcp Vector + Value variants — symmetric inverse of decode.
//   - FormatSqlLiteral byte-identity Filter == InsertValues (FR-022).
//     UUID literals never need escaping (canonical form is [0-9a-f-] only).
//   - FormatDdlTypeName byte-identity CreateTable == CtasCreateTable
//     (FR-027/FR-028, returns "UNIQUEIDENTIFIER" verbatim).
//   - EstimateLiteralSize lower bound covers the 38-char quoted form.
//   - NULL routed through dispatcher → "NULL".
//   - Dispatcher routing of LogicalTypeId::UUID via type_family.cpp.
//   - RenderAsString helper (issue-#89 fallback support).
//
// Build & run:
//   GEN=ninja make debug
//   make test-codec-uuid

#include "codec/literal_context.hpp"
#include "codec/literal_format.hpp"
#include "codec/type_family.hpp"
#include "codec/uuid_codec.hpp"
#include "copy/target_resolver.hpp"
#include "dml/ctas/mssql_ctas_config.hpp"
#include "tds/tds_column_metadata.hpp"
#include "tds/tds_types.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using duckdb::hugeint_t;
using duckdb::LogicalType;
using duckdb::UUID;
using duckdb::Value;
using duckdb::mssql::BCPColumnMetadata;
using duckdb::mssql::CTASConfig;
using duckdb::mssql::codec::DdlContext;
using duckdb::mssql::codec::LiteralContext;

namespace {

int failures = 0;

#define CHECK_EQ(actual, expected)                                                                       \
	do {                                                                                                 \
		const auto &_a = (actual);                                                                       \
		const auto &_e = (expected);                                                                     \
		if (!(_a == _e)) {                                                                               \
			++failures;                                                                                  \
			std::cerr << "FAIL [" << __LINE__ << "] " #actual " == " #expected << "\n  actual:   " << _a \
					  << "\n  expected: " << _e << "\n";                                                 \
		}                                                                                                \
	} while (0)

#define CHECK_TRUE(expr)                                          \
	do {                                                          \
		if (!(expr)) {                                            \
			++failures;                                           \
			std::cerr << "FAIL [" << __LINE__ << "] " #expr "\n"; \
		}                                                         \
	} while (0)

std::string ToHex(const duckdb::vector<uint8_t> &buf) {
	static constexpr char hex_chars[] = "0123456789abcdef";
	std::string out;
	out.reserve(buf.size() * 3);
	for (size_t i = 0; i < buf.size(); i++) {
		if (i > 0)
			out += ' ';
		out += hex_chars[buf[i] >> 4];
		out += hex_chars[buf[i] & 0x0F];
	}
	return out;
}

// codec::uuid::DecodeFromTds ignores its ColumnMetadata argument (length is
// always 16). The tests still construct one to avoid binding to nullptr.
const duckdb::tds::ColumnMetadata &UnusedColumnMetadata() {
	static duckdb::tds::ColumnMetadata stub{};
	stub.type_id = duckdb::tds::TDS_TYPE_UNIQUEIDENTIFIER;
	return stub;
}

BCPColumnMetadata MakeBCPColumn() {
	BCPColumnMetadata col;
	col.name = "uuid_col";
	col.duckdb_type = LogicalType::UUID;
	col.tds_type_token = duckdb::tds::TDS_TYPE_UNIQUEIDENTIFIER;
	col.max_length = 16;
	col.precision = 0;
	col.scale = 0;
	col.nullable = true;
	return col;
}

// Helper — parse "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" into a Value(UUID).
Value MakeUuidValue(const std::string &canonical) {
	return Value::UUID(canonical);
}

//===----------------------------------------------------------------------===//
// Decode
//===----------------------------------------------------------------------===//

void TestDecodeNilUuid() {
	std::cout << "Test: DecodeFromTds — all-zero wire → nil UUID\n";
	std::vector<uint8_t> wire(16, 0);
	duckdb::Vector vec(LogicalType::UUID, 1);
	duckdb::mssql::codec::uuid::DecodeFromTds(wire, UnusedColumnMetadata(), vec, 0);

	auto stored = duckdb::FlatVector::GetData<hugeint_t>(vec)[0];
	CHECK_EQ(UUID::ToString(stored), std::string("00000000-0000-0000-0000-000000000000"));
}

void TestDecodeMaxUuid() {
	std::cout << "Test: DecodeFromTds — all-0xFF wire → max UUID\n";
	std::vector<uint8_t> wire(16, 0xFF);
	duckdb::Vector vec(LogicalType::UUID, 1);
	duckdb::mssql::codec::uuid::DecodeFromTds(wire, UnusedColumnMetadata(), vec, 0);

	auto stored = duckdb::FlatVector::GetData<hugeint_t>(vec)[0];
	CHECK_EQ(UUID::ToString(stored), std::string("ffffffff-ffff-ffff-ffff-ffffffffffff"));
}

void TestDecodeRfc4122Example() {
	std::cout << "Test: DecodeFromTds — RFC 4122 §3 example 6ba7b810-9dad-11d1-80b4-00c04fd430c8\n";
	// Data1=0x6ba7b810 LE | Data2=0x9dad LE | Data3=0x11d1 LE | Data4 BE
	std::vector<uint8_t> wire = {0x10, 0xB8, 0xA7, 0x6B, 0xAD, 0x9D, 0xD1, 0x11,
								 0x80, 0xB4, 0x00, 0xC0, 0x4F, 0xD4, 0x30, 0xC8};
	duckdb::Vector vec(LogicalType::UUID, 1);
	duckdb::mssql::codec::uuid::DecodeFromTds(wire, UnusedColumnMetadata(), vec, 0);

	auto stored = duckdb::FlatVector::GetData<hugeint_t>(vec)[0];
	CHECK_EQ(UUID::ToString(stored), std::string("6ba7b810-9dad-11d1-80b4-00c04fd430c8"));
}

void TestDecodeMixedBytePattern() {
	std::cout << "Test: DecodeFromTds — 01234567-89ab-cdef-0123-456789abcdef\n";
	std::vector<uint8_t> wire = {0x67, 0x45, 0x23, 0x01, 0xAB, 0x89, 0xEF, 0xCD,
								 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
	duckdb::Vector vec(LogicalType::UUID, 1);
	duckdb::mssql::codec::uuid::DecodeFromTds(wire, UnusedColumnMetadata(), vec, 0);

	auto stored = duckdb::FlatVector::GetData<hugeint_t>(vec)[0];
	CHECK_EQ(UUID::ToString(stored), std::string("01234567-89ab-cdef-0123-456789abcdef"));
}

void TestDecodeHighBitSet() {
	std::cout << "Test: DecodeFromTds — high bit set in upper half (sortability XOR coverage)\n";
	// c2c7f47e-8edd-4e1f-8a3f-aef2a18b1c2d
	// Data1=0xc2c7f47e LE | Data2=0x8edd LE | Data3=0x4e1f LE | Data4 BE
	std::vector<uint8_t> wire = {0x7E, 0xF4, 0xC7, 0xC2, 0xDD, 0x8E, 0x1F, 0x4E,
								 0x8A, 0x3F, 0xAE, 0xF2, 0xA1, 0x8B, 0x1C, 0x2D};
	duckdb::Vector vec(LogicalType::UUID, 1);
	duckdb::mssql::codec::uuid::DecodeFromTds(wire, UnusedColumnMetadata(), vec, 0);

	auto stored = duckdb::FlatVector::GetData<hugeint_t>(vec)[0];
	CHECK_EQ(UUID::ToString(stored), std::string("c2c7f47e-8edd-4e1f-8a3f-aef2a18b1c2d"));

	// XOR mask sanity: DuckDB stores the upper byte 0xC2 -> 0x42 (bit 63 cleared).
	uint64_t upper_xor = static_cast<uint64_t>(stored.upper);
	CHECK_EQ(static_cast<uint8_t>((upper_xor >> 56) & 0xFF), uint8_t(0x42));
}

//===----------------------------------------------------------------------===//
// Encode (BCP)
//===----------------------------------------------------------------------===//

void TestEncodeVectorNilUuid() {
	std::cout << "Test: EncodeToBcp Vector — nil UUID → length 16 + 16 zero bytes\n";
	duckdb::Vector vec(LogicalType::UUID, 1);
	auto v = MakeUuidValue("00000000-0000-0000-0000-000000000000");
	vec.SetValue(0, v);

	BCPColumnMetadata col = MakeBCPColumn();
	duckdb::vector<uint8_t> buf;
	duckdb::mssql::codec::uuid::EncodeToBcp(vec, 0, col, buf);

	CHECK_EQ(ToHex(buf), std::string("10 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"));
}

void TestEncodeVectorRfc4122() {
	std::cout << "Test: EncodeToBcp Vector — 6ba7b810-9dad-11d1-80b4-00c04fd430c8 round-trips\n";
	duckdb::Vector vec(LogicalType::UUID, 1);
	auto v = MakeUuidValue("6ba7b810-9dad-11d1-80b4-00c04fd430c8");
	vec.SetValue(0, v);

	BCPColumnMetadata col = MakeBCPColumn();
	duckdb::vector<uint8_t> buf;
	duckdb::mssql::codec::uuid::EncodeToBcp(vec, 0, col, buf);

	CHECK_EQ(ToHex(buf), std::string("10 10 b8 a7 6b ad 9d d1 11 80 b4 00 c0 4f d4 30 c8"));
}

void TestEncodeValueHighBit() {
	std::cout << "Test: EncodeToBcp Value — high-bit GUID round-trips\n";
	auto v = MakeUuidValue("c2c7f47e-8edd-4e1f-8a3f-aef2a18b1c2d");
	BCPColumnMetadata col = MakeBCPColumn();
	duckdb::vector<uint8_t> buf;
	duckdb::mssql::codec::uuid::EncodeToBcp(v, col, buf);

	CHECK_EQ(ToHex(buf), std::string("10 7e f4 c7 c2 dd 8e 1f 4e 8a 3f ae f2 a1 8b 1c 2d"));
}

void TestEncodeValueMixedPattern() {
	std::cout << "Test: EncodeToBcp Value — 01234567-89ab-cdef-0123-456789abcdef\n";
	auto v = MakeUuidValue("01234567-89ab-cdef-0123-456789abcdef");
	BCPColumnMetadata col = MakeBCPColumn();
	duckdb::vector<uint8_t> buf;
	duckdb::mssql::codec::uuid::EncodeToBcp(v, col, buf);

	CHECK_EQ(ToHex(buf), std::string("10 67 45 23 01 ab 89 ef cd 01 23 45 67 89 ab cd ef"));
}

void TestEncodeDecodeRoundTrip() {
	std::cout << "Test: EncodeToBcp + DecodeFromTds round-trip equivalence\n";
	const std::string canonical = "c2c7f47e-8edd-4e1f-8a3f-aef2a18b1c2d";
	auto v = MakeUuidValue(canonical);
	BCPColumnMetadata bcp_col = MakeBCPColumn();
	duckdb::vector<uint8_t> buf;
	duckdb::mssql::codec::uuid::EncodeToBcp(v, bcp_col, buf);

	// Strip the 1-byte length prefix for decode.
	CHECK_EQ(buf.size(), size_t(17));
	std::vector<uint8_t> wire(buf.begin() + 1, buf.end());

	duckdb::Vector vec(LogicalType::UUID, 1);
	duckdb::mssql::codec::uuid::DecodeFromTds(wire, UnusedColumnMetadata(), vec, 0);
	auto stored = duckdb::FlatVector::GetData<hugeint_t>(vec)[0];
	CHECK_EQ(UUID::ToString(stored), canonical);
}

//===----------------------------------------------------------------------===//
// SQL literal (FR-022 — Filter == InsertValues byte-identity)
//===----------------------------------------------------------------------===//

void TestFormatSqlLiteralByteIdentity() {
	std::cout << "Test: FormatSqlLiteral Filter == InsertValues (FR-022)\n";
	const char *canonicals[] = {
		"00000000-0000-0000-0000-000000000000", "ffffffff-ffff-ffff-ffff-ffffffffffff",
		"6ba7b810-9dad-11d1-80b4-00c04fd430c8", "01234567-89ab-cdef-0123-456789abcdef",
		"c2c7f47e-8edd-4e1f-8a3f-aef2a18b1c2d",
	};
	for (const char *c : canonicals) {
		auto v = MakeUuidValue(c);
		auto filter = duckdb::mssql::codec::uuid::FormatSqlLiteral(v, LogicalType::UUID, LiteralContext::Filter);
		auto insert = duckdb::mssql::codec::uuid::FormatSqlLiteral(v, LogicalType::UUID, LiteralContext::InsertValues);
		CHECK_EQ(filter, std::string("'") + c + "'");
		CHECK_EQ(filter, insert);
	}
}

//===----------------------------------------------------------------------===//
// DDL emit (FR-027 / FR-028)
//===----------------------------------------------------------------------===//

void TestFormatDdlTypeNameByteIdentity() {
	std::cout << "Test: FormatDdlTypeName CreateTable == CtasCreateTable (UNIQUEIDENTIFIER byte-identity)\n";
	CTASConfig cfg;
	auto create = duckdb::mssql::codec::uuid::FormatDdlTypeName(LogicalType::UUID, cfg, DdlContext::CreateTable);
	auto ctas = duckdb::mssql::codec::uuid::FormatDdlTypeName(LogicalType::UUID, cfg, DdlContext::CtasCreateTable);
	CHECK_EQ(create, std::string("UNIQUEIDENTIFIER"));
	CHECK_EQ(ctas, std::string("UNIQUEIDENTIFIER"));
}

//===----------------------------------------------------------------------===//
// EstimateLiteralSize
//===----------------------------------------------------------------------===//

void TestEstimateLiteralSize() {
	std::cout << "Test: EstimateLiteralSize >= 38 (covers '36-hex-with-4-dashes' quoted form)\n";
	auto est = duckdb::mssql::codec::uuid::EstimateLiteralSize(LogicalType::UUID);
	CHECK_TRUE(est >= 38u);
}

//===----------------------------------------------------------------------===//
// Dispatcher routing (literal_format.cpp)
//===----------------------------------------------------------------------===//

void TestDispatcherRouting() {
	std::cout << "Test: codec::FormatSqlLiteral dispatches UUID family + NULL\n";
	// NULL routes to the bare "NULL" token without invoking the codec.
	CHECK_EQ(
		duckdb::mssql::codec::FormatSqlLiteral(Value(LogicalType::UUID), LogicalType::UUID, LiteralContext::Filter),
		std::string("NULL"));

	auto v = MakeUuidValue("6ba7b810-9dad-11d1-80b4-00c04fd430c8");
	CHECK_EQ(duckdb::mssql::codec::FormatSqlLiteral(v, LogicalType::UUID, LiteralContext::Filter),
			 std::string("'6ba7b810-9dad-11d1-80b4-00c04fd430c8'"));
	CHECK_EQ(duckdb::mssql::codec::FormatSqlLiteral(v, LogicalType::UUID, LiteralContext::InsertValues),
			 std::string("'6ba7b810-9dad-11d1-80b4-00c04fd430c8'"));

	// EstimateLiteralSize dispatches into uuid::EstimateLiteralSize.
	CHECK_TRUE(duckdb::mssql::codec::EstimateLiteralSize(LogicalType::UUID) >= 38u);
}

//===----------------------------------------------------------------------===//
// RenderAsString (issue-#89 fallback)
//===----------------------------------------------------------------------===//

void TestRenderAsString() {
	std::cout << "Test: RenderAsString — same lowercase canonical form as UUID::ToString\n";
	std::vector<uint8_t> wire = {0x10, 0xB8, 0xA7, 0x6B, 0xAD, 0x9D, 0xD1, 0x11,
								 0x80, 0xB4, 0x00, 0xC0, 0x4F, 0xD4, 0x30, 0xC8};
	auto rendered = duckdb::mssql::codec::uuid::RenderAsString(wire);
	CHECK_EQ(rendered, std::string("6ba7b810-9dad-11d1-80b4-00c04fd430c8"));

	std::vector<uint8_t> wire_nil(16, 0);
	auto rendered_nil = duckdb::mssql::codec::uuid::RenderAsString(wire_nil);
	CHECK_EQ(rendered_nil, std::string("00000000-0000-0000-0000-000000000000"));
}

}  // namespace

int main() {
	TestDecodeNilUuid();
	TestDecodeMaxUuid();
	TestDecodeRfc4122Example();
	TestDecodeMixedBytePattern();
	TestDecodeHighBitSet();
	TestEncodeVectorNilUuid();
	TestEncodeVectorRfc4122();
	TestEncodeValueHighBit();
	TestEncodeValueMixedPattern();
	TestEncodeDecodeRoundTrip();
	TestFormatSqlLiteralByteIdentity();
	TestFormatDdlTypeNameByteIdentity();
	TestEstimateLiteralSize();
	TestDispatcherRouting();
	TestRenderAsString();

	if (failures > 0) {
		std::cerr << "\n" << failures << " assertion(s) failed.\n";
		return 1;
	}
	std::cout << "\nAll codec::uuid assertions passed.\n";
	return 0;
}
