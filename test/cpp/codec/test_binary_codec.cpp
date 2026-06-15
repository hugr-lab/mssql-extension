// test/cpp/codec/test_binary_codec.cpp
// Unit tests for codec::binary (spec 045, US3 sub-phase 5 — Phase 6).
//
// Does NOT require a running SQL Server instance.
//
// Covers:
//   - DecodeFromTds for non-PLP + PLP (both reassembled to identical
//     contiguous byte vector by tds_row_reader before reaching here).
//   - EncodeToBcp Vector + Value variants, PLP and non-PLP framing.
//   - FormatSqlLiteral byte-identity Filter == InsertValues (FR-022,
//     "0x<UPPERHEX>").
//   - FormatDdlTypeName byte-identity CreateTable == CtasCreateTable
//     (FR-027/FR-028, "VARBINARY(MAX)").
//   - EstimateLiteralSize lower bound.
//   - NULL routed through dispatcher → "NULL".
//   - Dispatcher routing of LogicalTypeId::BLOB and ::GEOMETRY.
//   - RenderAsString helper (issue-#89 fallback support).
//
// Build & run:
//   GEN=ninja make debug
//   make test-codec-binary

#include "codec/binary_codec.hpp"
#include "codec/literal_context.hpp"
#include "codec/literal_format.hpp"
#include "codec/type_family.hpp"
#include "copy/target_resolver.hpp"
#include "dml/ctas/mssql_ctas_config.hpp"
#include "tds/tds_column_metadata.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using duckdb::LogicalType;
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

// codec::binary::DecodeFromTds ignores its ColumnMetadata argument (Binary
// dispatch is on byte count alone, see header comment). The tests still
// construct one to avoid binding a reference to nullptr.
const duckdb::tds::ColumnMetadata &UnusedColumnMetadata() {
	static duckdb::tds::ColumnMetadata stub{};
	return stub;
}

BCPColumnMetadata MakeBCPColumn(bool plp) {
	BCPColumnMetadata col;
	col.name = "blob_col";
	col.duckdb_type = LogicalType::BLOB;
	col.tds_type_token = 0xa5;	// BIGVARBINARYTYPE
	col.max_length = plp ? 0xFFFF : 100;
	col.precision = 0;
	col.scale = 0;
	col.nullable = true;
	return col;
}

void TestDecodeNonPLPEmpty() {
	std::cout << "Test: DecodeFromTds — non-PLP empty payload → empty BLOB\n";
	std::vector<uint8_t> wire;
	duckdb::Vector vec(LogicalType::BLOB, 1);
	duckdb::mssql::codec::binary::DecodeFromTds(wire, UnusedColumnMetadata(), vec, 0);
	auto stored = duckdb::FlatVector::GetData<duckdb::string_t>(vec)[0];
	CHECK_EQ(stored.GetSize(), 0u);
}

void TestDecodePLPPayload() {
	std::cout << "Test: DecodeFromTds — PLP \"Hello\" → BLOB \"Hello\"\n";
	std::vector<uint8_t> wire = {0x48, 0x65, 0x6c, 0x6c, 0x6f};
	duckdb::Vector vec(LogicalType::BLOB, 1);
	duckdb::mssql::codec::binary::DecodeFromTds(wire, UnusedColumnMetadata(), vec, 0);
	auto stored = duckdb::FlatVector::GetData<duckdb::string_t>(vec)[0];
	CHECK_EQ(std::string(stored.GetData(), stored.GetSize()), std::string("Hello"));
}

