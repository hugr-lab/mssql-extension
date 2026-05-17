// test/cpp/codec/test_datetime_codec.cpp
// Unit tests for codec::datetime (spec 045, US3 sub-phase 6 — Phase 6).
//
// Does NOT require a running SQL Server instance.
//
// Covers the full SQL Server datetime surface (non-UDT):
//   - DecodeFromTds for DATE / TIME / DATETIME / SMALLDATETIME /
//     DATETIME2 / DATETIMEN (4 + 8) / DATETIMEOFFSET.
//   - EncodeToBcp Vector + Value variants — DATE, TIME(scale=7),
//     DATETIME2 (TIMESTAMP / TIMESTAMP_NS / TIMESTAMP_MS / TIMESTAMP_SEC),
//     DATETIMEOFFSET.
//   - FormatSqlLiteral byte-identity Filter == InsertValues (FR-022) for
//     every DuckDB temporal type id.
//   - FormatDdlTypeName byte-identity CreateTable == CtasCreateTable
//     (FR-024/FR-027/FR-028) — including the new TIMESTAMP_MS/NS/SEC
//     arms (DATETIME2(3) / DATETIME2(7) / DATETIME2(0)).
//   - EstimateLiteralSize upper-bound sanity.
//   - Dispatcher (codec::FormatSqlLiteral) NULL + DATE/TIMESTAMP routing.
//   - RenderAsString — covers every TDS wire format the issue-#89 path
//     can see when a view CAST sends a temporal type into a VARCHAR
//     destination vector.
//
// Build & run:
//   GEN=ninja make debug
//   make test-codec-datetime

#include "codec/datetime_codec.hpp"
#include "codec/literal_context.hpp"
#include "codec/literal_format.hpp"
#include "codec/type_family.hpp"
#include "copy/target_resolver.hpp"
#include "dml/ctas/mssql_ctas_config.hpp"
#include "tds/tds_column_metadata.hpp"
#include "tds/tds_types.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using duckdb::date_t;
using duckdb::dtime_t;
using duckdb::LogicalType;
using duckdb::timestamp_t;
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

duckdb::tds::ColumnMetadata MakeTdsColumn(uint8_t type_id, uint8_t scale = 7) {
	duckdb::tds::ColumnMetadata col{};
	col.type_id = type_id;
	col.scale = scale;
	col.name = "ts_col";
	return col;
}

BCPColumnMetadata MakeBCPColumn(LogicalType ddb_type, uint8_t scale = 7) {
	BCPColumnMetadata col;
	col.name = "ts_col";
	col.duckdb_type = std::move(ddb_type);
	col.tds_type_token = 0x29;
	col.max_length = 8;
	col.precision = 0;
	col.scale = scale;
	col.nullable = true;
	return col;
}

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

// Days from 0001-01-01 to 1970-01-01 = 719162.
constexpr int32_t DAYS_0001_TO_EPOCH = 719162;
// Days from 1900-01-01 to 1970-01-01 = 25567.
constexpr int32_t DAYS_1900_TO_EPOCH = 25567;

//===----------------------------------------------------------------------===//
// DecodeFromTds — covers every TDS wire format.
//===----------------------------------------------------------------------===//

void TestDecodeDate() {
	std::cout << "Test: DecodeFromTds — DATE 2024-01-15\n";
	// 2024-01-15 → days since 0001-01-01 = 719162 + days(1970→2024-01-15)
	auto target = duckdb::Date::FromDate(2024, 1, 15);
	int32_t days_from_0001 = target.days + DAYS_0001_TO_EPOCH;
	std::vector<uint8_t> wire = {
		static_cast<uint8_t>(days_from_0001 & 0xFF),
		static_cast<uint8_t>((days_from_0001 >> 8) & 0xFF),
		static_cast<uint8_t>((days_from_0001 >> 16) & 0xFF),
	};
	duckdb::Vector vec(LogicalType::DATE, 1);
	auto col = MakeTdsColumn(duckdb::tds::TDS_TYPE_DATE, 0);
	duckdb::mssql::codec::datetime::DecodeFromTds(wire, col, vec, 0);
	CHECK_EQ(duckdb::FlatVector::GetData<date_t>(vec)[0].days, target.days);
}

