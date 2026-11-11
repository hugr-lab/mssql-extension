//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_sql_variant.hpp
//
// Spec 085 PR 2b step 6: a sql_variant value as the catalog's base tables store it (sysseobjvalues,
// sysobjvalues: a metadata-only ADD COLUMN's default, a module's flags, a partition boundary): u8 base
// xtype, u8 version (1), the base type's properties, then the value in the form a record stores it
// in. The properties: none (tinyint, smallint, int, bigint, bit, real, float, money, smallmoney,
// date, datetime, smalldatetime, uniqueidentifier), a u8 scale (time, datetime2, datetimeoffset),
// u8 precision and u8 scale (decimal, numeric), a u16 maximum length (binary, varbinary), a u16
// maximum length and a u32 CollationId (char, varchar, nchar, nvarchar). Measured on the catalog
// fixture's defaults (2022): int, varchar, nvarchar, decimal(9, 2), datetime2(3), varbinary.
// DuckDB-free (D15).
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>

namespace duckdb {
namespace mssql {
namespace backup {

struct SqlVariant {
	uint8_t xtype = 0;
	uint8_t precision = 0;
	uint8_t scale = 0;
	uint16_t max_length = 0;
	uint32_t collation = 0;
	std::string value;	// the value's bytes, as a record stores them
};

//! The sql_variant `bytes` (`what` names it in errors): a FormatError for a version other than 1, a
//! base type whose properties the reader does not know, or bytes too short for them.
SqlVariant ParseSqlVariant(const std::string &bytes, const std::string &what);

//! The integer a sql_variant of an integer type holds; a FormatError for another type or size.
int64_t SqlVariantInt(const SqlVariant &v, const std::string &what);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