void TestDecodeIntoGeometryVector() {
	std::cout << "Test: DecodeFromTds — same code-path writes into GEOMETRY vector\n";
	// Trivial WKB Point(0,0): byte-order=01 (little-endian) + type=01000000 (1=POINT)
	// + x=0.0 (8 bytes) + y=0.0 (8 bytes) = 21 bytes total.
	std::vector<uint8_t> wkb = {0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
								0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	duckdb::Vector vec(LogicalType::GEOMETRY(), 1);
	duckdb::mssql::codec::binary::DecodeFromTds(wkb, UnusedColumnMetadata(), vec, 0);
	auto stored = duckdb::FlatVector::GetData<duckdb::string_t>(vec)[0];
	CHECK_EQ(stored.GetSize(), wkb.size());
	CHECK_TRUE(std::memcmp(stored.GetData(), wkb.data(), wkb.size()) == 0);
}

void TestEncodeNonPLP() {
	std::cout << "Test: EncodeToBcp non-PLP — 0xDEADBEEF → 04 00 de ad be ef\n";
	duckdb::Vector vec(LogicalType::BLOB, 1);
	auto data = duckdb::FlatVector::GetData<duckdb::string_t>(vec);
	const char bytes[] = {(char)0xde, (char)0xad, (char)0xbe, (char)0xef};
	data[0] = duckdb::StringVector::AddStringOrBlob(vec, bytes, sizeof(bytes));

	BCPColumnMetadata col = MakeBCPColumn(/*plp=*/false);
	duckdb::vector<uint8_t> buf;
	duckdb::mssql::codec::binary::EncodeToBcp(vec, 0, col, buf);

	CHECK_EQ(ToHex(buf), std::string("04 00 de ad be ef"));
}

void TestEncodePLPEmpty() {
	std::cout << "Test: EncodeToBcp PLP empty — UNKNOWN_PLP_LEN + terminator\n";
	duckdb::Vector vec(LogicalType::BLOB, 1);
	auto data = duckdb::FlatVector::GetData<duckdb::string_t>(vec);
	data[0] = duckdb::StringVector::AddStringOrBlob(vec, "", 0);

	BCPColumnMetadata col = MakeBCPColumn(/*plp=*/true);
	duckdb::vector<uint8_t> buf;
	duckdb::mssql::codec::binary::EncodeToBcp(vec, 0, col, buf);

	// PLP header (0xFFFFFFFFFFFFFFFE LE) + 4-byte zero terminator.
	CHECK_EQ(ToHex(buf), std::string("fe ff ff ff ff ff ff ff 00 00 00 00"));
}

void TestEncodePLPHello() {
	std::cout << "Test: EncodeToBcp PLP \"Hello\" — chunk header + payload + terminator\n";
	duckdb::Vector vec(LogicalType::BLOB, 1);
	auto data = duckdb::FlatVector::GetData<duckdb::string_t>(vec);
	data[0] = duckdb::StringVector::AddStringOrBlob(vec, "Hello", 5);

	BCPColumnMetadata col = MakeBCPColumn(/*plp=*/true);
	duckdb::vector<uint8_t> buf;
	duckdb::mssql::codec::binary::EncodeToBcp(vec, 0, col, buf);

	CHECK_EQ(ToHex(buf), std::string("fe ff ff ff ff ff ff ff 05 00 00 00 48 65 6c 6c 6f 00 00 00 00"));
}

void TestEncodeValueOverload() {
	std::cout << "Test: EncodeToBcp Value overload — 0xFF non-PLP\n";
	std::string payload(1, (char)0xff);
	auto value = Value::BLOB_RAW(payload);
	BCPColumnMetadata col = MakeBCPColumn(/*plp=*/false);
	duckdb::vector<uint8_t> buf;
	duckdb::mssql::codec::binary::EncodeToBcp(value, col, buf);
	CHECK_EQ(ToHex(buf), std::string("01 00 ff"));
}

void TestFormatSqlLiteralByteIdentity() {
	std::cout << "Test: FormatSqlLiteral Filter == InsertValues (0xDEADBEEF byte-identity)\n";
	std::string payload;
	payload += (char)0xde;
	payload += (char)0xad;
	payload += (char)0xbe;
	payload += (char)0xef;
	auto value = Value::BLOB_RAW(payload);

	auto filter = duckdb::mssql::codec::binary::FormatSqlLiteral(value, LogicalType::BLOB, LiteralContext::Filter);
	auto insert =
		duckdb::mssql::codec::binary::FormatSqlLiteral(value, LogicalType::BLOB, LiteralContext::InsertValues);

	CHECK_EQ(filter, std::string("0xDEADBEEF"));
	CHECK_EQ(insert, std::string("0xDEADBEEF"));
	CHECK_EQ(filter, insert);
}

void TestFormatSqlLiteralEmpty() {
	std::cout << "Test: FormatSqlLiteral empty BLOB → \"0x\"\n";
	auto value = Value::BLOB_RAW(std::string());
	auto out = duckdb::mssql::codec::binary::FormatSqlLiteral(value, LogicalType::BLOB, LiteralContext::Filter);
	CHECK_EQ(out, std::string("0x"));
}

void TestFormatDdlTypeNameByteIdentity() {
	std::cout << "Test: FormatDdlTypeName CreateTable == CtasCreateTable (VARBINARY(MAX) byte-identity)\n";
	CTASConfig cfg;
	auto create = duckdb::mssql::codec::binary::FormatDdlTypeName(LogicalType::BLOB, cfg, DdlContext::CreateTable);
	auto ctas = duckdb::mssql::codec::binary::FormatDdlTypeName(LogicalType::BLOB, cfg, DdlContext::CtasCreateTable);
	CHECK_EQ(create, std::string("VARBINARY(MAX)"));
	CHECK_EQ(ctas, std::string("VARBINARY(MAX)"));
}

void TestFormatDdlTypeNameForGeometry() {
	std::cout << "Test: FormatDdlTypeName GEOMETRY → VARBINARY(MAX) (best-effort write path)\n";
	CTASConfig cfg;
	auto out =
		duckdb::mssql::codec::binary::FormatDdlTypeName(LogicalType::GEOMETRY(), cfg, DdlContext::CtasCreateTable);
	CHECK_EQ(out, std::string("VARBINARY(MAX)"));
}

void TestEstimateLiteralSize() {
	std::cout << "Test: EstimateLiteralSize returns generous upper bound\n";
	auto est = duckdb::mssql::codec::binary::EstimateLiteralSize(LogicalType::BLOB);
	CHECK_TRUE(est >= 16);	// at minimum covers a small inline BLOB
}

void TestDispatcherNullAndRoutes() {
	std::cout << "Test: codec::FormatSqlLiteral dispatcher NULL + BLOB + GEOMETRY routing\n";
	CHECK_EQ(
		duckdb::mssql::codec::FormatSqlLiteral(Value(LogicalType::BLOB), LogicalType::BLOB, LiteralContext::Filter),
		std::string("NULL"));
	std::string payload(1, (char)0xff);
	auto blob_value = Value::BLOB_RAW(payload);
	CHECK_EQ(duckdb::mssql::codec::FormatSqlLiteral(blob_value, LogicalType::BLOB, LiteralContext::Filter),
			 std::string("0xFF"));
	// GEOMETRY family routes through Binary codec via type_family.cpp mapping.
	auto geo_value = Value::BLOB_RAW(payload);
	CHECK_EQ(duckdb::mssql::codec::FormatSqlLiteral(geo_value, LogicalType::GEOMETRY(), LiteralContext::InsertValues),
			 std::string("0xFF"));
}

void TestRenderAsString() {
	std::cout << "Test: RenderAsString — same output as FormatSqlLiteral on equivalent bytes\n";
	std::vector<uint8_t> bytes = {0xde, 0xad, 0xbe, 0xef};
	auto rendered = duckdb::mssql::codec::binary::RenderAsString(bytes);
	CHECK_EQ(rendered, std::string("0xDEADBEEF"));

	std::vector<uint8_t> empty;
	auto rendered_empty = duckdb::mssql::codec::binary::RenderAsString(empty);
	CHECK_EQ(rendered_empty, std::string("0x"));
}

}  // namespace

int main() {
	TestDecodeNonPLPEmpty();
	TestDecodePLPPayload();
	TestDecodeIntoGeometryVector();
	TestEncodeNonPLP();
	TestEncodePLPEmpty();
	TestEncodePLPHello();
	TestEncodeValueOverload();
	TestFormatSqlLiteralByteIdentity();
	TestFormatSqlLiteralEmpty();
	TestFormatDdlTypeNameByteIdentity();
	TestFormatDdlTypeNameForGeometry();
	TestEstimateLiteralSize();
	TestDispatcherNullAndRoutes();
	TestRenderAsString();

	if (failures > 0) {
		std::cerr << "\n" << failures << " assertion(s) failed.\n";
		return 1;
	}
	std::cout << "\nAll codec::binary assertions passed.\n";
	return 0;
}