void TestDecodeTimeScale7() {
	std::cout << "Test: DecodeFromTds — TIME(7) 14:30:00 (5-byte form)\n";
	// 14:30:00 = 52200 seconds = 52200 * 10^7 = 522,000,000,000 (100-ns ticks)
	int64_t ticks = 522000000000LL;
	std::vector<uint8_t> wire(5);
	for (int i = 0; i < 5; i++) {
		wire[i] = static_cast<uint8_t>((ticks >> (i * 8)) & 0xFF);
	}
	duckdb::Vector vec(LogicalType::TIME, 1);
	auto col = MakeTdsColumn(duckdb::tds::TDS_TYPE_TIME, 7);
	duckdb::mssql::codec::datetime::DecodeFromTds(wire, col, vec, 0);
	auto expected = duckdb::Time::FromTime(14, 30, 0, 0);
	CHECK_EQ(duckdb::FlatVector::GetData<dtime_t>(vec)[0].micros, expected.micros);
}

void TestDecodeTimeScale0() {
	std::cout << "Test: DecodeFromTds — TIME(0) 14:30:00 (3-byte form)\n";
	// 14:30:00 = 52200 seconds — at scale 0 (3 bytes) ticks are in seconds
	int64_t ticks = 52200;
	std::vector<uint8_t> wire = {
		static_cast<uint8_t>(ticks & 0xFF),
		static_cast<uint8_t>((ticks >> 8) & 0xFF),
		static_cast<uint8_t>((ticks >> 16) & 0xFF),
	};
	duckdb::Vector vec(LogicalType::TIME, 1);
	auto col = MakeTdsColumn(duckdb::tds::TDS_TYPE_TIME, 0);
	duckdb::mssql::codec::datetime::DecodeFromTds(wire, col, vec, 0);
	auto expected = duckdb::Time::FromTime(14, 30, 0, 0);
	CHECK_EQ(duckdb::FlatVector::GetData<dtime_t>(vec)[0].micros, expected.micros);
}

void TestDecodeDatetime() {
	std::cout << "Test: DecodeFromTds — DATETIME (8 bytes, 1/300 s ticks)\n";
	// 2024-01-15 14:30:00 UTC. Days from 1900: epoch_days + (2024-01-15 - 1970-01-01)
	auto target_date = duckdb::Date::FromDate(2024, 1, 15);
	int32_t days_from_1900 = target_date.days + DAYS_1900_TO_EPOCH;
	// 14:30:00 = 52200 seconds = 52200 * 300 = 15,660,000 ticks (1/300 s)
	int32_t ticks = 15660000;
	std::vector<uint8_t> wire(8);
	std::memcpy(&wire[0], &days_from_1900, 4);
	std::memcpy(&wire[4], &ticks, 4);
	duckdb::Vector vec(LogicalType::TIMESTAMP, 1);
	auto col = MakeTdsColumn(duckdb::tds::TDS_TYPE_DATETIME, 0);
	duckdb::mssql::codec::datetime::DecodeFromTds(wire, col, vec, 0);
	auto expected = duckdb::Timestamp::FromDatetime(target_date, duckdb::Time::FromTime(14, 30, 0, 0));
	CHECK_EQ(duckdb::FlatVector::GetData<timestamp_t>(vec)[0].value, expected.value);
}

void TestDecodeSmallDatetime() {
	std::cout << "Test: DecodeFromTds — SMALLDATETIME (4 bytes, minute resolution)\n";
	auto target_date = duckdb::Date::FromDate(2024, 1, 15);
	uint16_t days_from_1900 = static_cast<uint16_t>(target_date.days + DAYS_1900_TO_EPOCH);
	uint16_t minutes = 14 * 60 + 30;  // 14:30
	std::vector<uint8_t> wire(4);
	std::memcpy(&wire[0], &days_from_1900, 2);
	std::memcpy(&wire[2], &minutes, 2);
	duckdb::Vector vec(LogicalType::TIMESTAMP, 1);
	auto col = MakeTdsColumn(duckdb::tds::TDS_TYPE_SMALLDATETIME, 0);
	duckdb::mssql::codec::datetime::DecodeFromTds(wire, col, vec, 0);
	auto expected = duckdb::Timestamp::FromDatetime(target_date, duckdb::Time::FromTime(14, 30, 0, 0));
	CHECK_EQ(duckdb::FlatVector::GetData<timestamp_t>(vec)[0].value, expected.value);
}

