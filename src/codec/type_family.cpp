//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/type_family.cpp
//
// Implementations of FamilyFromTdsType and FamilyFromLogicalType.
// Mappings follow data-model.md "Type-family table" and mirror the
// current per-arm dispatch in the 5 dispatch sites.
//===----------------------------------------------------------------------===//

#include "codec/type_family.hpp"

#include "duckdb/common/exception.hpp"
#include "tds/tds_types.hpp"

namespace duckdb {
namespace mssql {
namespace codec {

TypeFamily FamilyFromTdsType(uint8_t tds_type_id) {
	switch (tds_type_id) {
	case tds::TDS_TYPE_BIT:
	case tds::TDS_TYPE_BITN:
		return TypeFamily::Boolean;

	case tds::TDS_TYPE_TINYINT:
	case tds::TDS_TYPE_SMALLINT:
	case tds::TDS_TYPE_INT:
	case tds::TDS_TYPE_BIGINT:
	case tds::TDS_TYPE_INTN:
		return TypeFamily::Integer;

	case tds::TDS_TYPE_REAL:
	case tds::TDS_TYPE_FLOAT:
	case tds::TDS_TYPE_FLOATN:
		return TypeFamily::Float;

	case tds::TDS_TYPE_DECIMAL:
	case tds::TDS_TYPE_NUMERIC:
		return TypeFamily::Decimal;

	case tds::TDS_TYPE_MONEY:
	case tds::TDS_TYPE_SMALLMONEY:
	case tds::TDS_TYPE_MONEYN:
		return TypeFamily::Money;

	case tds::TDS_TYPE_BIGCHAR:
	case tds::TDS_TYPE_BIGVARCHAR:
	case tds::TDS_TYPE_NCHAR:
	case tds::TDS_TYPE_NVARCHAR:
	case tds::TDS_TYPE_XML:
		return TypeFamily::String;

	case tds::TDS_TYPE_BIGBINARY:
	case tds::TDS_TYPE_BIGVARBINARY:
		return TypeFamily::Binary;

	case tds::TDS_TYPE_DATE:
	case tds::TDS_TYPE_TIME:
	case tds::TDS_TYPE_DATETIME:
	case tds::TDS_TYPE_SMALLDATETIME:
	case tds::TDS_TYPE_DATETIME2:
	case tds::TDS_TYPE_DATETIMEN:
	case tds::TDS_TYPE_DATETIMEOFFSET:
		return TypeFamily::DateTime;

	case tds::TDS_TYPE_UNIQUEIDENTIFIER:
		return TypeFamily::Uuid;

	default:
		throw InvalidInputException("Unsupported TDS type id 0x%02X for TypeFamily mapping", tds_type_id);
	}
}

TypeFamily FamilyFromLogicalType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return TypeFamily::Boolean;

	case LogicalTypeId::TINYINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
		// HUGEINT lives in Integer family but forwards to Decimal for
		// BCP encode (precision=38, scale=0) and DDL CreateTable.
		return TypeFamily::Integer;

	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		return TypeFamily::Float;

	case LogicalTypeId::DECIMAL:
		return TypeFamily::Decimal;

	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::INTERVAL:
		// INTERVAL has no scan decode / BCP encode / literal support;
		// it is handled inside String family for DDL only, returning
		// NVARCHAR(100) per current MapTypeToSQLServer behavior.
		return TypeFamily::String;

	case LogicalTypeId::BLOB:
		return TypeFamily::Binary;

	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_TZ:
		return TypeFamily::DateTime;

	case LogicalTypeId::UUID:
		return TypeFamily::Uuid;

	default:
		throw NotImplementedException("Unsupported DuckDB type '%s' for TypeFamily mapping", type.ToString());
	}
}

}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
