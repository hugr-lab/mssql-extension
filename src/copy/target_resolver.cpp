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
#include <unordered_map>

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
	// For temp tables with empty schema, return just the table name
	if (schema_name.empty() && IsTempTable()) {
		return GetBracketedTable();
	}
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

string BCPColumnMetadata::GetSQLServerTypeDeclaration() const {
	switch (tds_type_token) {
	case tds::TDS_TYPE_BITN:
		return "bit";

	case tds::TDS_TYPE_INTN:
		if (max_length == 1) {
			return "tinyint";
		} else if (max_length == 2) {
			return "smallint";
		} else if (max_length == 4) {
			return "int";
		} else {
			return "bigint";
		}

	case tds::TDS_TYPE_FLOATN:
		if (max_length == 4) {
			return "real";
		} else {
			return "float";
		}

	case tds::TDS_TYPE_DECIMAL:
	case tds::TDS_TYPE_NUMERIC:
		return "decimal(" + std::to_string(precision) + ", " + std::to_string(scale) + ")";

	case tds::TDS_TYPE_NVARCHAR:
		if (max_length == 0xFFFF) {
			return "nvarchar(max)";
		} else {
			// max_length is in bytes, nvarchar is 2 bytes per character
			return "nvarchar(" + std::to_string(max_length / 2) + ")";
		}

	case tds::TDS_TYPE_BIGVARBINARY:
		if (max_length == 0xFFFF) {
			return "varbinary(max)";
		} else {
			return "varbinary(" + std::to_string(max_length) + ")";
		}

	case tds::TDS_TYPE_UNIQUEIDENTIFIER:
		return "uniqueidentifier";

	case tds::TDS_TYPE_DATE:
		return "date";

	case tds::TDS_TYPE_TIME:
		return "time(" + std::to_string(scale) + ")";

	case tds::TDS_TYPE_DATETIME2:
		return "datetime2(" + std::to_string(scale) + ")";

	case tds::TDS_TYPE_DATETIMEOFFSET:
		return "datetimeoffset(" + std::to_string(scale) + ")";

	default:
		// Fallback to DuckDB type-based declaration
		return TargetResolver::GetSQLServerTypeDeclaration(duckdb_type);
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
	// URL formats supported:
	// - mssql://<catalog>/<table>           (schema defaults to 'dbo')
	// - mssql://<catalog>/<schema>/<table>  (explicit schema)
	// - mssql://<catalog>/#temp_table       (temp table, no schema)
	// - mssql://<catalog>//#temp_table      (temp table, empty schema - NEW)
	// - mssql://<catalog>//##global_temp    (global temp, empty schema - NEW)

	DebugLog(2, "ResolveURL: parsing '%s'", url.c_str());

	// Check prefix
	if (!StringUtil::StartsWith(url, "mssql://")) {
		throw InvalidInputException("MSSQL COPY: URL must start with 'mssql://', got: %s", url);
	}

	// Remove prefix
	string path = url.substr(8);  // Skip "mssql://"

	// Check for triple slash (invalid)
	if (path.find("///") != string::npos) {
		throw InvalidInputException(
			"MSSQL COPY: Invalid URL format - triple slash not allowed. Expected:\n"
			"  mssql://<catalog>/<table>\n"
			"  mssql://<catalog>/<schema>/<table>\n"
			"  mssql://<catalog>/#temp_table\n"
			"  mssql://<catalog>//#temp_table");
	}

	// Check for empty schema syntax: mssql://catalog//#table
	// This creates ["catalog", "", "#table"] when split
	bool has_empty_schema = path.find("//") != string::npos;

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
		// mssql://<catalog>/<schema>/<table> or mssql://<catalog>//<table> (empty schema)
		target.schema_name = parts[1];	// May be empty for temp tables
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

	// Validate empty schema: only allowed for temp tables
	if (target.schema_name.empty()) {
		if (!target.IsTempTable()) {
			throw InvalidInputException(
				"MSSQL COPY: Empty schema only valid for temp tables (table name must start with '#'). "
				"Got table name: '%s'",
				target.table_name);
		}
		DebugLog(2, "ResolveURL: empty schema accepted for temp table '%s'", target.table_name.c_str());
	}

	DebugLog(1, "ResolveURL: catalog='%s', schema='%s', table='%s', is_temp=%d, is_global_temp=%d",
			 target.catalog_name.c_str(), target.schema_name.c_str(), target.table_name.c_str(), target.is_temp_table,
			 target.is_global_temp);

	return target;
}

//===----------------------------------------------------------------------===//
// TargetResolver::ResolveCatalog
//===----------------------------------------------------------------------===//

BCPCopyTarget TargetResolver::ResolveCatalog(ClientContext &context, const string &catalog, const string &schema,
											 const string &table, bool allow_empty_schema) {
	BCPCopyTarget target;
	target.catalog_name = catalog;
	target.table_name = table;

	// Detect temp table first (needed for empty schema validation)
	target.DetectTempTable();

	// Handle schema: empty schema allowed only for temp tables
	if (schema.empty() && allow_empty_schema && target.IsTempTable()) {
		// Empty schema explicitly requested for temp table - keep it empty
		target.schema_name = "";
		DebugLog(2, "ResolveCatalog: empty schema accepted for temp table '%s'", table.c_str());
	} else if (schema.empty() && allow_empty_schema && !target.IsTempTable()) {
		// Empty schema for non-temp table is an error
		throw InvalidInputException(
			"MSSQL COPY: Empty schema only valid for temp tables (table name must start with '#'). "
			"Got table name: '%s'",
			table);
	} else if (schema.empty()) {
		// Default behavior: empty schema defaults to 'dbo'
		target.schema_name = "dbo";
	} else {
		target.schema_name = schema;
	}

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

	DebugLog(1, "ResolveCatalog: catalog='%s', schema='%s', table='%s', is_temp=%d", target.catalog_name.c_str(),
			 target.schema_name.c_str(), target.table_name.c_str(), target.IsTempTable() ? 1 : 0);

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

	// Build a map of target column names to their type information (case-insensitive)
	std::unordered_map<string, std::pair<string, idx_t>> target_columns;  // name -> (type, index)
	for (idx_t i = 0; i < result.rows.size(); i++) {
		if (result.rows[i].size() >= 2) {
			string col_name_lower = StringUtil::Lower(result.rows[i][0]);
			target_columns[col_name_lower] = {result.rows[i][1], i};
		}
	}

	// Validate each source column exists in target and has compatible type
	idx_t matched_columns = 0;
	for (idx_t i = 0; i < source_names.size(); i++) {
		string source_name_lower = StringUtil::Lower(source_names[i]);
		auto it = target_columns.find(source_name_lower);

		if (it == target_columns.end()) {
			// Source column not found in target - this is OK, we'll just ignore it
			DebugLog(2, "ValidateExistingTableSchema: source column '%s' not in target (will be ignored)",
			         source_names[i].c_str());
			continue;
		}

		matched_columns++;
		const string &target_type_name = it->second.first;

		// Check type compatibility
		bool compatible = IsTypeCompatible(source_types[i], target_type_name);
		if (!compatible) {
			throw InvalidInputException(
				"MSSQL COPY: Column '%s' type mismatch: target expects %s, source provides %s. "
				"Use REPLACE=true to recreate the table with the new schema.",
				source_names[i], StringUtil::Upper(target_type_name), source_types[i].ToString());
		}

		DebugLog(3, "ValidateExistingTableSchema: column '%s' compatible (source: %s, target: %s)",
		         source_names[i].c_str(), source_types[i].ToString().c_str(), target_type_name.c_str());
	}

	// At least one source column must match a target column
	if (matched_columns == 0) {
		throw InvalidInputException(
			"MSSQL COPY: No matching columns between source and target table '%s'. "
			"Source columns: %s. Target table has %llu columns. "
			"Column matching is case-insensitive by name.",
			target.GetFullyQualifiedName(), StringUtil::Join(source_names, ", "),
			(unsigned long long)result.rows.size());
	}

	DebugLog(2, "ValidateExistingTableSchema: validated %llu/%llu source columns match target (target has %llu columns)",
	         (unsigned long long)matched_columns, (unsigned long long)source_names.size(),
	         (unsigned long long)result.rows.size());
}

//===----------------------------------------------------------------------===//
// Helper: Map SQL Server type name to TDS type token
//===----------------------------------------------------------------------===//

static uint8_t SQLServerTypeToTDSToken(const string &type_name) {
	string type_lower = StringUtil::Lower(type_name);

	if (type_lower == "bit") {
		return tds::TDS_TYPE_BITN;
	} else if (type_lower == "tinyint") {
		return tds::TDS_TYPE_INTN;
	} else if (type_lower == "smallint") {
		return tds::TDS_TYPE_INTN;
	} else if (type_lower == "int") {
		return tds::TDS_TYPE_INTN;
	} else if (type_lower == "bigint") {
		return tds::TDS_TYPE_INTN;
	} else if (type_lower == "real") {
		return tds::TDS_TYPE_FLOATN;
	} else if (type_lower == "float") {
		return tds::TDS_TYPE_FLOATN;
	} else if (type_lower == "decimal" || type_lower == "numeric") {
		return tds::TDS_TYPE_DECIMAL;
	} else if (type_lower == "money" || type_lower == "smallmoney") {
		return tds::TDS_TYPE_DECIMAL;
	} else if (type_lower == "varchar" || type_lower == "char" || type_lower == "text") {
		return tds::TDS_TYPE_NVARCHAR;	// Use NVARCHAR for all char types in BCP
	} else if (type_lower == "nvarchar" || type_lower == "nchar" || type_lower == "ntext") {
		return tds::TDS_TYPE_NVARCHAR;
	} else if (type_lower == "varbinary" || type_lower == "binary" || type_lower == "image") {
		return tds::TDS_TYPE_BIGVARBINARY;
	} else if (type_lower == "uniqueidentifier") {
		return tds::TDS_TYPE_UNIQUEIDENTIFIER;
	} else if (type_lower == "date") {
		return tds::TDS_TYPE_DATE;
	} else if (type_lower == "time") {
		return tds::TDS_TYPE_TIME;
	} else if (type_lower == "datetime" || type_lower == "datetime2" || type_lower == "smalldatetime") {
		return tds::TDS_TYPE_DATETIME2;
	} else if (type_lower == "datetimeoffset") {
		return tds::TDS_TYPE_DATETIMEOFFSET;
	}

	// Default to NVARCHAR for unknown types
	return tds::TDS_TYPE_NVARCHAR;
}

//===----------------------------------------------------------------------===//
// Helper: Get max_length for SQL Server type
//===----------------------------------------------------------------------===//

static uint16_t SQLServerTypeMaxLength(const string &type_name, int16_t max_length, uint8_t precision) {
	string type_lower = StringUtil::Lower(type_name);

	if (type_lower == "bit") {
		return 1;
	} else if (type_lower == "tinyint") {
		return 1;
	} else if (type_lower == "smallint") {
		return 2;
	} else if (type_lower == "int") {
		return 4;
	} else if (type_lower == "bigint") {
		return 8;
	} else if (type_lower == "real") {
		return 4;
	} else if (type_lower == "float") {
		return 8;
	} else if (type_lower == "decimal" || type_lower == "numeric") {
		// Calculate based on precision
		if (precision <= 9) {
			return 5;
		} else if (precision <= 19) {
			return 9;
		} else if (precision <= 28) {
			return 13;
		} else {
			return 17;
		}
	} else if (type_lower == "money") {
		return 8;
	} else if (type_lower == "smallmoney") {
		return 4;
	} else if (type_lower == "varchar" || type_lower == "nvarchar" || type_lower == "char" || type_lower == "nchar") {
		// For (max), max_length is -1 in sys.columns
		if (max_length == -1) {
			return 0xFFFF;	// MAX indicator
		}
		// For nvarchar/nchar, max_length is in bytes (2 bytes per char)
		return static_cast<uint16_t>(max_length);
	} else if (type_lower == "text" || type_lower == "ntext") {
		return 0xFFFF;	// MAX indicator
	} else if (type_lower == "varbinary" || type_lower == "binary") {
		if (max_length == -1) {
			return 0xFFFF;	// MAX indicator
		}
		return static_cast<uint16_t>(max_length);
	} else if (type_lower == "image") {
		return 0xFFFF;
	} else if (type_lower == "uniqueidentifier") {
		return 16;
	} else if (type_lower == "date") {
		return 3;
	} else if (type_lower == "time") {
		return 5;
	} else if (type_lower == "datetime2") {
		return 8;
	} else if (type_lower == "datetime") {
		return 8;
	} else if (type_lower == "smalldatetime") {
		return 4;
	} else if (type_lower == "datetimeoffset") {
		return 10;
	}

	// Default
	return 0xFFFF;
}

//===----------------------------------------------------------------------===//
// TargetResolver::GetExistingTableColumnMetadata
//===----------------------------------------------------------------------===//

vector<BCPColumnMetadata> TargetResolver::GetExistingTableColumnMetadata(tds::TdsConnection &conn,
																		 const BCPCopyTarget &target) {
	// Query the target table's column information
	string column_sql;
	if (target.IsTempTable()) {
		column_sql = StringUtil::Format(
			"SELECT c.name AS column_name, t.name AS type_name, c.max_length, c.precision, c.scale, c.is_nullable "
			"FROM tempdb.sys.columns c "
			"JOIN tempdb.sys.types t ON c.user_type_id = t.user_type_id "
			"WHERE c.object_id = OBJECT_ID('tempdb..%s') "
			"ORDER BY c.column_id",
			target.GetBracketedTable());
	} else {
		column_sql = StringUtil::Format(
			"SELECT c.name AS column_name, t.name AS type_name, c.max_length, c.precision, c.scale, c.is_nullable "
			"FROM sys.columns c "
			"JOIN sys.types t ON c.user_type_id = t.user_type_id "
			"WHERE c.object_id = OBJECT_ID('%s') "
			"ORDER BY c.column_id",
			target.GetFullyQualifiedName());
	}

	DebugLog(3, "GetExistingTableColumnMetadata SQL: %s", column_sql.c_str());

	auto result = MSSQLSimpleQuery::Execute(conn, column_sql);
	if (!result.success) {
		throw InvalidInputException("MSSQL COPY: Failed to query table schema: %s", result.error_message);
	}

	vector<BCPColumnMetadata> columns;
	columns.reserve(result.rows.size());

	for (idx_t i = 0; i < result.rows.size(); i++) {
		if (result.rows[i].size() < 6) {
			continue;
		}

		BCPColumnMetadata col;
		col.name = result.rows[i][0];
		const string &type_name = result.rows[i][1];
		int16_t max_length = static_cast<int16_t>(std::stoi(result.rows[i][2]));
		col.precision = static_cast<uint8_t>(std::stoi(result.rows[i][3]));
		col.scale = static_cast<uint8_t>(std::stoi(result.rows[i][4]));
		col.nullable = (result.rows[i][5] == "1" || result.rows[i][5] == "true");

		// Map SQL Server type to TDS type token
		col.tds_type_token = SQLServerTypeToTDSToken(type_name);
		col.max_length = SQLServerTypeMaxLength(type_name, max_length, col.precision);

		// Set a reasonable DuckDB type for encoding purposes
		// This is used by BCPRowEncoder to know how to encode the data
		string type_lower = StringUtil::Lower(type_name);
		if (type_lower == "bit") {
			col.duckdb_type = LogicalType::BOOLEAN;
		} else if (type_lower == "tinyint") {
			col.duckdb_type = LogicalType::TINYINT;
		} else if (type_lower == "smallint") {
			col.duckdb_type = LogicalType::SMALLINT;
		} else if (type_lower == "int") {
			col.duckdb_type = LogicalType::INTEGER;
		} else if (type_lower == "bigint") {
			col.duckdb_type = LogicalType::BIGINT;
		} else if (type_lower == "real") {
			col.duckdb_type = LogicalType::FLOAT;
		} else if (type_lower == "float") {
			col.duckdb_type = LogicalType::DOUBLE;
		} else if (type_lower == "decimal" || type_lower == "numeric") {
			col.duckdb_type = LogicalType::DECIMAL(col.precision, col.scale);
		} else if (type_lower == "money") {
			col.duckdb_type = LogicalType::DECIMAL(19, 4);
		} else if (type_lower == "smallmoney") {
			col.duckdb_type = LogicalType::DECIMAL(10, 4);
		} else if (type_lower == "uniqueidentifier") {
			col.duckdb_type = LogicalType::UUID;
		} else if (type_lower == "date") {
			col.duckdb_type = LogicalType::DATE;
		} else if (type_lower == "time") {
			col.duckdb_type = LogicalType::TIME;
		} else if (type_lower == "datetime" || type_lower == "datetime2" || type_lower == "smalldatetime") {
			col.duckdb_type = LogicalType::TIMESTAMP;
		} else if (type_lower == "datetimeoffset") {
			col.duckdb_type = LogicalType::TIMESTAMP_TZ;
		} else if (type_lower == "varbinary" || type_lower == "binary" || type_lower == "image") {
			col.duckdb_type = LogicalType::BLOB;
		} else {
			// Default to VARCHAR for text types
			col.duckdb_type = LogicalType::VARCHAR;
		}

		DebugLog(3, "GetExistingTableColumnMetadata: column '%s' type=%s tds=0x%02X max_len=%d prec=%d scale=%d",
				 col.name.c_str(), type_name.c_str(), col.tds_type_token, col.max_length, col.precision, col.scale);

		columns.push_back(std::move(col));
	}

	DebugLog(2, "GetExistingTableColumnMetadata: retrieved %llu columns from target table",
			 (unsigned long long)columns.size());

	return columns;
}

//===----------------------------------------------------------------------===//
// TargetResolver::BuildColumnMapping
//===----------------------------------------------------------------------===//

vector<int32_t> TargetResolver::BuildColumnMapping(const vector<string> &source_names,
                                                   const vector<BCPColumnMetadata> &target_columns) {
	vector<int32_t> mapping;
	mapping.reserve(target_columns.size());

	// Build a case-insensitive map of source column names to indices
	std::unordered_map<string, int32_t> source_name_to_idx;
	for (idx_t i = 0; i < source_names.size(); i++) {
		source_name_to_idx[StringUtil::Lower(source_names[i])] = static_cast<int32_t>(i);
	}

	// For each target column, find the matching source column by name
	for (idx_t target_idx = 0; target_idx < target_columns.size(); target_idx++) {
		string target_name_lower = StringUtil::Lower(target_columns[target_idx].name);
		auto it = source_name_to_idx.find(target_name_lower);
		if (it != source_name_to_idx.end()) {
			mapping.push_back(it->second);
			DebugLog(3, "BuildColumnMapping: target[%llu]='%s' -> source[%d]", (unsigned long long)target_idx,
			         target_columns[target_idx].name.c_str(), it->second);
		} else {
			mapping.push_back(-1);  // No source column for this target
			DebugLog(3, "BuildColumnMapping: target[%llu]='%s' -> NULL (no source)", (unsigned long long)target_idx,
			         target_columns[target_idx].name.c_str());
		}
	}

	DebugLog(2, "BuildColumnMapping: mapped %llu source columns to %llu target columns",
	         (unsigned long long)source_names.size(), (unsigned long long)target_columns.size());

	return mapping;
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
