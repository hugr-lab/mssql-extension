// test/cpp/test_bcp_column_metadata.cpp
// Unit tests for BCPColumnMetadata::FromServerColumn (spec 062 W3): the
// metadata of an EXISTING column from the seven fields sys.columns gives for
// it. No SQL Server needed.
//
// Why a unit test and not the COPY suite: the same function now feeds two
// callers -- COPY's own metadata query, and the catalog cache for INSERT via
// BCP (no round trip) -- and later spec 066's `#temp` declaration. COLMETADATA
// that disagrees with the target by one byte of length or one type token fails
// the load with a server error that names the column and nothing else, so the
// table below pins every rule the resolver's loop carried:
//   - the TDS token and wire length per type, including money-as-decimal
//     and the varchar->nvarchar doubling;
//   - the spec 060 / #225 UTF-8 retarget (BIGVARCHAR, bytes unhalved, the
//     UTF-8 wire collation, the collation name kept for the INSERT BULK text);
//   - the DuckDB type the encoder is told to produce (tinyint is UTINYINT);
//   - MAX (-1) and the inline limit (varchar over 4000 goes PLP).
//
// Build & run:
//   make test-cpp-run

#include "copy/target_resolver.hpp"
#include "duckdb/common/types.hpp"
#include "tds/tds_types.hpp"

#include <array>
#include <iostream>
#include <string>

using duckdb::LogicalTypeId;
using duckdb::mssql::BCPColumnMetadata;

