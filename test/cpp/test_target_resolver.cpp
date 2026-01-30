#include "catch.hpp"
#include "copy/target_resolver.hpp"

using namespace duckdb;
using namespace duckdb::mssql;

TEST_CASE("BCPCopyTarget - Basic construction", "[mssql][copy][target_resolver]") {
	SECTION("Default constructor") {
		BCPCopyTarget target;
		REQUIRE(target.catalog_name.empty());
		REQUIRE(target.schema_name.empty());
		REQUIRE(target.table_name.empty());
		REQUIRE_FALSE(target.is_temp_table);
		REQUIRE_FALSE(target.is_global_temp);
		REQUIRE_FALSE(target.IsTempTable());
	}

	SECTION("Constructor with components") {
		BCPCopyTarget target("my_catalog", "dbo", "my_table");
		REQUIRE(target.catalog_name == "my_catalog");
		REQUIRE(target.schema_name == "dbo");
		REQUIRE(target.table_name == "my_table");
		REQUIRE_FALSE(target.is_temp_table);
		REQUIRE_FALSE(target.is_global_temp);
	}
}

TEST_CASE("BCPCopyTarget - Temp table detection", "[mssql][copy][target_resolver]") {
	SECTION("Session-scoped temp table (#)") {
		BCPCopyTarget target("catalog", "dbo", "#temp_table");
		REQUIRE(target.is_temp_table);
		REQUIRE_FALSE(target.is_global_temp);
		REQUIRE(target.IsTempTable());
	}

	SECTION("Global temp table (##)") {
		BCPCopyTarget target("catalog", "dbo", "##global_temp");
		REQUIRE_FALSE(target.is_temp_table);
		REQUIRE(target.is_global_temp);
		REQUIRE(target.IsTempTable());
	}

	SECTION("Regular table - no prefix") {
		BCPCopyTarget target("catalog", "dbo", "regular_table");
		REQUIRE_FALSE(target.is_temp_table);
		REQUIRE_FALSE(target.is_global_temp);
		REQUIRE_FALSE(target.IsTempTable());
	}

	SECTION("DetectTempTable() after setting table_name") {
		BCPCopyTarget target;
		target.table_name = "#staging";
		target.DetectTempTable();
		REQUIRE(target.is_temp_table);
		REQUIRE_FALSE(target.is_global_temp);
	}
}

TEST_CASE("BCPCopyTarget - Name formatting", "[mssql][copy][target_resolver]") {
	SECTION("GetFullyQualifiedName") {
		BCPCopyTarget target("catalog", "dbo", "my_table");
		REQUIRE(target.GetFullyQualifiedName() == "[dbo].[my_table]");
	}

	SECTION("GetBracketedSchema") {
		BCPCopyTarget target("catalog", "custom_schema", "table");
		REQUIRE(target.GetBracketedSchema() == "[custom_schema]");
	}

	SECTION("GetBracketedTable") {
		BCPCopyTarget target("catalog", "dbo", "MyTable");
		REQUIRE(target.GetBracketedTable() == "[MyTable]");
	}

	SECTION("Names with special characters") {
		BCPCopyTarget target("catalog", "dbo", "table with spaces");
		REQUIRE(target.GetBracketedTable() == "[table with spaces]");
		REQUIRE(target.GetFullyQualifiedName() == "[dbo].[table with spaces]");
	}

	SECTION("Temp table formatting") {
		BCPCopyTarget target("catalog", "dbo", "#temp");
		REQUIRE(target.GetBracketedTable() == "[#temp]");
	}
}

TEST_CASE("BCPColumnMetadata - Basic properties", "[mssql][copy][target_resolver]") {
	SECTION("Default constructor") {
		BCPColumnMetadata col;
		REQUIRE(col.name.empty());
		REQUIRE(col.tds_type_token == 0);
		REQUIRE(col.max_length == 0);
		REQUIRE(col.precision == 0);
		REQUIRE(col.scale == 0);
		REQUIRE(col.nullable == true);
	}

	SECTION("Constructor with basic fields") {
		BCPColumnMetadata col("my_col", LogicalType::INTEGER, false);
		REQUIRE(col.name == "my_col");
		REQUIRE(col.nullable == false);
	}
}

