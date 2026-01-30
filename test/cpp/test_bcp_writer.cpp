#include "catch.hpp"
#include "copy/bcp_writer.hpp"
#include "copy/target_resolver.hpp"
#include "tds/tds_types.hpp"

using namespace duckdb;
using namespace duckdb::mssql;
using namespace duckdb::tds;

// Helper to create a simple column metadata for testing
static BCPColumnMetadata MakeIntColumn(const string &name) {
	BCPColumnMetadata col;
	col.name = name;
	col.duckdb_type = LogicalType::INTEGER;
	col.tds_type_token = TDS_TYPE_INTN;
	col.max_length = 4;
	col.nullable = true;
	return col;
}

static BCPColumnMetadata MakeVarcharColumn(const string &name) {
	BCPColumnMetadata col;
	col.name = name;
	col.duckdb_type = LogicalType::VARCHAR;
	col.tds_type_token = TDS_TYPE_NVARCHAR;
	col.max_length = 0xFFFF;  // MAX
	col.nullable = true;
	return col;
}

static BCPColumnMetadata MakeBoolColumn(const string &name) {
	BCPColumnMetadata col;
	col.name = name;
	col.duckdb_type = LogicalType::BOOLEAN;
	col.tds_type_token = TDS_TYPE_BITN;
	col.max_length = 1;
	col.nullable = true;
	return col;
}

static BCPColumnMetadata MakeDecimalColumn(const string &name, uint8_t precision, uint8_t scale) {
	BCPColumnMetadata col;
	col.name = name;
	col.duckdb_type = LogicalType::DECIMAL(precision, scale);
	col.tds_type_token = TDS_TYPE_DECIMAL;
	col.precision = precision;
	col.scale = scale;
	// Calculate max_length based on precision
	if (precision <= 9) {
		col.max_length = 5;
	} else if (precision <= 19) {
		col.max_length = 9;
	} else if (precision <= 28) {
		col.max_length = 13;
	} else {
		col.max_length = 17;
	}
	col.nullable = true;
	return col;
}

TEST_CASE("BCPColumnMetadata - TDS type tokens", "[mssql][copy][bcp_writer]") {
	SECTION("Integer type token") {
		auto col = MakeIntColumn("id");
		REQUIRE(col.tds_type_token == 0x26);  // INTNTYPE
	}

	SECTION("Varchar type token") {
		auto col = MakeVarcharColumn("name");
		REQUIRE(col.tds_type_token == 0xE7);  // NVARCHARTYPE
	}

	SECTION("Boolean type token") {
		auto col = MakeBoolColumn("active");
		REQUIRE(col.tds_type_token == 0x68);  // BITNTYPE
	}

	SECTION("Decimal type token") {
		auto col = MakeDecimalColumn("amount", 10, 2);
		REQUIRE(col.tds_type_token == 0x6A);  // DECIMALNTYPE
	}
}

TEST_CASE("BCPColumnMetadata - max_length calculation", "[mssql][copy][bcp_writer]") {
	SECTION("Integer max_length") {
		auto col = MakeIntColumn("id");
		REQUIRE(col.max_length == 4);
	}

	SECTION("Varchar max_length (MAX)") {
		auto col = MakeVarcharColumn("name");
		REQUIRE(col.max_length == 0xFFFF);
	}

	SECTION("Decimal max_length based on precision") {
		// Precision <= 9: 5 bytes
		auto col1 = MakeDecimalColumn("d1", 5, 2);
		REQUIRE(col1.max_length == 5);

		// Precision 10-19: 9 bytes
		auto col2 = MakeDecimalColumn("d2", 15, 2);
		REQUIRE(col2.max_length == 9);

		// Precision 20-28: 13 bytes
		auto col3 = MakeDecimalColumn("d3", 25, 2);
		REQUIRE(col3.max_length == 13);

		// Precision 29-38: 17 bytes
		auto col4 = MakeDecimalColumn("d4", 38, 0);
		REQUIRE(col4.max_length == 17);
	}
}

