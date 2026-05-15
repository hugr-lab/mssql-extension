//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/type_family.hpp
//
// Defines the TypeFamily enum that partitions all TDS wire types and
// DuckDB LogicalTypeIds into 9 mutually-exclusive groups, each owned by
// one per-family codec module under src/codec/.
//
// Also defines DdlContext (CreateTable vs CtasCreateTable) for the DDL
// type-name mapping that consolidates MapTypeToSQLServer and
// MapLogicalTypeToCTAS in mssql_ddl_translator.cpp.
//
// Used by all 5 dispatch sites:
//   - src/tds/encoding/type_converter.cpp        (FamilyFromTdsType)
//   - src/tds/encoding/bcp_row_encoder.cpp       (FamilyFromLogicalType)
//   - src/table_scan/filter_encoder.cpp          (FamilyFromLogicalType)
//   - src/dml/insert/mssql_value_serializer.cpp  (FamilyFromLogicalType)
//   - src/catalog/mssql_ddl_translator.cpp       (FamilyFromLogicalType)
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"

#include <cstdint>

namespace duckdb {
namespace mssql {
namespace codec {

enum class TypeFamily : uint8_t {
	Boolean,
	Integer,
	Float,
	Decimal,
	Money,
	String,
	Binary,
	DateTime,
	Uuid,
};

// Post-spec-045 both DdlContext values produce byte-identical T-SQL for
// the same (LogicalType, CTASConfig) inputs (FR-024..FR-028). The enum
// is retained for API uniformity across family `FormatDdlTypeName`
// signatures and for future per-context DDL hints (e.g., identity
// columns, partition columns, computed columns). Spec-045 family
// modules MAY ignore the parameter — mark `(void)ctx;` if unused.
enum class DdlContext : uint8_t {
	CreateTable,	  // general DDL via MSSQLDDLTranslator::MapTypeToSQLServer
	CtasCreateTable,  // CTAS DDL via MSSQLDDLTranslator::MapLogicalTypeToCTAS
};

// Maps a TDS wire type id to its TypeFamily. Used by scan decode.
// Throws InvalidInputException for unknown type ids (preserves current
// behavior of type_converter.cpp:ConvertValue default arm).
TypeFamily FamilyFromTdsType(uint8_t tds_type_id);

// Maps a DuckDB LogicalType to its TypeFamily. Used by BCP encode,
// literal format (both Filter and InsertValues contexts), and DDL type
// name.
//
// Throws NotImplementedException for unsupported DuckDB types
// (preserves current default-arm behavior of bcp_row_encoder.cpp,
// mssql_value_serializer.cpp, etc.).
TypeFamily FamilyFromLogicalType(const LogicalType &type);

}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