void TestDecodeDatetimen8() {
	std::cout << "Test: DecodeFromTds — DATETIMEN length=8 → DATETIME path\n";
	auto target_date = duckdb::Date::FromDate(2024, 1, 15);
	int32_t days_from_1900 = target_date.days + DAYS_1900_TO_EPOCH;
	int32_t ticks = 15660000;
	std::vector<uint8_t> wire(8);
	std::memcpy(&wire[0], &days_from_1900, 4);
	std::memcpy(&wire[4], &ticks, 4);
	duckdb::Vector vec(LogicalType::TIMESTAMP, 1);
	auto col = MakeTdsColumn(duckdb::tds::TDS_TYPE_DATETIMEN, 0);
	duckdb::mssql::codec::datetime::DecodeFromTds(wire, col, vec, 0);
	auto expected = duckdb::Timestamp::FromDatetime(target_date, duckdb::Time::FromTime(14, 30, 0, 0));
	CHECK_EQ(duckdb::FlatVector::GetData<timestamp_t>(vec)[0].value, expected.value);
}

void TestDecodeDatetimen4() {
	std::cout << "Test: DecodeFromTds — DATETIMEN length=4 → SMALLDATETIME path\n";
	auto target_date = duckdb::Date::FromDate(2024, 1, 15);
	uint16_t days_from_1900 = static_cast<uint16_t>(target_date.days + DAYS_1900_TO_EPOCH);
	uint16_t minutes = 14 * 60 + 30;
	std::vector<uint8_t> wire(4);
	std::memcpy(&wire[0], &days_from_1900, 2);
	std::memcpy(&wire[2], &minutes, 2);
	duckdb::Vector vec(LogicalType::TIMESTAMP, 1);
	auto col = MakeTdsColumn(duckdb::tds::TDS_TYPE_DATETIMEN, 0);
	duckdb::mssql::codec::datetime::DecodeFromTds(wire, col, vec, 0);
	auto expected = duckdb::Timestamp::FromDatetime(target_date, duckdb::Time::FromTime(14, 30, 0, 0));
	CHECK_EQ(duckdb::FlatVector::GetData<timestamp_t>(vec)[0].value, expected.value);
}

void TestDecodeDatetime2Scale6() {
	std::cout << "Test: DecodeFromTds — DATETIME2(6) (μs precision, 8-byte form: 5 time + 3 date)\n";
	// scale 5-7 → 5-byte time. 14:30:00.123456 at scale 6 — ticks are μs.
	int64_t time_ticks = (14LL * 3600 + 30 * 60) * 1000000LL + 123456;	// 52200123456
	auto target_date = duckdb::Date::FromDate(2024, 1, 15);
	int32_t days_from_0001 = target_date.days + DAYS_0001_TO_EPOCH;
	std::vector<uint8_t> wire(8);
	for (int i = 0; i < 5; i++) {
		wire[i] = static_cast<uint8_t>((time_ticks >> (i * 8)) & 0xFF);
	}
	wire[5] = static_cast<uint8_t>(days_from_0001 & 0xFF);
	wire[6] = static_cast<uint8_t>((days_from_0001 >> 8) & 0xFF);
	wire[7] = static_cast<uint8_t>((days_from_0001 >> 16) & 0xFF);
	duckdb::Vector vec(LogicalType::TIMESTAMP, 1);
	auto col = MakeTdsColumn(duckdb::tds::TDS_TYPE_DATETIME2, 6);
	duckdb::mssql::codec::datetime::DecodeFromTds(wire, col, vec, 0);
	auto expected = duckdb::Timestamp::FromDatetime(target_date, duckdb::Time::FromTime(14, 30, 0, 123456));
	CHECK_EQ(duckdb::FlatVector::GetData<timestamp_t>(vec)[0].value, expected.value);
}

void TestDecodeDatetimeOffset() {
	std::cout << "Test: DecodeFromTds — DATETIMEOFFSET(7) reads UTC time directly\n";
	// 14:30:00 UTC encoded as 100-ns ticks.
	int64_t time_ticks = (14LL * 3600 + 30 * 60) * 10000000LL;
	auto target_date = duckdb::Date::FromDate(2024, 1, 15);
	int32_t days_from_0001 = target_date.days + DAYS_0001_TO_EPOCH;
	int16_t offset = 0;
	std::vector<uint8_t> wire(10);
	for (int i = 0; i < 5; i++) {
		wire[i] = static_cast<uint8_t>((time_ticks >> (i * 8)) & 0xFF);
	}
	wire[5] = static_cast<uint8_t>(days_from_0001 & 0xFF);
	wire[6] = static_cast<uint8_t>((days_from_0001 >> 8) & 0xFF);
	wire[7] = static_cast<uint8_t>((days_from_0001 >> 16) & 0xFF);
	std::memcpy(&wire[8], &offset, 2);
	duckdb::Vector vec(LogicalType::TIMESTAMP_TZ, 1);
	auto col = MakeTdsColumn(duckdb::tds::TDS_TYPE_DATETIMEOFFSET, 7);
	duckdb::mssql::codec::datetime::DecodeFromTds(wire, col, vec, 0);
	auto expected = duckdb::Timestamp::FromDatetime(target_date, duckdb::Time::FromTime(14, 30, 0, 0));
	CHECK_EQ(duckdb::FlatVector::GetData<timestamp_t>(vec)[0].value, expected.value);
}