TEST_CASE("BCPColumnMetadata - Flags generation", "[mssql][copy][bcp_writer]") {
	SECTION("Nullable column") {
		auto col = MakeIntColumn("id");
		col.nullable = true;
		uint16_t flags = col.GetFlags();

		// Check fNullable bit (0x0001)
		REQUIRE((flags & 0x0001) == 0x0001);
		// Check usUpdateable bits (0x0008 = read/write)
		REQUIRE((flags & 0x0008) == 0x0008);
	}

	SECTION("Non-nullable column") {
		auto col = MakeIntColumn("id");
		col.nullable = false;
		uint16_t flags = col.GetFlags();

		// fNullable bit should NOT be set
		REQUIRE((flags & 0x0001) == 0x0000);
		// usUpdateable should still be set
		REQUIRE((flags & 0x0008) == 0x0008);
	}
}

TEST_CASE("BCPColumnMetadata - Type classification for wire format", "[mssql][copy][bcp_writer]") {
	SECTION("NVARCHAR is variable length USHORT") {
		auto col = MakeVarcharColumn("name");
		REQUIRE(col.IsVariableLengthUSHORT() == true);
		REQUIRE(col.IsFixedLength() == false);
		REQUIRE(col.GetLengthPrefixSize() == 2);
	}

	SECTION("INTEGER is fixed length") {
		auto col = MakeIntColumn("id");
		REQUIRE(col.IsVariableLengthUSHORT() == false);
		REQUIRE(col.IsFixedLength() == true);
		REQUIRE(col.GetLengthPrefixSize() == 1);
	}

	SECTION("BOOLEAN is fixed length") {
		auto col = MakeBoolColumn("active");
		REQUIRE(col.IsVariableLengthUSHORT() == false);
		REQUIRE(col.IsFixedLength() == true);
		REQUIRE(col.GetLengthPrefixSize() == 1);
	}

	SECTION("DECIMAL is fixed length") {
		auto col = MakeDecimalColumn("amount", 10, 2);
		REQUIRE(col.IsVariableLengthUSHORT() == false);
		REQUIRE(col.IsFixedLength() == true);
		REQUIRE(col.GetLengthPrefixSize() == 1);
	}
}

TEST_CASE("TDS token constants", "[mssql][copy][bcp_writer]") {
	// Verify TDS type constants are correct
	SECTION("Type tokens") {
		REQUIRE(TDS_TYPE_INTN == 0x26);
		REQUIRE(TDS_TYPE_BITN == 0x68);
		REQUIRE(TDS_TYPE_FLOATN == 0x6D);
		REQUIRE(TDS_TYPE_DECIMAL == 0x6A);
		REQUIRE(TDS_TYPE_NUMERIC == 0x6C);
		REQUIRE(TDS_TYPE_NVARCHAR == 0xE7);
		REQUIRE(TDS_TYPE_BIGVARBINARY == 0xA5);
		REQUIRE(TDS_TYPE_UNIQUEIDENTIFIER == 0x24);
		REQUIRE(TDS_TYPE_DATE == 0x28);
		REQUIRE(TDS_TYPE_TIME == 0x29);
		REQUIRE(TDS_TYPE_DATETIME2 == 0x2A);
		REQUIRE(TDS_TYPE_DATETIMEOFFSET == 0x2B);
	}
}

TEST_CASE("Column metadata for multiple columns", "[mssql][copy][bcp_writer]") {
	vector<BCPColumnMetadata> columns;
	columns.push_back(MakeIntColumn("id"));
	columns.push_back(MakeVarcharColumn("name"));
	columns.push_back(MakeBoolColumn("active"));
	columns.push_back(MakeDecimalColumn("amount", 18, 2));

	REQUIRE(columns.size() == 4);

	SECTION("First column is integer") {
		REQUIRE(columns[0].name == "id");
		REQUIRE(columns[0].tds_type_token == TDS_TYPE_INTN);
	}

	SECTION("Second column is varchar") {
		REQUIRE(columns[1].name == "name");
		REQUIRE(columns[1].tds_type_token == TDS_TYPE_NVARCHAR);
	}

	SECTION("Third column is boolean") {
		REQUIRE(columns[2].name == "active");
		REQUIRE(columns[2].tds_type_token == TDS_TYPE_BITN);
	}

	SECTION("Fourth column is decimal") {
		REQUIRE(columns[3].name == "amount");
		REQUIRE(columns[3].tds_type_token == TDS_TYPE_DECIMAL);
		REQUIRE(columns[3].precision == 18);
		REQUIRE(columns[3].scale == 2);
	}
}
