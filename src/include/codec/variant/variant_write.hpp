//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/variant/variant_write.hpp
//
// Spec 093 T2 (D3, DQ-43): a DuckDB VARIANT value written into a SQL Server sql_variant. The base type
// comes from the value's VARIANT tag, D3's write table -- the narrowest SQL Server type that holds every
// value of the tag:
//
//   BOOL                         bit
//   INT8 INT16 / UINT8           smallint / tinyint
//   INT32 UINT16 / INT64 UINT32  int / bigint
//   UINT64 INT128 UINT128        decimal(38,0) when it fits, else refused
//   FLOAT DOUBLE                 real float (NaN and the infinities refused: the server holds neither)
//   DECIMAL(w,s)                 decimal(w,s)
//   DATE TIME_MICROS TIME_NANOS  date time(6) time(7)
//   TIMESTAMP_SEC _MILIS _MICROS _NANOS      datetime2(0) (3) (6) (7)
//   TIMESTAMP_MICROS_TZ _NANOS_TZ            datetimeoffset(6) (7), at +00:00
//   UUID                         uniqueidentifier
//   BLOB                         varbinary(n), n <= 8000
//   VARCHAR                      nvarchar(n), n <= 4000 UTF-16 units; else varchar(n) when every
//                                character is ASCII and n <= 8000
//
// Nanoseconds round to 100 ns and a narrowing time rounds as ComputeDatetime2Components does (a
// time(7) that rounds to 24:00 is refused; a datetime2 carries into the next day). Refused by name, the
// column and the tag in the message: OBJECT, ARRAY, BIGNUM, BITSTRING, GEOMETRY, INTERVAL,
// TIME_MICROS_TZ, and every value out of the base type's range (a date or timestamp the server cannot
// hold included).
//
// One form a value, two writers: the statement path's literal (CAST(CAST(<literal> AS <base type>) AS
// sql_variant): the outer cast keeps a multi-row VALUES list from folding its rows to one type by data
// type precedence) and the bulk wire's SSVARIANT envelope (measured to be taken: S2).
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/variant.hpp"
#include "duckdb/common/vector.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace duckdb {
namespace mssql {

struct BCPColumnMetadata;

namespace codec {
namespace variant {

//! The wire bytes a sql_variant's string base values are written with on the bulk path: the database
//! collation `database_collation`'s, from the build's collation table (code_page::WireCollationForName);
//! a name the table lacks (or none) gives Latin1_General_100_BIN2_UTF8's -- mssql_utf8_collation's
//! default, which every target with sql_variant has (2019+, Azure SQL; Fabric has no sql_variant).
//! Such a value then compares and sorts by BIN2_UTF8, not by the database's collation.
std::array<uint8_t, 5> StringCollationBytes(const std::string &database_collation);

//! The statement path's literal of one non-NULL VARIANT value: CAST(CAST(<literal> AS <base type>) AS
//! sql_variant), or NULL for a VARIANT NULL. An InvalidInputException naming `column` for a value
//! sql_variant cannot hold.
std::string FormatLiteral(const Value &value, const std::string &column);

//! The bulk wire's value of row `row` of a VARIANT vector (`data`, its unified form): a u32 length and
//! the SSVARIANT envelope, 0 for NULL (S2). String base values carry `col.collation`. An
//! InvalidInputException naming the column for a value sql_variant cannot hold.
void EncodeToBcp(const UnifiedVariantVectorData &data, idx_t row, const BCPColumnMetadata &col, vector<uint8_t> &out);

//! EncodeToBcp for one Value (the per-value encoder's path).
void EncodeValueToBcp(const Value &value, const BCPColumnMetadata &col, vector<uint8_t> &out);

//! The INSERT BULK / COLMETADATA maximum length of a sql_variant column (LONGLEN): 8000 value bytes
//! and the envelope's header and properties (S2).
constexpr uint32_t SQL_VARIANT_MAX_LENGTH = 8009;

}  // namespace variant
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