//===----------------------------------------------------------------------===//
// EncodeToBcp — Vector + Value overloads, covering wire-size variants.
//===----------------------------------------------------------------------===//

void TestEncodeDateVector() {
	std::cout << "Test: EncodeToBcp(Vector) DATE — 3-byte LE, length-prefixed\n";
	duckdb::Vector vec(LogicalType::DATE, 1);
	auto target = duckdb::Date::FromDate(2024, 1, 15);
	duckdb::FlatVector::GetData<date_t>(vec)[0] = target;
	auto col = MakeBCPColumn(LogicalType::DATE, 0);
	duckdb::vector<uint8_t> buf;
	duckdb::mssql::codec::datetime::EncodeToBcp(vec, 0, col, buf);
	// Expected: [3][days_from_0001 LE]
	int32_t days_from_0001 = target.days + DAYS_0001_TO_EPOCH;
	CHECK_EQ(buf.size(), 4u);
	CHECK_EQ(static_cast<int>(buf[0]), 3);
	int32_t recovered = buf[1] | (buf[2] << 8) | (buf[3] << 16);
	CHECK_EQ(recovered, days_from_0001);
}

void TestEncodeDateValue() {
	std::cout << "Test: EncodeToBcp(Value) DATE — round-trip via Value\n";
	auto target = duckdb::Date::FromDate(2024, 1, 15);
	auto value = Value::DATE(target);
	auto col = MakeBCPColumn(LogicalType::DATE, 0);
	duckdb::vector<uint8_t> buf;
	duckdb::mssql::codec::datetime::EncodeToBcp(value, col, buf);
	int32_t days_from_0001 = target.days + DAYS_0001_TO_EPOCH;
	int32_t recovered = buf[1] | (buf[2] << 8) | (buf[3] << 16);
	CHECK_EQ(recovered, days_from_0001);
}

void TestEncodeTimeScale7Vector() {
	std::cout << "Test: EncodeToBcp(Vector) TIME(7) — 5-byte 100-ns ticks\n";
	duckdb::Vector vec(LogicalType::TIME, 1);
	auto target = duckdb::Time::FromTime(14, 30, 0, 0);
	duckdb::FlatVector::GetData<dtime_t>(vec)[0] = target;
	auto col = MakeBCPColumn(LogicalType::TIME, 7);
	duckdb::vector<uint8_t> buf;
	duckdb::mssql::codec::datetime::EncodeToBcp(vec, 0, col, buf);
	CHECK_EQ(static_cast<int>(buf[0]), 5);	// 5-byte time at scale 7
	int64_t expected_ticks = 522000000000LL;
	int64_t recovered = 0;
	for (int i = 0; i < 5; i++) {
		recovered |= static_cast<int64_t>(buf[1 + i]) << (i * 8);
	}
	CHECK_EQ(recovered, expected_ticks);
}

void TestEncodeTimestampVector() {
	std::cout << "Test: EncodeToBcp(Vector) TIMESTAMP — DATETIME2 path (3-byte date + 5-byte time)\n";
	duckdb::Vector vec(LogicalType::TIMESTAMP, 1);
	auto target_date = duckdb::Date::FromDate(2024, 1, 15);
	auto target = duckdb::Timestamp::FromDatetime(target_date, duckdb::Time::FromTime(14, 30, 0, 0));
	duckdb::FlatVector::GetData<timestamp_t>(vec)[0] = target;
	auto col = MakeBCPColumn(LogicalType::TIMESTAMP, 7);
	duckdb::vector<uint8_t> buf;
	duckdb::mssql::codec::datetime::EncodeToBcp(vec, 0, col, buf);
	CHECK_EQ(static_cast<int>(buf[0]), 8);	// 5 time + 3 date
}

