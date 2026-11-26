// Spec 093 T8 (D9): the backup reader's use of the TDS path's type kernels (backup_type_forms.hpp).

#include "backup/backup_type_forms.hpp"

#include "backup/backup_system_tables.hpp"
#include "catalog/mssql_column_info.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

// The CLR system type's name, lower case ("" for any other column).
std::string ClrSystemName(const CatalogColumn &c) {
	if (c.system_type_id != XTYPE_CLR) {
		return std::string();
	}
	const std::string name = StringUtil::Lower(c.system_type_name);
	return name == "hierarchyid" || name == "geometry" || name == "geography" ? name : std::string();
}

}  // namespace

bool IsClrSystemType(const CatalogColumn &c) {
	return !ClrSystemName(c).empty();
}

LogicalType ClrSystemTypeOf(const CatalogColumn &c, const TypeForms &forms) {
	const std::string name = ClrSystemName(c);
	if (name != "hierarchyid" && forms.geometry_as != tds::GeometryAs::Geometry) {
		return LogicalType::BLOB;
	}
	// TYPE mssql's type for the name: hierarchyid VARCHAR, geometry GEOMETRY, geography GEOMETRY('OGC:CRS84').
	return MSSQLColumnInfo::MapSQLServerTypeToDuckDB(name, c.max_length, c.precision, c.scale,
													 MSSQLDatetime2Read::Nanoseconds);
}

void DescribeClrSystemType(const CatalogColumn &c, const TypeForms &forms, tds::ColumnMetadata &m) {
	m.type_id = tds::TDS_TYPE_UDT;
	m.udt_schema = "sys";
	m.udt_type_name = ClrSystemName(c);
	// A UDT travels as PLP: its value, in row or assembled from off row, staged whole for the kernel.
	m.max_length = 0xFFFF;
	m.geometry_as = forms.geometry_as;
}

void DescribeSqlVariant(tds::ColumnMetadata &m) {
	m.type_id = tds::TDS_TYPE_SQL_VARIANT;
	m.max_length = tds::SQL_VARIANT_MAX_BYTES;
	m.variant_disk_form = true;
}

bool IsVectorColumn(const CatalogColumn &c) {
	return c.system_type_id == XTYPE_VARBINARY && c.user_type_id == static_cast<int32_t>(tds::VECTOR_USER_TYPE) &&
		   StringUtil::Lower(c.type_name) == "vector";
}

// The most dimensions of a vector of `element_type`: its values fill at most 8000 - 8 bytes (1998 float32,
// 3996 float16; measured on SQL Server 2025, #644).
static int32_t MaxVectorDimensions(uint8_t element_type) {
	return (8000 - tds::VECTOR_HEADER_BYTES) / tds::VectorElementBytes(element_type);
}

uint16_t VectorDeclaration(const CatalogColumn &c, std::string &why) {
	if (c.scale != tds::VECTOR_ELEMENT_FLOAT32 && c.scale != tds::VECTOR_ELEMENT_FLOAT16) {
		why = "a vector of element type " + std::to_string(c.scale) + ", which no vector has (0 float32, 1 float16)";
		return 0;
	}
	const int32_t element = tds::VectorElementBytes(c.scale);
	const int32_t values = static_cast<int32_t>(c.max_length) - tds::VECTOR_HEADER_BYTES;
	const int32_t dimensions = values > 0 && values % element == 0 ? values / element : 0;
	if (dimensions < 1 || dimensions > MaxVectorDimensions(c.scale)) {
		why = "a vector declared " + std::to_string(c.max_length) + " bytes long, which no " +
			  (c.scale == tds::VECTOR_ELEMENT_FLOAT16 ? "float16" : "float32") + " vector(1.." +
			  std::to_string(MaxVectorDimensions(c.scale)) + ") is";
		return 0;
	}
	return static_cast<uint16_t>(dimensions);
}

void DescribeVector(const CatalogColumn &c, tds::ColumnMetadata &m) {
	m.type_id = tds::TDS_TYPE_VECTOR;
	m.user_type = tds::VECTOR_USER_TYPE;
	m.vector_element_type = c.scale;
	m.max_length = static_cast<uint16_t>(c.max_length);
}

std::string GeometryAsGiven(const std::string &value, const char *source) {
	ParseGeometryAs(value, source);
	return value;
}

TypeForms TypeFormsOf(const std::string &geometry_as, const char *source) {
	TypeForms forms;
	if (!geometry_as.empty()) {
		forms.geometry_as = ParseGeometryAs(geometry_as, source);
	}
	return forms;
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
