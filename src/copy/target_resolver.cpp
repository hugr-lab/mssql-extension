#include "copy/target_resolver.hpp"

#include "catalog/mssql_catalog.hpp"
#include "copy/bcp_config.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "query/mssql_simple_query.hpp"
#include "tds/tds_connection.hpp"
#include "tds/tds_types.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace duckdb {
namespace mssql {

//===----------------------------------------------------------------------===//
// Debug Logging
//===----------------------------------------------------------------------===//

static int GetDebugLevel() {
	const char *env = std::getenv("MSSQL_DEBUG");
	if (!env) {
		return 0;
	}
	return std::atoi(env);
}

static void DebugLog(int level, const char *format, ...) {
	if (GetDebugLevel() < level) {
		return;
	}
	va_list args;
	va_start(args, format);
	fprintf(stderr, "[MSSQL COPY] ");
	vfprintf(stderr, format, args);
	fprintf(stderr, "\n");
	va_end(args);
}

//===----------------------------------------------------------------------===//
// BCPCopyTarget Implementation
//===----------------------------------------------------------------------===//

void BCPCopyTarget::DetectTempTable() {
	if (!table_name.empty()) {
		if (table_name.size() >= 2 && table_name[0] == '#' && table_name[1] == '#') {
			is_global_temp = true;
			is_temp_table = false;
		} else if (table_name[0] == '#') {
			is_temp_table = true;
			is_global_temp = false;
		}
	}
}

string BCPCopyTarget::GetFullyQualifiedName() const {
	return GetBracketedSchema() + "." + GetBracketedTable();
}

string BCPCopyTarget::GetBracketedSchema() const {
	return "[" + schema_name + "]";
}

string BCPCopyTarget::GetBracketedTable() const {
	return "[" + table_name + "]";
}

//===----------------------------------------------------------------------===//
// BCPColumnMetadata Implementation
//===----------------------------------------------------------------------===//

bool BCPColumnMetadata::IsVariableLengthUSHORT() const {
	// NVARCHARTYPE (0xE7) and BIGVARBINARYTYPE (0xA5) use USHORTLEN
	return tds_type_token == 0xE7 || tds_type_token == 0xA5;
}

bool BCPColumnMetadata::IsFixedLength() const {
	// INTNTYPE, BITNTYPE, FLTNTYPE, DECIMALNTYPE, GUIDTYPE, date/time types
	switch (tds_type_token) {
	case 0x26:	// INTNTYPE
	case 0x68:	// BITNTYPE
	case 0x6D:	// FLTNTYPE
	case 0x6A:	// DECIMALNTYPE
	case 0x6C:	// NUMERICNTYPE
	case 0x24:	// GUIDTYPE
	case 0x28:	// DATENTYPE
	case 0x29:	// TIMENTYPE
	case 0x2A:	// DATETIME2NTYPE
	case 0x2B:	// DATETIMEOFFSETNTYPE
		return true;
	default:
		return false;
	}
}

uint8_t BCPColumnMetadata::GetLengthPrefixSize() const {
	if (IsVariableLengthUSHORT()) {
		return 2;  // USHORTLEN
	} else if (IsFixedLength()) {
		return 1;  // BYTELEN for nullable fixed types
	}
	return 0;
}

//===----------------------------------------------------------------------===//
// TargetResolver::ResolveURL
//===----------------------------------------------------------------------===//

BCPCopyTarget TargetResolver::ResolveURL(ClientContext &context, const string &url) {
	// URL format: mssql://<catalog>/<schema>/<table>
	// or: mssql://<catalog>/#temp_table
	// or: mssql://<catalog>/##global_temp_table

	DebugLog(2, "ResolveURL: parsing '%s'", url.c_str());

	// Check prefix
	if (!StringUtil::StartsWith(url, "mssql://")) {
		throw InvalidInputException("MSSQL COPY: URL must start with 'mssql://', got: %s", url);
	}

	// Remove prefix
	string path = url.substr(8);  // Skip "mssql://"

	// Split by '/'
	vector<string> parts = StringUtil::Split(path, '/');

	BCPCopyTarget target;

	if (parts.empty()) {
		throw InvalidInputException(
			"MSSQL COPY: URL must specify at least catalog and table: mssql://<catalog>/<table>");
	}

	// First part is always the catalog name
	target.catalog_name = parts[0];
	if (target.catalog_name.empty()) {
		throw InvalidInputException("MSSQL COPY: Catalog name cannot be empty in URL");
	}

	// Verify catalog exists and is an MSSQL catalog
	try {
		auto &catalog = Catalog::GetCatalog(context, target.catalog_name);
		if (catalog.GetCatalogType() != "mssql") {
			throw InvalidInputException("MSSQL COPY: Catalog '%s' is not an MSSQL catalog (type: %s)",
										target.catalog_name, catalog.GetCatalogType());
		}
	} catch (CatalogException &e) {
		throw InvalidInputException("MSSQL COPY: Catalog '%s' not found. Use ATTACH to connect first.",
									target.catalog_name);
	}

	if (parts.size() == 2) {
		// mssql://<catalog>/<table> - use default schema 'dbo'
		target.schema_name = "dbo";
		target.table_name = parts[1];
	} else if (parts.size() == 3) {
		// mssql://<catalog>/<schema>/<table>
		target.schema_name = parts[1];
		target.table_name = parts[2];
	} else {
		throw InvalidInputException(
			"MSSQL COPY: Invalid URL format. Expected mssql://<catalog>/<table> or mssql://<catalog>/<schema>/<table>");
	}

	if (target.table_name.empty()) {
		throw InvalidInputException("MSSQL COPY: Table name cannot be empty in URL");
	}

	// Detect temp table from name
	target.DetectTempTable();

	DebugLog(1, "ResolveURL: catalog='%s', schema='%s', table='%s', is_temp=%d, is_global_temp=%d",
			 target.catalog_name.c_str(), target.schema_name.c_str(), target.table_name.c_str(), target.is_temp_table,
			 target.is_global_temp);

	return target;
}

//===----------------------------------------------------------------------===//
// TargetResolver::ResolveCatalog
//===----------------------------------------------------------------------===//

BCPCopyTarget TargetResolver::ResolveCatalog(ClientContext &context, const string &catalog, const string &schema,
											 const string &table) {
	BCPCopyTarget target;
	target.catalog_name = catalog;
	target.schema_name = schema.empty() ? "dbo" : schema;
	target.table_name = table;

	// Verify catalog exists and is an MSSQL catalog
	try {
		auto &cat = Catalog::GetCatalog(context, target.catalog_name);
		if (cat.GetCatalogType() != "mssql") {
			throw InvalidInputException("MSSQL COPY: Catalog '%s' is not an MSSQL catalog (type: %s)",
										target.catalog_name, cat.GetCatalogType());
		}
	} catch (CatalogException &e) {
		throw InvalidInputException("MSSQL COPY: Catalog '%s' not found. Use ATTACH to connect first.",
									target.catalog_name);
	}

	target.DetectTempTable();

	DebugLog(1, "ResolveCatalog: catalog='%s', schema='%s', table='%s'", target.catalog_name.c_str(),
			 target.schema_name.c_str(), target.table_name.c_str());

	return target;
}

//===----------------------------------------------------------------------===//
// TargetResolver::ValidateTarget
//===----------------------------------------------------------------------===//

void TargetResolver::ValidateTarget(ClientContext &context, tds::TdsConnection &conn, BCPCopyTarget &target,
									const BCPCopyConfig &config, const vector<LogicalType> &source_types,
									const vector<string> &source_names) {
	DebugLog(2, "ValidateTarget: checking %s", target.GetFullyQualifiedName().c_str());

	// Build object check SQL
	string object_sql;
	if (target.IsTempTable()) {
		// Temp tables are in tempdb
		object_sql = StringUtil::Format(
			"SELECT OBJECT_ID('tempdb..%s') AS obj_id, "
			"OBJECTPROPERTY(OBJECT_ID('tempdb..%s'), 'IsView') AS is_view",
			target.GetBracketedTable(), target.GetBracketedTable());
	} else {
		object_sql = StringUtil::Format(
			"SELECT OBJECT_ID('%s') AS obj_id, "
			"OBJECTPROPERTY(OBJECT_ID('%s'), 'IsView') AS is_view",
			target.GetFullyQualifiedName(), target.GetFullyQualifiedName());
	}

	DebugLog(3, "ValidateTarget SQL: %s", object_sql.c_str());

	// Execute check
	auto result = MSSQLSimpleQuery::Execute(conn, object_sql);
	if (!result.success) {
		throw InvalidInputException("MSSQL COPY: Failed to check target object: %s", result.error_message);
	}

	bool table_exists = false;
	bool is_view = false;

	if (!result.rows.empty() && !result.rows[0].empty()) {
		// Check if OBJECT_ID returned non-NULL
		if (!result.rows[0][0].empty() && result.rows[0][0] != "NULL") {
			table_exists = true;
			// Check if it's a view
			if (result.rows[0].size() > 1 && result.rows[0][1] == "1") {
				is_view = true;
			}
		}
	}

	DebugLog(2, "ValidateTarget: exists=%d, is_view=%d", table_exists, is_view);

	// Handle different scenarios
	DebugLog(1, "ValidateTarget: exists=%d, is_view=%d, config.overwrite=%d, config.create_table=%d", table_exists,
			 is_view, config.overwrite ? 1 : 0, config.create_table ? 1 : 0);

	if (table_exists) {
		if (is_view) {
			throw InvalidInputException("MSSQL COPY: Cannot COPY to a view. Target '%s' is a view.",
										target.GetFullyQualifiedName());
		}

		if (config.overwrite) {
			// Drop and recreate
			DebugLog(1, "ValidateTarget: REPLACE=true, dropping and recreating table");
			DropTable(conn, target);
			CreateTable(conn, target, source_types, source_names);
		} else {
			// Table exists and we'll append - validate schema compatibility
			DebugLog(1, "ValidateTarget: table exists and OVERWRITE=false, validating schema compatibility");
			ValidateExistingTableSchema(conn, target, source_types, source_names);
		}
	} else {
		// Table doesn't exist
		if (config.create_table) {
			DebugLog(1, "ValidateTarget: CREATE_TABLE=true, creating table");
			CreateTable(conn, target, source_types, source_names);
		} else {
			throw InvalidInputException(
				"MSSQL COPY: Target table '%s' does not exist. "
				"Use CREATE_TABLE=true option to create it automatically.",
				target.GetFullyQualifiedName());
		}
	}
}

//===----------------------------------------------------------------------===//
// TargetResolver::CreateTable
//===----------------------------------------------------------------------===//

void TargetResolver::CreateTable(tds::TdsConnection &conn, const BCPCopyTarget &target,
								 const vector<LogicalType> &source_types, const vector<string> &source_names) {
	if (source_types.size() != source_names.size()) {
		throw InvalidInputException("MSSQL COPY: Column types and names count mismatch");
	}

	// Build CREATE TABLE statement
	string sql = "CREATE TABLE " + target.GetFullyQualifiedName() + " (\n";

	for (idx_t i = 0; i < source_types.size(); i++) {
		if (i > 0) {
			sql += ",\n";
		}
		sql += "  [" + source_names[i] + "] " + GetSQLServerTypeDeclaration(source_types[i]) + " NULL";
	}

	sql += "\n)";

	DebugLog(2, "CreateTable SQL: %s", sql.c_str());

	auto result = MSSQLSimpleQuery::Execute(conn, sql);
	if (!result.success) {
		throw InvalidInputException("MSSQL COPY: Failed to create table '%s': %s", target.GetFullyQualifiedName(),
									result.error_message);
	}

	DebugLog(1, "CreateTable: created %s with %llu columns", target.GetFullyQualifiedName().c_str(),
			 (unsigned long long)source_types.size());
}

//===----------------------------------------------------------------------===//
// TargetResolver::DropTable
//===----------------------------------------------------------------------===//

void TargetResolver::DropTable(tds::TdsConnection &conn, const BCPCopyTarget &target) {
	string sql = "DROP TABLE " + target.GetFullyQualifiedName();

	DebugLog(2, "DropTable SQL: %s", sql.c_str());

	auto result = MSSQLSimpleQuery::Execute(conn, sql);
	if (!result.success) {
		throw InvalidInputException("MSSQL COPY: Failed to drop table '%s': %s", target.GetFullyQualifiedName(),
									result.error_message);
	}

	DebugLog(1, "DropTable: dropped %s", target.GetFullyQualifiedName().c_str());
}

//===----------------------------------------------------------------------===//
// IsTypeCompatible - Helper to check type compatibility
//===----------------------------------------------------------------------===//

static bool IsTypeCompatible(const LogicalType &source_type, const string &target_type_name) {
	// Normalize target type name to lowercase for comparison
	string target_lower = StringUtil::Lower(target_type_name);

	switch (source_type.id()) {
	case LogicalTypeId::BOOLEAN:
		return target_lower == "bit";

	case LogicalTypeId::TINYINT:
	case LogicalTypeId::UTINYINT:
		return target_lower == "tinyint" || target_lower == "smallint" || target_lower == "int" ||
			   target_lower == "bigint";

	case LogicalTypeId::SMALLINT:
		return target_lower == "smallint" || target_lower == "int" || target_lower == "bigint";

	case LogicalTypeId::INTEGER:
		return target_lower == "int" || target_lower == "bigint";

	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UBIGINT:
		return target_lower == "bigint";

	case LogicalTypeId::FLOAT:
		return target_lower == "real" || target_lower == "float";

	case LogicalTypeId::DOUBLE:
		return target_lower == "float" || target_lower == "real";

	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::HUGEINT:
		return target_lower == "decimal" || target_lower == "numeric" || target_lower == "money" ||
			   target_lower == "smallmoney";

	case LogicalTypeId::VARCHAR:
		return target_lower == "varchar" || target_lower == "nvarchar" || target_lower == "char" ||
			   target_lower == "nchar" || target_lower == "text" || target_lower == "ntext";

	case LogicalTypeId::BLOB:
		return target_lower == "varbinary" || target_lower == "binary" || target_lower == "image";

	case LogicalTypeId::UUID:
		return target_lower == "uniqueidentifier";

	case LogicalTypeId::DATE:
		return target_lower == "date" || target_lower == "datetime" || target_lower == "datetime2" ||
			   target_lower == "smalldatetime";

	case LogicalTypeId::TIME:
		return target_lower == "time";

	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
		return target_lower == "datetime2" || target_lower == "datetime" || target_lower == "smalldatetime";

	case LogicalTypeId::TIMESTAMP_TZ:
		return target_lower == "datetimeoffset";

	default:
		// For unknown types, allow if types look similar
		return true;
	}
}

//===----------------------------------------------------------------------===//
// TargetResolver::ValidateExistingTableSchema
//===----------------------------------------------------------------------===//

void TargetResolver::ValidateExistingTableSchema(tds::TdsConnection &conn, const BCPCopyTarget &target,
												 const vector<LogicalType> &source_types,
												 const vector<string> &source_names) {
	// Query the target table's column information
	string column_sql;
	if (target.IsTempTable()) {
		column_sql = StringUtil::Format(
			"SELECT c.name AS column_name, t.name AS type_name, c.max_length, c.precision, c.scale "
			"FROM tempdb.sys.columns c "
			"JOIN tempdb.sys.types t ON c.user_type_id = t.user_type_id "
			"WHERE c.object_id = OBJECT_ID('tempdb..%s') "
			"ORDER BY c.column_id",
			target.GetBracketedTable());
	} else {
		column_sql = StringUtil::Format(
			"SELECT c.name AS column_name, t.name AS type_name, c.max_length, c.precision, c.scale "
			"FROM sys.columns c "
			"JOIN sys.types t ON c.user_type_id = t.user_type_id "
			"WHERE c.object_id = OBJECT_ID('%s') "
			"ORDER BY c.column_id",
			target.GetFullyQualifiedName());
	}

	DebugLog(3, "ValidateExistingTableSchema SQL: %s", column_sql.c_str());

	auto result = MSSQLSimpleQuery::Execute(conn, column_sql);
	if (!result.success) {
		throw InvalidInputException("MSSQL COPY: Failed to query table schema: %s", result.error_message);
	}

	// Check column count
	if (result.rows.size() != source_types.size()) {
		throw InvalidInputException(
			"MSSQL COPY: Column count mismatch. Source has %llu columns, target table '%s' has %llu columns. "
			"Use REPLACE=true to recreate the table with the new schema.",
			(unsigned long long)source_types.size(), target.GetFullyQualifiedName(),
			(unsigned long long)result.rows.size());
	}

	// Validate each column
	for (idx_t i = 0; i < source_types.size(); i++) {
		if (result.rows[i].size() < 2) {
			continue;  // Skip if we couldn't get column info
		}

		const string &target_col_name = result.rows[i][0];
		const string &target_type_name = result.rows[i][1];

		// Check column name (case-insensitive)
		if (!StringUtil::CIEquals(source_names[i], target_col_name)) {
			throw InvalidInputException(
				"MSSQL COPY: Column name mismatch at position %llu. Source has '%s', target table has '%s'. "
				"Use REPLACE=true to recreate the table with the new schema.",
				(unsigned long long)(i + 1), source_names[i], target_col_name);
		}

		// Check type compatibility (basic validation)
		// We allow some flexibility in type mapping, so we check for broad compatibility
		bool compatible = IsTypeCompatible(source_types[i], target_type_name);
		if (!compatible) {
			throw InvalidInputException(
				"MSSQL COPY: Type mismatch for column '%s'. Source type '%s' is not compatible with target type '%s'. "
				"Use REPLACE=true to recreate the table with the new schema.",
				source_names[i], source_types[i].ToString(), target_type_name);
		}

		DebugLog(3, "ValidateExistingTableSchema: column %llu '%s' compatible (source: %s, target: %s)",
				 (unsigned long long)i, source_names[i].c_str(), source_types[i].ToString().c_str(),
				 target_type_name.c_str());
	}

	DebugLog(2, "ValidateExistingTableSchema: schema validated successfully");
}

//===----------------------------------------------------------------------===//
// TargetResolver::GenerateColumnMetadata
//===----------------------------------------------------------------------===//

vector<BCPColumnMetadata> TargetResolver::GenerateColumnMetadata(const vector<LogicalType> &source_types,
																 const vector<string> &source_names) {
	vector<BCPColumnMetadata> columns;
	columns.reserve(source_types.size());

	for (idx_t i = 0; i < source_types.size(); i++) {
		BCPColumnMetadata col;
		col.name = source_names[i];
		col.duckdb_type = source_types[i];
		col.nullable = true;  // All columns nullable by default for COPY

		// Map DuckDB type to TDS type
		col.tds_type_token = GetTDSTypeToken(source_types[i]);
		col.max_length = GetTDSMaxLength(source_types[i]);

		// Handle precision/scale for decimal types
		if (source_types[i].id() == LogicalTypeId::DECIMAL) {
			uint8_t width, scale;
			source_types[i].GetDecimalProperties(width, scale);
			col.precision = width;
			col.scale = scale;
			// Calculate max_length based on precision
			if (width <= 9) {
				col.max_length = 5;
			} else if (width <= 19) {
				col.max_length = 9;
			} else if (width <= 28) {
				col.max_length = 13;
			} else {
				col.max_length = 17;
			}
		}

		// Handle scale for time types
		if (source_types[i].id() == LogicalTypeId::TIME || source_types[i].id() == LogicalTypeId::TIMESTAMP ||
			source_types[i].id() == LogicalTypeId::TIMESTAMP_TZ) {
			col.scale = 6;		 // DuckDB uses microsecond precision
			col.max_length = 8;	 // 5 bytes time + 3 bytes date for datetime2
		}

		columns.push_back(std::move(col));
	}

	DebugLog(2, "GenerateColumnMetadata: generated %llu columns", (unsigned long long)columns.size());

	return columns;
}

//===----------------------------------------------------------------------===//
// TargetResolver::GetSQLServerTypeDeclaration
//===----------------------------------------------------------------------===//

string TargetResolver::GetSQLServerTypeDeclaration(const LogicalType &duckdb_type) {
	switch (duckdb_type.id()) {
	case LogicalTypeId::BOOLEAN:
		return "bit";

	case LogicalTypeId::TINYINT:
	case LogicalTypeId::UTINYINT:
		return "tinyint";

	case LogicalTypeId::SMALLINT:
		return "smallint";

	case LogicalTypeId::INTEGER:
		return "int";

	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UBIGINT:  // UBIGINT maps to bigint (may overflow for large values)
		return "bigint";

	case LogicalTypeId::FLOAT:
		return "real";

	case LogicalTypeId::DOUBLE:
		return "float";

	case LogicalTypeId::DECIMAL: {
		uint8_t width, scale;
		duckdb_type.GetDecimalProperties(width, scale);
		return StringUtil::Format("decimal(%d,%d)", width, scale);
	}

	case LogicalTypeId::VARCHAR:
		return "nvarchar(max)";

	case LogicalTypeId::BLOB:
		return "varbinary(max)";

	case LogicalTypeId::UUID:
		return "uniqueidentifier";

	case LogicalTypeId::DATE:
		return "date";

	case LogicalTypeId::TIME:
		return "time(6)";  // DuckDB uses microsecond precision

	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
		return "datetime2(6)";	// Use microsecond precision

	case LogicalTypeId::TIMESTAMP_TZ:
		return "datetimeoffset(6)";

	case LogicalTypeId::HUGEINT:
		return "decimal(38,0)";	 // HUGEINT maps to max precision decimal

	default:
		throw NotImplementedException("MSSQL COPY: Unsupported DuckDB type for SQL Server: %s", duckdb_type.ToString());
	}
}

//===----------------------------------------------------------------------===//
// TargetResolver::GetTDSTypeToken
//===----------------------------------------------------------------------===//

uint8_t TargetResolver::GetTDSTypeToken(const LogicalType &duckdb_type) {
	switch (duckdb_type.id()) {
	case LogicalTypeId::BOOLEAN:
		return tds::TDS_TYPE_BITN;	// 0x68

	case LogicalTypeId::TINYINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
		return tds::TDS_TYPE_INTN;	// 0x26

	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		return tds::TDS_TYPE_FLOATN;  // 0x6D

	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::HUGEINT:
		return tds::TDS_TYPE_DECIMAL;  // 0x6A

	case LogicalTypeId::VARCHAR:
		return tds::TDS_TYPE_NVARCHAR;	// 0xE7

	case LogicalTypeId::BLOB:
		return tds::TDS_TYPE_BIGVARBINARY;	// 0xA5

	case LogicalTypeId::UUID:
		return tds::TDS_TYPE_UNIQUEIDENTIFIER;	// 0x24

	case LogicalTypeId::DATE:
		return tds::TDS_TYPE_DATE;	// 0x28

	case LogicalTypeId::TIME:
		return tds::TDS_TYPE_TIME;	// 0x29

	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
		return tds::TDS_TYPE_DATETIME2;	 // 0x2A

	case LogicalTypeId::TIMESTAMP_TZ:
		return tds::TDS_TYPE_DATETIMEOFFSET;  // 0x2B

	default:
		throw NotImplementedException("MSSQL COPY: Unsupported DuckDB type for TDS: %s", duckdb_type.ToString());
	}
}

//===----------------------------------------------------------------------===//
// TargetResolver::GetTDSMaxLength
//===----------------------------------------------------------------------===//

uint16_t TargetResolver::GetTDSMaxLength(const LogicalType &duckdb_type) {
	switch (duckdb_type.id()) {
	case LogicalTypeId::BOOLEAN:
		return 1;

	case LogicalTypeId::TINYINT:
	case LogicalTypeId::UTINYINT:
		return 1;

	case LogicalTypeId::SMALLINT:
		return 2;

	case LogicalTypeId::INTEGER:
		return 4;

	case LogicalTypeId::BIGINT:
		return 8;

	case LogicalTypeId::FLOAT:
		return 4;

	case LogicalTypeId::DOUBLE:
		return 8;

	case LogicalTypeId::DECIMAL:
		// Will be recalculated based on precision in GenerateColumnMetadata
		return 17;	// Max decimal size

	case LogicalTypeId::VARCHAR:
		return 0xFFFF;	// MAX indicator for nvarchar(max)

	case LogicalTypeId::BLOB:
		return 0xFFFF;	// MAX indicator for varbinary(max)

	case LogicalTypeId::UUID:
		return 16;

	case LogicalTypeId::DATE:
		return 3;

	case LogicalTypeId::TIME:
		return 5;  // Scale 6 = 5 bytes

	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
		return 8;  // 5 bytes time + 3 bytes date

	case LogicalTypeId::TIMESTAMP_TZ:
		return 10;	// 5 bytes time + 3 bytes date + 2 bytes offset

	case LogicalTypeId::HUGEINT:
		return 17;	// Max decimal size for HUGEINT

	default:
		throw NotImplementedException("MSSQL COPY: Unsupported DuckDB type for max_length: %s", duckdb_type.ToString());
	}
}

}  // namespace mssql
}  // namespace duckdb