void TestEncodeTimestampTZValue() {
	std::cout << "Test: EncodeToBcp(Value) TIMESTAMP_TZ → DATETIMEOFFSET (offset=0 trailer)\n";
	auto target_date = duckdb::Date::FromDate(2024, 1, 15);
	auto ts = duckdb::Timestamp::FromDatetime(target_date, duckdb::Time::FromTime(14, 30, 0, 0));
	auto value = Value::TIMESTAMPTZ(duckdb::timestamp_tz_t(ts.value));
	auto col = MakeBCPColumn(LogicalType::TIMESTAMP_TZ, 7);
	duckdb::vector<uint8_t> buf;
	duckdb::mssql::codec::datetime::EncodeToBcp(value, col, buf);
	CHECK_EQ(static_cast<int>(buf[0]), 10);	 // 5 time + 3 date + 2 offset
	// Trailing 2 bytes are signed offset minutes (always 0 from our encoder).
	int16_t offset_minutes = static_cast<int16_t>(buf[9] | (buf[10] << 8));
	CHECK_EQ(static_cast<int>(offset_minutes), 0);
}

void TestEncodeTimestampMSandSEC() {
	std::cout << "Test: EncodeToBcp(Vector) TIMESTAMP_MS and TIMESTAMP_S route through DATETIME2\n";
	// In production these come from a DuckDB cast that fills a Vector of the
	// target logical type; the codec uses GetVectorValue<timestamp_t> on the
	// raw 64-bit storage (all TIMESTAMP_* share the same physical layout).
	auto target_date = duckdb::Date::FromDate(2024, 1, 15);
	auto ts = duckdb::Timestamp::FromDatetime(target_date, duckdb::Time::FromTime(14, 30, 0, 0));

	duckdb::Vector vec_ms(LogicalType::TIMESTAMP_MS, 1);
	duckdb::FlatVector::GetData<timestamp_t>(vec_ms)[0] = ts;
	auto col_ms = MakeBCPColumn(LogicalType::TIMESTAMP_MS, 3);
	duckdb::vector<uint8_t> buf_ms;
	duckdb::mssql::codec::datetime::EncodeToBcp(vec_ms, 0, col_ms, buf_ms);
	CHECK_EQ(static_cast<int>(buf_ms[0]), 7);  // 4 time @ scale 3 + 3 date

	duckdb::Vector vec_sec(LogicalType::TIMESTAMP_S, 1);
	duckdb::FlatVector::GetData<timestamp_t>(vec_sec)[0] = ts;
	auto col_sec = MakeBCPColumn(LogicalType::TIMESTAMP_S, 0);
	duckdb::vector<uint8_t> buf_sec;
	duckdb::mssql::codec::datetime::EncodeToBcp(vec_sec, 0, col_sec, buf_sec);
	CHECK_EQ(static_cast<int>(buf_sec[0]), 6);	// 3 time @ scale 0 + 3 date
}

//===----------------------------------------------------------------------===//
// FormatSqlLiteral — FR-022 byte-identity for every DuckDB temporal type.
//===----------------------------------------------------------------------===//

void TestFormatSqlLiteralByteIdentity() {
	std::cout << "Test: FormatSqlLiteral Filter == InsertValues for every temporal type (FR-022)\n";

	auto date_v = Value::DATE(duckdb::Date::FromDate(2024, 1, 15));
	auto time_v = Value::TIME(duckdb::Time::FromTime(14, 30, 0, 123456));
	auto ts_v = Value::TIMESTAMP(duckdb::Timestamp::FromDatetime(duckdb::Date::FromDate(2024, 1, 15),
																 duckdb::Time::FromTime(14, 30, 0, 123456)));
	auto tz_ts =
		duckdb::Timestamp::FromDatetime(duckdb::Date::FromDate(2024, 1, 15), duckdb::Time::FromTime(14, 30, 0, 0));
	auto tstz_v = Value::TIMESTAMPTZ(duckdb::timestamp_tz_t(tz_ts.value));

	struct Case {
		Value v;
		LogicalType t;
	};
	std::vector<Case> cases = {
		{date_v, LogicalType::DATE},
		{time_v, LogicalType::TIME},
		{ts_v, LogicalType::TIMESTAMP},
		{tstz_v, LogicalType::TIMESTAMP_TZ},
	};
	for (auto &c : cases) {
		auto filter = duckdb::mssql::codec::datetime::FormatSqlLiteral(c.v, c.t, LiteralContext::Filter);
		auto insert = duckdb::mssql::codec::datetime::FormatSqlLiteral(c.v, c.t, LiteralContext::InsertValues);
		CHECK_EQ(filter, insert);
	}

	// Spot-check the canonical forms.
	CHECK_EQ(duckdb::mssql::codec::datetime::FormatSqlLiteral(date_v, LogicalType::DATE, LiteralContext::Filter),
			 std::string("'2024-01-15'"));
	CHECK_EQ(duckdb::mssql::codec::datetime::FormatSqlLiteral(time_v, LogicalType::TIME, LiteralContext::Filter),
			 std::string("'14:30:00.1234560'"));
	CHECK_EQ(duckdb::mssql::codec::datetime::FormatSqlLiteral(ts_v, LogicalType::TIMESTAMP, LiteralContext::Filter),
			 std::string("CAST('2024-01-15T14:30:00.1234560' AS DATETIME2(7))"));
	CHECK_EQ(
		duckdb::mssql::codec::datetime::FormatSqlLiteral(tstz_v, LogicalType::TIMESTAMP_TZ, LiteralContext::Filter),
		std::string("CAST('2024-01-15T14:30:00.0000000+00:00' AS DATETIMEOFFSET(7))"));
}