namespace {

int failures = 0;

#define CHECK(cond)                                                  \
	do {                                                             \
		if (!(cond)) {                                               \
			++failures;                                              \
			std::cerr << "FAIL [" << __LINE__ << "] " #cond << "\n"; \
		}                                                            \
	} while (0)

#define CHECK_EQ(actual, expected)                                                                                  \
	do {                                                                                                            \
		const auto _a = (actual);                                                                                   \
		const auto _e = (expected);                                                                                 \
		if (!(_a == _e)) {                                                                                          \
			++failures;                                                                                             \
			std::cerr << "FAIL [" << __LINE__ << "] " #actual " == " #expected << "\n  actual:   " << (long long)_a \
					  << "\n  expected: " << (long long)_e << "\n";                                                 \
		}                                                                                                           \
	} while (0)

BCPColumnMetadata Col(const std::string &type, int16_t max_length, uint8_t precision = 0, uint8_t scale = 0,
					  bool nullable = true, const std::string &collation = "") {
	return BCPColumnMetadata::FromServerColumn("c", type, max_length, precision, scale, nullable, collation);
}

void TestFixedWidth() {
	auto c = Col("int", 4, 10, 0, false);
	CHECK_EQ(c.tds_type_token, duckdb::tds::TDS_TYPE_INTN);
	CHECK_EQ(c.max_length, 4);
	CHECK(c.duckdb_type.id() == LogicalTypeId::INTEGER);
	CHECK(!c.nullable);
	CHECK(c.name == "c");

	CHECK_EQ(Col("bigint", 8).max_length, 8);
	CHECK(Col("bigint", 8).duckdb_type.id() == LogicalTypeId::BIGINT);
	CHECK_EQ(Col("smallint", 2).max_length, 2);
	CHECK(Col("smallint", 2).duckdb_type.id() == LogicalTypeId::SMALLINT);

	// tinyint is the server's one UNSIGNED byte, and the catalog reports it as
	// UTINYINT too; TINYINT here would mean a signed source travelling as a
	// smallint.
	auto t = Col("tinyint", 1);
	CHECK_EQ(t.tds_type_token, duckdb::tds::TDS_TYPE_INTN);
	CHECK_EQ(t.max_length, 1);
	CHECK(t.duckdb_type.id() == LogicalTypeId::UTINYINT);

	auto b = Col("bit", 1);
	CHECK_EQ(b.tds_type_token, duckdb::tds::TDS_TYPE_BITN);
	CHECK_EQ(b.max_length, 1);
	CHECK(b.duckdb_type.id() == LogicalTypeId::BOOLEAN);

	CHECK_EQ(Col("real", 4).tds_type_token, duckdb::tds::TDS_TYPE_FLOATN);
	CHECK_EQ(Col("real", 4).max_length, 4);
	CHECK(Col("real", 4).duckdb_type.id() == LogicalTypeId::FLOAT);
	CHECK_EQ(Col("float", 8).max_length, 8);
	CHECK(Col("float", 8).duckdb_type.id() == LogicalTypeId::DOUBLE);

	// Type names arrive as the server spells them; case must not matter.
	CHECK_EQ(Col("INT", 4).max_length, 4);
	CHECK(Col("Int", 4).duckdb_type.id() == LogicalTypeId::INTEGER);
}

void TestDecimalAndMoney() {
	auto d = Col("decimal", 9, 18, 2);
	CHECK_EQ(d.tds_type_token, duckdb::tds::TDS_TYPE_DECIMAL);
	CHECK_EQ(d.max_length, 9);	// precision 10..19 -> 9 bytes
	CHECK_EQ(d.precision, 18);
	CHECK_EQ(d.scale, 2);
	CHECK(d.duckdb_type.id() == LogicalTypeId::DECIMAL);
	CHECK_EQ(duckdb::DecimalType::GetWidth(d.duckdb_type), 18);
	CHECK_EQ(duckdb::DecimalType::GetScale(d.duckdb_type), 2);

	CHECK_EQ(Col("numeric", 5, 9, 0).max_length, 5);
	CHECK_EQ(Col("numeric", 13, 28, 4).max_length, 13);
	CHECK_EQ(Col("numeric", 17, 38, 0).max_length, 17);

	// money goes on the wire as a decimal of its precision (19,4) -> 9 bytes,
	// not the 8 bytes of a MONEY wire form nothing writes.
	auto m = Col("money", 8, 19, 4);
	CHECK_EQ(m.tds_type_token, duckdb::tds::TDS_TYPE_DECIMAL);
	CHECK_EQ(m.max_length, 9);
	CHECK(m.duckdb_type.id() == LogicalTypeId::DECIMAL);
	CHECK_EQ(duckdb::DecimalType::GetWidth(m.duckdb_type), 19);
	CHECK_EQ(duckdb::DecimalType::GetScale(m.duckdb_type), 4);
	auto sm = Col("smallmoney", 4, 10, 4);
	CHECK_EQ(sm.max_length, 9);
	CHECK_EQ(duckdb::DecimalType::GetWidth(sm.duckdb_type), 10);
}

void TestStrings() {
	// nvarchar: sys.columns max_length is already bytes (2 per char).
	auto n = Col("nvarchar", 100, 0, 0, true, "SQL_Latin1_General_CP1_CI_AS");
	CHECK_EQ(n.tds_type_token, duckdb::tds::TDS_TYPE_NVARCHAR);
	CHECK_EQ(n.max_length, 100);
	CHECK(n.duckdb_type.id() == LogicalTypeId::VARCHAR);
	CHECK(n.collation_name.empty());  // no UTF-8 retarget: the wire default collation
	CHECK_EQ(Col("nvarchar", -1).max_length, 0xFFFF);
	CHECK_EQ(Col("nchar", 20).max_length, 20);
	CHECK_EQ(Col("ntext", 16).max_length, 0xFFFF);

	// varchar under a non-UTF-8 collation travels as NVARCHAR: single-byte
	// characters become UTF-16, so the declared length doubles; past 4000
	// characters it cannot be inline and goes PLP.
	auto v = Col("varchar", 20, 0, 0, true, "SQL_Latin1_General_CP1_CI_AS");
	CHECK_EQ(v.tds_type_token, duckdb::tds::TDS_TYPE_NVARCHAR);
	CHECK_EQ(v.max_length, 40);
	CHECK(v.collation_name.empty());
	CHECK_EQ(Col("varchar", 5000, 0, 0, true, "SQL_Latin1_General_CP1_CI_AS").max_length, 0xFFFF);
	CHECK_EQ(Col("varchar", -1, 0, 0, true, "SQL_Latin1_General_CP1_CI_AS").max_length, 0xFFFF);
	CHECK_EQ(Col("char", 10).max_length, 20);
	CHECK_EQ(Col("text", 16).max_length, 0xFFFF);

	// Spec 060 / #225: a char column under a UTF-8 collation takes the bytes
	// we hold -- BIGVARCHAR, length unhalved, the UTF-8 wire collation, and the
	// collation NAME kept for the INSERT BULK column list (the server reads the
	// bytes by the collation in the statement text, not the COLMETADATA one).
	const std::array<uint8_t, 5> utf8_wire = {0x09, 0x04, 0xD0, 0x24, 0x00};
	auto u = Col("varchar", 20, 0, 0, false, "Latin1_General_100_BIN2_UTF8");
	CHECK_EQ(u.tds_type_token, duckdb::tds::TDS_TYPE_BIGVARCHAR);
	CHECK_EQ(u.max_length, 20);
	CHECK(u.collation == utf8_wire);
	CHECK(u.collation_name == "Latin1_General_100_BIN2_UTF8");
	CHECK(!u.nullable);
	CHECK(u.duckdb_type.id() == LogicalTypeId::VARCHAR);
	auto umax = Col("varchar", -1, 0, 0, true, "Latin1_General_100_CI_AS_SC_UTF8");
	CHECK_EQ(umax.tds_type_token, duckdb::tds::TDS_TYPE_BIGVARCHAR);
	CHECK_EQ(umax.max_length, 0xFFFF);
	// The name test is case-insensitive and only a char type qualifies.
	CHECK_EQ(Col("char", 8, 0, 0, true, "latin1_general_100_bin2_utf8").tds_type_token,
			 duckdb::tds::TDS_TYPE_BIGVARCHAR);
	CHECK_EQ(Col("nvarchar", 16, 0, 0, true, "Latin1_General_100_BIN2_UTF8").tds_type_token,
			 duckdb::tds::TDS_TYPE_NVARCHAR);
}

void TestBinaryAndTemporal() {
	auto vb = Col("varbinary", -1);
	CHECK_EQ(vb.tds_type_token, duckdb::tds::TDS_TYPE_BIGVARBINARY);
	CHECK_EQ(vb.max_length, 0xFFFF);
	CHECK(vb.duckdb_type.id() == LogicalTypeId::BLOB);
	CHECK(Col("image", 16).duckdb_type.id() == LogicalTypeId::BLOB);

	CHECK_EQ(Col("uniqueidentifier", 16).tds_type_token, duckdb::tds::TDS_TYPE_UNIQUEIDENTIFIER);
	CHECK(Col("uniqueidentifier", 16).duckdb_type.id() == LogicalTypeId::UUID);

	CHECK_EQ(Col("date", 3).tds_type_token, duckdb::tds::TDS_TYPE_DATE);
	CHECK(Col("date", 3).duckdb_type.id() == LogicalTypeId::DATE);
	CHECK_EQ(Col("time", 5, 0, 7).tds_type_token, duckdb::tds::TDS_TYPE_TIME);
	CHECK(Col("time", 5, 0, 7).duckdb_type.id() == LogicalTypeId::TIME);
	CHECK_EQ(Col("time", 5, 0, 7).scale, 7);

	// Every datetime flavour is DATETIME2 on the BCP wire with its own scale.
	for (const char *t : {"datetime", "datetime2", "smalldatetime"}) {
		auto c = Col(t, 8, 0, 3);
		CHECK_EQ(c.tds_type_token, duckdb::tds::TDS_TYPE_DATETIME2);
		CHECK(c.duckdb_type.id() == LogicalTypeId::TIMESTAMP);
	}
	auto dto = Col("datetimeoffset", 10, 0, 7);
	CHECK_EQ(dto.tds_type_token, duckdb::tds::TDS_TYPE_DATETIMEOFFSET);
	CHECK(dto.duckdb_type.id() == LogicalTypeId::TIMESTAMP_TZ);

	CHECK_EQ(Col("xml", -1).tds_type_token, duckdb::tds::TDS_TYPE_XML);
}

}  // namespace

int main() {
	TestFixedWidth();
	TestDecimalAndMoney();
	TestStrings();
	TestBinaryAndTemporal();
	if (failures) {
		std::cerr << failures << " failure(s)\n";
		return 1;
	}
	std::cout << "test_bcp_column_metadata: all checks passed\n";
	return 0;
}