TEST_CASE("BCPColumnMetadata - Flags", "[mssql][copy][target_resolver]") {
	SECTION("Nullable column flags") {
		BCPColumnMetadata col;
		col.nullable = true;
		uint16_t flags = col.GetFlags();
		REQUIRE((flags & 0x0001) != 0);  // fNullable bit set
		REQUIRE((flags & 0x0008) != 0);  // usUpdateable bit set
	}

	SECTION("Non-nullable column flags") {
		BCPColumnMetadata col;
		col.nullable = false;
		uint16_t flags = col.GetFlags();
		REQUIRE((flags & 0x0001) == 0);  // fNullable bit not set
		REQUIRE((flags & 0x0008) != 0);  // usUpdateable bit set
	}
}

TEST_CASE("BCPColumnMetadata - Type classification", "[mssql][copy][target_resolver]") {
	SECTION("Variable length USHORT types") {
		BCPColumnMetadata col;

		col.tds_type_token = 0xE7;  // NVARCHARTYPE
		REQUIRE(col.IsVariableLengthUSHORT());

		col.tds_type_token = 0xA5;  // BIGVARBINARYTYPE
		REQUIRE(col.IsVariableLengthUSHORT());

		col.tds_type_token = 0x26;  // INTNTYPE
		REQUIRE_FALSE(col.IsVariableLengthUSHORT());
	}

	SECTION("Fixed length types") {
		BCPColumnMetadata col;

		col.tds_type_token = 0x26;  // INTNTYPE
		REQUIRE(col.IsFixedLength());

		col.tds_type_token = 0x68;  // BITNTYPE
		REQUIRE(col.IsFixedLength());

		col.tds_type_token = 0x6D;  // FLTNTYPE
		REQUIRE(col.IsFixedLength());

		col.tds_type_token = 0x6A;  // DECIMALNTYPE
		REQUIRE(col.IsFixedLength());

		col.tds_type_token = 0x24;  // GUIDTYPE
		REQUIRE(col.IsFixedLength());

		col.tds_type_token = 0x28;  // DATENTYPE
		REQUIRE(col.IsFixedLength());

		col.tds_type_token = 0x29;  // TIMENTYPE
		REQUIRE(col.IsFixedLength());

		col.tds_type_token = 0x2A;  // DATETIME2NTYPE
		REQUIRE(col.IsFixedLength());

		col.tds_type_token = 0x2B;  // DATETIMEOFFSETNTYPE
		REQUIRE(col.IsFixedLength());

		col.tds_type_token = 0xE7;  // NVARCHARTYPE - not fixed
		REQUIRE_FALSE(col.IsFixedLength());
	}

	SECTION("Length prefix size") {
		BCPColumnMetadata col;

		// Variable length USHORT types have 2-byte prefix
		col.tds_type_token = 0xE7;  // NVARCHARTYPE
		REQUIRE(col.GetLengthPrefixSize() == 2);

		col.tds_type_token = 0xA5;  // BIGVARBINARYTYPE
		REQUIRE(col.GetLengthPrefixSize() == 2);

		// Fixed length types have 1-byte prefix (for nullable)
		col.tds_type_token = 0x26;  // INTNTYPE
		REQUIRE(col.GetLengthPrefixSize() == 1);

		col.tds_type_token = 0x6A;  // DECIMALNTYPE
		REQUIRE(col.GetLengthPrefixSize() == 1);
	}
}

TEST_CASE("BCPColumnMetadata - Default collation", "[mssql][copy][target_resolver]") {
	BCPColumnMetadata col;
	// Default collation should be Latin1_General_CI_AS
	REQUIRE(col.collation[0] == 0x09);
	REQUIRE(col.collation[1] == 0x04);
	REQUIRE(col.collation[2] == 0xD0);
	REQUIRE(col.collation[3] == 0x00);
	REQUIRE(col.collation[4] == 0x34);
}