void TestFormatSqlLiteralNull() {
	std::cout << "Test: FormatSqlLiteral NULL → \"NULL\"\n";
	auto null_v = Value(LogicalType::TIMESTAMP);
	CHECK_EQ(duckdb::mssql::codec::datetime::FormatSqlLiteral(null_v, LogicalType::TIMESTAMP, LiteralContext::Filter),
			 std::string("NULL"));
}

//===----------------------------------------------------------------------===//
// FormatDdlTypeName — FR-024/FR-027/FR-028 byte-identity for every type.
//===----------------------------------------------------------------------===//

void TestFormatDdlTypeNameByteIdentity() {
	std::cout << "Test: FormatDdlTypeName CreateTable == CtasCreateTable for every temporal type (FR-027/FR-028)\n";
	CTASConfig cfg;
	struct Case {
		LogicalType t;
		std::string expected;
	};
	std::vector<Case> cases = {
		{LogicalType::DATE, "DATE"},
		{LogicalType::TIME, "TIME(7)"},
		{LogicalType::TIMESTAMP, "DATETIME2(6)"},
		{LogicalType::TIMESTAMP_MS, "DATETIME2(3)"},
		{LogicalType::TIMESTAMP_NS, "DATETIME2(7)"},
		{LogicalType::TIMESTAMP_S, "DATETIME2(0)"},
		{LogicalType::TIMESTAMP_TZ, "DATETIMEOFFSET(7)"},
	};
	for (auto &c : cases) {
		auto create = duckdb::mssql::codec::datetime::FormatDdlTypeName(c.t, cfg, DdlContext::CreateTable);
		auto ctas = duckdb::mssql::codec::datetime::FormatDdlTypeName(c.t, cfg, DdlContext::CtasCreateTable);
		CHECK_EQ(create, ctas);
		CHECK_EQ(create, c.expected);
	}
}

//===----------------------------------------------------------------------===//
// EstimateLiteralSize — sanity upper bound for every type.
//===----------------------------------------------------------------------===//

void TestEstimateLiteralSize() {
	std::cout << "Test: EstimateLiteralSize covers every DuckDB temporal type\n";
	CHECK_TRUE(duckdb::mssql::codec::datetime::EstimateLiteralSize(LogicalType::DATE) >= 12);
	CHECK_TRUE(duckdb::mssql::codec::datetime::EstimateLiteralSize(LogicalType::TIME) >= 20);
	CHECK_TRUE(duckdb::mssql::codec::datetime::EstimateLiteralSize(LogicalType::TIMESTAMP) >= 50);
	CHECK_TRUE(duckdb::mssql::codec::datetime::EstimateLiteralSize(LogicalType::TIMESTAMP_MS) >= 50);
	CHECK_TRUE(duckdb::mssql::codec::datetime::EstimateLiteralSize(LogicalType::TIMESTAMP_NS) >= 50);
	CHECK_TRUE(duckdb::mssql::codec::datetime::EstimateLiteralSize(LogicalType::TIMESTAMP_S) >= 50);
	CHECK_TRUE(duckdb::mssql::codec::datetime::EstimateLiteralSize(LogicalType::TIMESTAMP_TZ) >= 65);
}

