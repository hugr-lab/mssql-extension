// Spec 093 T8 (D9): the backup reader's use of the TDS path's type kernels (backup_type_forms.hpp).

#include "backup/backup_type_forms.hpp"

#include "backup/backup_system_tables.hpp"
#include "catalog/mssql_column_info.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"

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

std::string GeometryAsGiven(const std::string &value, const char *source) {
	ParseGeometryAs(value, source);
	return value;
}

std::string Datetime2AsGiven(const std::string &value, const char *source) {
	ParseDatetime2Read(value, source);
	return value;
}

std::string Datetime2AsSetting(ClientContext &context) {
	Value setting;
	if (context.TryGetCurrentSetting("mssql_datetime2_7_as", setting) && !setting.IsNull()) {
		return Datetime2AsGiven(setting.ToString(), "mssql_datetime2_7_as");
	}
	return std::string();
}

TypeFormOptions TypeFormOptionsOf(ClientContext &context, const named_argument_map_t &named,
								  const vector<std::string> &paths, const std::string &function) {
	TypeFormOptions options;
	if (paths.size() == 1) {
		options = AttachedTypeFormOptions(context, paths.front());
	}
	auto geometry_as = named.find("geometry_as");
	if (geometry_as != named.end() && !geometry_as->second.IsNull()) {
		options.geometry_as = GeometryAsGiven(geometry_as->second.ToString(), (function + ": geometry_as").c_str());
	}
	auto datetime2_7_as = named.find("datetime2_7_as");
	if (datetime2_7_as != named.end() && !datetime2_7_as->second.IsNull()) {
		options.datetime2_7_as =
			Datetime2AsGiven(datetime2_7_as->second.ToString(), (function + ": datetime2_7_as").c_str());
	} else if (options.datetime2_7_as.empty()) {
		// Not attached, or attached with neither the option nor the setting then: the setting now.
		options.datetime2_7_as = Datetime2AsSetting(context);
	}
	return options;
}

TypeForms TypeFormsOf(const TypeFormOptions &options, const std::string &where) {
	TypeForms forms;
	if (!options.geometry_as.empty()) {
		forms.geometry_as = ParseGeometryAs(options.geometry_as, (where + ": geometry_as").c_str());
	}
	if (!options.datetime2_7_as.empty()) {
		forms.datetime2_read = ParseDatetime2Read(options.datetime2_7_as, where + ": datetime2_7_as");
	}
	return forms;
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