//===----------------------------------------------------------------------===//
// Dispatcher — codec::FormatSqlLiteral should route DATE/TIMESTAMP/TZ
// through the DateTime family arm.
//===----------------------------------------------------------------------===//

void TestDispatcherRouting() {
	std::cout << "Test: codec::FormatSqlLiteral dispatcher routes DateTime family\n";
	CHECK_EQ(
		duckdb::mssql::codec::FormatSqlLiteral(Value(LogicalType::DATE), LogicalType::DATE, LiteralContext::Filter),
		std::string("NULL"));
	auto date_v = Value::DATE(duckdb::Date::FromDate(2024, 1, 15));
	CHECK_EQ(duckdb::mssql::codec::FormatSqlLiteral(date_v, LogicalType::DATE, LiteralContext::InsertValues),
			 std::string("'2024-01-15'"));
	auto ts =
		duckdb::Timestamp::FromDatetime(duckdb::Date::FromDate(2024, 1, 15), duckdb::Time::FromTime(14, 30, 0, 0));
	auto tstz_v = Value::TIMESTAMPTZ(duckdb::timestamp_tz_t(ts.value));
	auto rendered = duckdb::mssql::codec::FormatSqlLiteral(tstz_v, LogicalType::TIMESTAMP_TZ, LiteralContext::Filter);
	CHECK_EQ(rendered, std::string("CAST('2024-01-15T14:30:00.0000000+00:00' AS DATETIMEOFFSET(7))"));
}

//===----------------------------------------------------------------------===//
// RenderAsString — issue-#89 fallback. Covers every TDS wire format the
// fallback dispatcher can encounter.
//===----------------------------------------------------------------------===//

void TestRenderAsStringAllWireFormats() {
	std::cout << "Test: RenderAsString covers every TDS temporal wire format (issue-#89 fallback)\n";
	auto target_date = duckdb::Date::FromDate(2024, 1, 15);

	// DATE
	{
		int32_t days_from_0001 = target_date.days + DAYS_0001_TO_EPOCH;
		std::vector<uint8_t> wire = {
			static_cast<uint8_t>(days_from_0001 & 0xFF),
			static_cast<uint8_t>((days_from_0001 >> 8) & 0xFF),
			static_cast<uint8_t>((days_from_0001 >> 16) & 0xFF),
		};
		auto col = MakeTdsColumn(duckdb::tds::TDS_TYPE_DATE, 0);
		CHECK_EQ(duckdb::mssql::codec::datetime::RenderAsString(wire, col), std::string("2024-01-15"));
	}

	// TIME scale=7
	{
		int64_t ticks = 522000000000LL;
		std::vector<uint8_t> wire(5);
		for (int i = 0; i < 5; i++)
			wire[i] = static_cast<uint8_t>((ticks >> (i * 8)) & 0xFF);
		auto col = MakeTdsColumn(duckdb::tds::TDS_TYPE_TIME, 7);
		CHECK_EQ(duckdb::mssql::codec::datetime::RenderAsString(wire, col), std::string("14:30:00.0000000"));
	}

	// DATETIME (8 bytes)
	{
		int32_t days_from_1900 = target_date.days + DAYS_1900_TO_EPOCH;
		int32_t ticks = 15660000;
		std::vector<uint8_t> wire(8);
		std::memcpy(&wire[0], &days_from_1900, 4);
		std::memcpy(&wire[4], &ticks, 4);
		auto col = MakeTdsColumn(duckdb::tds::TDS_TYPE_DATETIME, 0);
		CHECK_EQ(duckdb::mssql::codec::datetime::RenderAsString(wire, col), std::string("2024-01-15 14:30:00.0000000"));
	}

	// SMALLDATETIME (4 bytes)
	{
		uint16_t days_from_1900 = static_cast<uint16_t>(target_date.days + DAYS_1900_TO_EPOCH);
		uint16_t minutes = 14 * 60 + 30;
		std::vector<uint8_t> wire(4);
		std::memcpy(&wire[0], &days_from_1900, 2);
		std::memcpy(&wire[2], &minutes, 2);
		auto col = MakeTdsColumn(duckdb::tds::TDS_TYPE_SMALLDATETIME, 0);
		CHECK_EQ(duckdb::mssql::codec::datetime::RenderAsString(wire, col), std::string("2024-01-15 14:30:00.0000000"));
	}

	// DATETIME2 scale=7 (8 bytes)
	{
		int64_t time_ticks = (14LL * 3600 + 30 * 60) * 10000000LL;
		int32_t days_from_0001 = target_date.days + DAYS_0001_TO_EPOCH;
		std::vector<uint8_t> wire(8);
		for (int i = 0; i < 5; i++)
			wire[i] = static_cast<uint8_t>((time_ticks >> (i * 8)) & 0xFF);
		wire[5] = static_cast<uint8_t>(days_from_0001 & 0xFF);
		wire[6] = static_cast<uint8_t>((days_from_0001 >> 8) & 0xFF);
		wire[7] = static_cast<uint8_t>((days_from_0001 >> 16) & 0xFF);
		auto col = MakeTdsColumn(duckdb::tds::TDS_TYPE_DATETIME2, 7);
		CHECK_EQ(duckdb::mssql::codec::datetime::RenderAsString(wire, col), std::string("2024-01-15 14:30:00.0000000"));
	}

	// DATETIMEN length=8 (acts as DATETIME)
	{
		int32_t days_from_1900 = target_date.days + DAYS_1900_TO_EPOCH;
		int32_t ticks = 15660000;
		std::vector<uint8_t> wire(8);
		std::memcpy(&wire[0], &days_from_1900, 4);
		std::memcpy(&wire[4], &ticks, 4);
		auto col = MakeTdsColumn(duckdb::tds::TDS_TYPE_DATETIMEN, 0);
		CHECK_EQ(duckdb::mssql::codec::datetime::RenderAsString(wire, col), std::string("2024-01-15 14:30:00.0000000"));
	}

	// DATETIMEN length=4 (acts as SMALLDATETIME)
	{
		uint16_t days_from_1900 = static_cast<uint16_t>(target_date.days + DAYS_1900_TO_EPOCH);
		uint16_t minutes = 14 * 60 + 30;
		std::vector<uint8_t> wire(4);
		std::memcpy(&wire[0], &days_from_1900, 2);
		std::memcpy(&wire[2], &minutes, 2);
		auto col = MakeTdsColumn(duckdb::tds::TDS_TYPE_DATETIMEN, 0);
		CHECK_EQ(duckdb::mssql::codec::datetime::RenderAsString(wire, col), std::string("2024-01-15 14:30:00.0000000"));
	}

	// DATETIMEOFFSET scale=7 (10 bytes)
	{
		int64_t time_ticks = (14LL * 3600 + 30 * 60) * 10000000LL;
		int32_t days_from_0001 = target_date.days + DAYS_0001_TO_EPOCH;
		int16_t offset = 0;
		std::vector<uint8_t> wire(10);
		for (int i = 0; i < 5; i++)
			wire[i] = static_cast<uint8_t>((time_ticks >> (i * 8)) & 0xFF);
		wire[5] = static_cast<uint8_t>(days_from_0001 & 0xFF);
		wire[6] = static_cast<uint8_t>((days_from_0001 >> 8) & 0xFF);
		wire[7] = static_cast<uint8_t>((days_from_0001 >> 16) & 0xFF);
		std::memcpy(&wire[8], &offset, 2);
		auto col = MakeTdsColumn(duckdb::tds::TDS_TYPE_DATETIMEOFFSET, 7);
		CHECK_EQ(duckdb::mssql::codec::datetime::RenderAsString(wire, col),
				 std::string("2024-01-15 14:30:00.0000000+00:00"));
	}
}

}  // namespace

int main() {
	TestDecodeDate();
	TestDecodeTimeScale7();
	TestDecodeTimeScale0();
	TestDecodeDatetime();
	TestDecodeSmallDatetime();
	TestDecodeDatetimen8();
	TestDecodeDatetimen4();
	TestDecodeDatetime2Scale6();
	TestDecodeDatetimeOffset();

	TestEncodeDateVector();
	TestEncodeDateValue();
	TestEncodeTimeScale7Vector();
	TestEncodeTimestampVector();
	TestEncodeTimestampTZValue();
	TestEncodeTimestampMSandSEC();

	TestFormatSqlLiteralByteIdentity();
	TestFormatSqlLiteralNull();
	TestFormatDdlTypeNameByteIdentity();
	TestEstimateLiteralSize();
	TestDispatcherRouting();
	TestRenderAsStringAllWireFormats();

	if (failures == 0) {
		std::cout << "\nAll codec::datetime tests passed.\n";
		return 0;
	}
	std::cerr << "\n" << failures << " codec::datetime test(s) FAILED.\n";
	return 1;
}
