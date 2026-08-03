#include "copy/target_resolver.hpp"

#include "catalog/mssql_catalog.hpp"
#include "codec/target_string_type.hpp"
#include "copy/bcp_config.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/extension_type_info.hpp"
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
// UTF-8 on the write wire (spec 060 / issue #225)
//===----------------------------------------------------------------------===//

//! Latin1_General_100_CI_AS_SC_UTF8, as the server itself sends it in
//! COLMETADATA: LCID 0x0409, flags 0x4D (fIgnoreCase | fIgnoreWidth |
//! fIgnoreKana | fUTF8), version 2, SortId 0. Read off the wire rather than
//! derived from a table — COLLATIONPROPERTY's CollationID is a different
//! namespace and Fabric closes the connection on it.
//!
//! A CONSTANT is correct here, and the existing code is the proof: every string
//! column has always been declared SQL_Latin1_General_CP1_CI_AS whatever the
//! target's own collation was, and loads into differently-collated columns work.
//! The declared collation tells the server how to READ the bytes; it then
//! converts into the column's own. Code page 65001 reads any UTF-8, so one
//! constant covers every UTF-8 target.
static const std::array<uint8_t, 5> UTF8_WIRE_COLLATION = {0x09, 0x04, 0xD0, 0x24, 0x00};

//! Past this many bytes a varchar column has to be MAX, and travels as PLP.
static constexpr int32_t MAX_INLINE_VARCHAR_BYTES = 8000;

//! Does this target column hold UTF-8 bytes? Only a char type can, and only
//! under a collation whose name ends in _UTF8 — the same test #225 uses on the
//! read side, and the reason it is a name test is that Fabric refuses
//! COLLATIONPROPERTY.
static bool IsUtf8CharColumn(const string &type_name, const string &collation_name) {
	const string lower = StringUtil::Lower(type_name);
	if (lower != "varchar" && lower != "char") {
		return false;
	}
	return StringUtil::EndsWith(StringUtil::Upper(collation_name), "_UTF8");
}

//===----------------------------------------------------------------------===//
// BCPColumnMetadata Implementation
//===----------------------------------------------------------------------===//

bool BCPColumnMetadata::IsVariableLengthUSHORT() const {
	// NVARCHARTYPE (0xE7), BIGVARCHRTYPE (0xA7) and BIGVARBINARYTYPE (0xA5) use USHORTLEN
	return tds_type_token == 0xE7 || tds_type_token == 0xA7 || tds_type_token == 0xA5;
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

	case tds::TDS_TYPE_BIGVARCHAR: {
		// A UTF-8 target: max_length is BYTES and the payload is UTF-8, so the
		// number goes through unchanged rather than halved. The COLLATE clause
		// is what makes the server read the payload as UTF-8 — see the note on
		// BCPColumnMetadata::collation_name.
		string decl = max_length == 0xFFFF ? "varchar(max)" : "varchar(" + std::to_string(max_length) + ")";
		if (!collation_name.empty()) {
			decl += " COLLATE " + collation_name;
		}
		return decl;
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

	case tds::TDS_TYPE_XML:
		// SQL Server rejects XML type in INSERT BULK.
		// Send as nvarchar(max) — auto-converts to XML on the target column.
		// No length limitation: nvarchar(max) supports up to 2 GB, same as XML.
		return "nvarchar(max)";

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
// ResolveVarcharCollation
//===----------------------------------------------------------------------===//

//! Issue #225 / spec 060: a varchar column round-trips non-ASCII only if its
//! collation is a UTF-8 one. The rule lives on MSSQLCatalog so this path cannot
//! drift from CREATE TABLE and CTAS; all this does is answer whether any column
//! asked for a single-byte column without naming a collation itself. Resolved
//! once per COPY, not per column.
static string ResolveVarcharCollation(ClientContext &context, const BCPCopyTarget &target, const BCPCopyConfig &config,
									  const vector<LogicalType> &source_types) {
	// Two ways a single-byte column gets created here, and BOTH need a collation:
	// a column that asked for MSSQL_VARCHAR(n) without naming one, and the
	// session setting that turns EVERY unannotated VARCHAR into one. Missing the
	// second is how 'Привет' reached a CP1252 database as '??????' through COPY
	// while CTAS handled it correctly — issue #225's trap, one path at a time.
	bool wants_varchar = config.text_type_varchar;
	if (!wants_varchar) {
		for (const auto &type : source_types) {
			if (codec::NeedsVarcharCollation(type)) {
				wants_varchar = true;
				break;
			}
		}
	}
	if (!wants_varchar) {
		return string();
	}
	auto &catalog = Catalog::GetCatalog(context, target.catalog_name).Cast<MSSQLCatalog>();
	return catalog.ResolveVarcharCollation(context, true, target.IsTempTable());
}

//===----------------------------------------------------------------------===//
// TargetResolver::ValidateTarget
//===----------------------------------------------------------------------===//

// The shape a table we are about to CREATE will have. Spec 060 lets CTAS/COPY
// build a clustered columnstore or a clustered rowstore index instead of a heap,
// and TABLOCK's answer differs across all three — so the created shape has to be
// carried, not assumed to be a heap the way the issue-#45 policy did.
static MSSQLIndexKind ShapeOfCreatedTable(const MSSQLTableOptions &options) {
	switch (options.kind) {
	case MSSQLTableKind::COLUMNSTORE:
		return MSSQLIndexKind::CLUSTERED_COLUMNSTORE;
	case MSSQLTableKind::CLUSTERED:
		return MSSQLIndexKind::CLUSTERED;
	default:
		return MSSQLIndexKind::HEAP;
	}
}

void TargetResolver::ValidateTarget(ClientContext &context, tds::TdsConnection &conn, BCPCopyTarget &target,
									BCPCopyConfig &config, const vector<LogicalType> &source_types,
									const vector<string> &source_names) {
	DebugLog(2, "ValidateTarget: checking %s", target.GetFullyQualifiedName().c_str());

	// Build object check SQL.
	//
	// The third column is the target's physical shape, and it rides on the check
	// that already had to run — sys.indexes row index_id 0 (heap) or 1 (the
	// clustered index, rowstore or columnstore), which is what decides TABLOCK
	// under 'auto' (spec 057 step 1). No extra round trip, the same trick spec 049
	// used for the catalog's index_kind.
	string object_sql;
	if (target.IsTempTable()) {
		// Temp tables are in tempdb, and so are their indexes.
		object_sql = StringUtil::Format(
			"SELECT OBJECT_ID('tempdb..%s') AS obj_id, "
			"OBJECTPROPERTY(OBJECT_ID('tempdb..%s'), 'IsView') AS is_view, "
			"(SELECT TOP 1 i.type FROM tempdb.sys.indexes i "
			" WHERE i.object_id = OBJECT_ID('tempdb..%s') AND i.index_id <= 1) AS index_type",
			target.GetBracketedTable(), target.GetBracketedTable(), target.GetBracketedTable());
	} else {
		object_sql = StringUtil::Format(
			"SELECT OBJECT_ID('%s') AS obj_id, "
			"OBJECTPROPERTY(OBJECT_ID('%s'), 'IsView') AS is_view, "
			"(SELECT TOP 1 i.type FROM sys.indexes i "
			" WHERE i.object_id = OBJECT_ID('%s') AND i.index_id <= 1) AS index_type",
			target.GetFullyQualifiedName(), target.GetFullyQualifiedName(), target.GetFullyQualifiedName());
	}

	DebugLog(3, "ValidateTarget SQL: %s", object_sql.c_str());

	// Execute check
	auto result = MSSQLSimpleQuery::Execute(conn, object_sql);
	if (!result.success) {
		throw InvalidInputException("MSSQL COPY: Failed to check target object: %s", result.error_message);
	}

	bool table_exists = false;
	bool is_view = false;
	MSSQLIndexKind existing_shape = MSSQLIndexKind::HEAP;

	if (!result.rows.empty() && !result.rows[0].empty()) {
		// Check if OBJECT_ID returned non-NULL
		if (!result.rows[0][0].empty() && result.rows[0][0] != "NULL") {
			table_exists = true;
			// Check if it's a view
			if (result.rows[0].size() > 1 && result.rows[0][1] == "1") {
				is_view = true;
			}
			if (result.rows[0].size() > 2) {
				// A NULL / unparseable type maps to HEAP, which is the conservative
				// answer for the write path — see MSSQLIndexKindFromSysIndexesType.
				existing_shape = MSSQLIndexKindFromSysIndexesType(result.rows[0][2]);
			}
		}
	}

	DebugLog(2, "ValidateTarget: exists=%d, is_view=%d", table_exists, is_view);

	// Handle different scenarios
	DebugLog(1, "ValidateTarget: exists=%d, is_view=%d, config.overwrite=%d, config.create_table=%d", table_exists,
			 is_view, config.overwrite ? 1 : 0, config.create_table ? 1 : 0);

	// Track whether we're creating a new table for auto-TABLOCK (Issue #45)
	config.is_new_table = false;

	// Spec 060: refuse a string type this server cannot store before any DDL is
	// generated, then resolve the collation once per statement, not per column.
	{
		auto &target_catalog = Catalog::GetCatalog(context, target.catalog_name).Cast<MSSQLCatalog>();
		target_catalog.ValidateStringTargets(source_types);
		target_catalog.ValidateTableOptions(config.table_options);
	}
	config.varchar_collation = ResolveVarcharCollation(context, target, config, source_types);
	if (config.text_type_varchar || !config.varchar_collation.empty()) {
		config.wire_varchar_collation = Catalog::GetCatalog(context, target.catalog_name)
											.Cast<MSSQLCatalog>()
											.WireVarcharCollation(config.varchar_collation);
	}

	if (table_exists) {
		if (is_view) {
			throw InvalidInputException("MSSQL COPY: Cannot COPY to a view. Target '%s' is a view.",
										target.GetFullyQualifiedName());
		}

		if (config.overwrite) {
			// Drop and recreate
			DebugLog(1, "ValidateTarget: REPLACE=true, dropping and recreating table");
			DropTable(conn, target);
			CreateTable(conn, target, source_types, source_names, config.varchar_collation, config.table_options,
						config.text_type_varchar);
			// After drop+create, this is effectively a new table
			config.is_new_table = true;
			config.target_shape = ShapeOfCreatedTable(config.table_options);
		} else {
			// Table exists and we'll append - validate schema compatibility
			DebugLog(1, "ValidateTarget: table exists and OVERWRITE=false, validating schema compatibility");
			ValidateExistingTableSchema(conn, target, source_types, source_names);

			// Spec 060 D7: empty it first if asked, keeping its definition. On THIS
			// connection, which is the one the load runs on: routing it elsewhere
			// would let an aborted COPY leave the table both empty and unloaded,
			// and inside a DuckDB transaction the load's own rows would not see
			// the truncate.
			if (config.truncate) {
				const string truncate_sql = "TRUNCATE TABLE " + target.GetFullyQualifiedName();
				DebugLog(1, "ValidateTarget: TRUNCATE=true, emptying %s", target.GetFullyQualifiedName().c_str());
				auto truncate_result = MSSQLSimpleQuery::Execute(conn, truncate_sql);
				if (!truncate_result.success) {
					throw InvalidInputException(
						"MSSQL COPY: Failed to truncate '%s': %s. TRUNCATE needs ALTER on the table and is refused "
						"when a foreign key references it — use replace to recreate the table instead.",
						target.GetFullyQualifiedName(), truncate_result.error_message);
				}
			}

			// Appending to existing table, not a new table
			config.is_new_table = false;
			config.target_shape = existing_shape;
		}
	} else {
		// Table doesn't exist
		if (config.create_table) {
			DebugLog(1, "ValidateTarget: CREATE_TABLE=true, creating table");
			CreateTable(conn, target, source_types, source_names, config.varchar_collation, config.table_options,
						config.text_type_varchar);
			// Newly created table
			config.is_new_table = true;
			config.target_shape = ShapeOfCreatedTable(config.table_options);
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
								 const vector<LogicalType> &source_types, const vector<string> &source_names,
								 const string &varchar_collation, const MSSQLTableOptions &table_options,
								 bool single_byte_text) {
	if (source_types.size() != source_names.size()) {
		throw InvalidInputException("MSSQL COPY: Column types and names count mismatch");
	}

	// Build CREATE TABLE statement
	string sql = "CREATE TABLE " + target.GetFullyQualifiedName() + " (\n";

	for (idx_t i = 0; i < source_types.size(); i++) {
		if (i > 0) {
			sql += ",\n";
		}
		sql += "  [" + source_names[i] + "] " +
			   GetSQLServerTypeDeclaration(source_types[i], varchar_collation, single_byte_text) + " NULL";
	}

	sql += "\n)";
	sql += table_options.CreateTableSuffix();

	DebugLog(2, "CreateTable SQL: %s", sql.c_str());

	auto result = MSSQLSimpleQuery::Execute(conn, sql);
	if (!result.success) {
		throw InvalidInputException("MSSQL COPY: Failed to create table '%s': %s", target.GetFullyQualifiedName(),
									result.error_message);
	}

	// Spec 060 D5: a clustered index of either kind is its own DDL statement. It
	// goes in BEFORE the load on purpose — a clustered columnstore built after
	// the rows are in place costs a full rebuild, and building it first is what
	// lets the load write compressed rowgroups directly.
	const string post_create = table_options.PostCreateStatement(target.schema_name, target.table_name);
	if (!post_create.empty()) {
		DebugLog(2, "CreateTable post-create SQL: %s", post_create.c_str());
		auto index_result = MSSQLSimpleQuery::Execute(conn, post_create);
		if (!index_result.success) {
			throw InvalidInputException("MSSQL COPY: Failed to create index on '%s': %s",
										target.GetFullyQualifiedName(), index_result.error_message);
		}
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
		return target_lower == "smallint" || target_lower == "int" || target_lower == "bigint" ||
			   target_lower == "tinyint";

	case LogicalTypeId::INTEGER:
		return target_lower == "int" || target_lower == "bigint" || target_lower == "smallint" ||
			   target_lower == "tinyint";

	case LogicalTypeId::BIGINT:
		// Issue #153. Narrowing is allowed because the encoder performs it with a
		// range check and refuses the row that does not fit — the same answer SQL
		// Server gives, arrived at before the batch is sent rather than by
		// aborting it mid-stream. DuckDB defaults integer arithmetic to BIGINT, so
		// refusing this made appending to an `int` column need REPLACE=true and a
		// schema change the user did not want.
		return target_lower == "bigint" || target_lower == "int" || target_lower == "smallint" ||
			   target_lower == "tinyint";

	case LogicalTypeId::UBIGINT:
		// Stays exact: UBIGINT travels as DECIMAL(20,0) on the wire, not as an
		// integer, so the narrowing arms above do not apply to it.
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
			   target_lower == "nchar" || target_lower == "text" || target_lower == "ntext" || target_lower == "xml";

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
			"JOIN tempdb.sys.types t ON c.system_type_id = t.user_type_id AND t.system_type_id = t.user_type_id "
			"WHERE c.object_id = OBJECT_ID('tempdb..%s') "
			"ORDER BY c.column_id",
			target.GetBracketedTable());
	} else {
		column_sql = StringUtil::Format(
			"SELECT c.name AS column_name, t.name AS type_name, c.max_length, c.precision, c.scale "
			"FROM sys.columns c "
			"JOIN sys.types t ON c.system_type_id = t.user_type_id AND t.system_type_id = t.user_type_id "
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

	DebugLog(2,
			 "ValidateExistingTableSchema: validated %llu/%llu source columns match target (target has %llu columns)",
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
	} else if (type_lower == "xml") {
		return tds::TDS_TYPE_XML;
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
	} else if (type_lower == "varchar" || type_lower == "char") {
		// For (max), max_length is -1 in sys.columns
		if (max_length == -1) {
			return 0xFFFF;	// MAX indicator
		}
		// varchar/char data is transmitted as NVARCHAR over the BCP wire, so the
		// declared length must be UTF-16 bytes: sys.columns reports single-byte
		// characters, hence 2x. varchar(n) with n > 4000 exceeds the 8000-byte
		// inline NVARCHAR limit and must be sent as nvarchar(max) (PLP).
		if (max_length > 4000) {
			return 0xFFFF;	// MAX indicator (PLP)
		}
		return static_cast<uint16_t>(max_length * 2);
	} else if (type_lower == "nvarchar" || type_lower == "nchar") {
		// For (max), max_length is -1 in sys.columns
		if (max_length == -1) {
			return 0xFFFF;	// MAX indicator
		}
		// For nvarchar/nchar, sys.columns max_length is already in bytes (2 bytes per char)
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
	} else if (type_lower == "xml") {
		return 0xFFFF;	// PLP indicator
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
			"SELECT c.name AS column_name, t.name AS type_name, c.max_length, c.precision, c.scale, c.is_nullable, "
			"ISNULL(c.collation_name, '') AS collation_name "
			"FROM tempdb.sys.columns c "
			"JOIN tempdb.sys.types t ON c.system_type_id = t.user_type_id AND t.system_type_id = t.user_type_id "
			"WHERE c.object_id = OBJECT_ID('tempdb..%s') "
			"ORDER BY c.column_id",
			target.GetBracketedTable());
	} else {
		column_sql = StringUtil::Format(
			"SELECT c.name AS column_name, t.name AS type_name, c.max_length, c.precision, c.scale, c.is_nullable, "
			"ISNULL(c.collation_name, '') AS collation_name "
			"FROM sys.columns c "
			"JOIN sys.types t ON c.system_type_id = t.user_type_id AND t.system_type_id = t.user_type_id "
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

		// Spec 060 / issue #225 (write side): a char column under a UTF-8
		// collation takes the bytes we already hold. Declaring BIGVARCHAR with a
		// UTF-8 collation sends them as they are, instead of transcoding to
		// UTF-16 here for the server to transcode back. max_length goes through
		// unhalved because both sides now count bytes.
		const string &collation_name = result.rows[i].size() > 6 ? result.rows[i][6] : string();
		if (IsUtf8CharColumn(type_name, collation_name)) {
			col.tds_type_token = tds::TDS_TYPE_BIGVARCHAR;
			col.max_length = max_length < 0 ? 0xFFFF : static_cast<uint16_t>(max_length);
			col.collation = UTF8_WIRE_COLLATION;
			col.collation_name = collation_name;
		}

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
			mapping.push_back(-1);	// No source column for this target
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
																 const vector<string> &source_names,
																 const string &varchar_collation,
																 bool single_byte_text) {
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

		// Spec 060 / issue #225 (write side): the same UTF-8 retarget the
		// existing-table path does, for the paths that build metadata from
		// DuckDB types instead — CTAS always, and COPY when it replaces the
		// table. Without this CTAS declared nvarchar(50) on the wire for a
		// column it had just created as varchar(50), so every value was
		// transcoded to UTF-16 for the server to transcode back — the whole of
		// the string path on Fabric, where nvarchar does not exist.
		{
			codec::TargetStringType spec;
			const bool annotated_varchar = codec::TryGetTargetStringType(source_types[i], spec) && !spec.unicode;
			const bool plain_varchar_target = single_byte_text && source_types[i].id() == LogicalTypeId::VARCHAR;
			if (annotated_varchar || plain_varchar_target) {
				// Empty means we cannot name a UTF-8 collation for this target, so
				// the column stays on the transcoding path — which is the right
				// answer for a code-page target, where sending UTF-8 bytes would
				// be read in that code page and mangled.
				const string &collation = spec.collation.empty() ? varchar_collation : spec.collation;
				if (!collation.empty()) {
					col.tds_type_token = tds::TDS_TYPE_BIGVARCHAR;
					// Both sides count bytes now, so the length is not doubled the
					// way an nvarchar declaration doubles it. An unannotated column
					// stated no length and is varchar(max) — the PLP sentinel.
					const bool is_max = !annotated_varchar || spec.length > MAX_INLINE_VARCHAR_BYTES;
					col.max_length = is_max ? 0xFFFF : static_cast<uint16_t>(spec.length);
					col.collation = UTF8_WIRE_COLLATION;
					col.collation_name = collation;
				}
			}
		}

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

		// Handle UBIGINT as DECIMAL(20,0)
		if (source_types[i].id() == LogicalTypeId::UBIGINT) {
			col.precision = 20;
			col.scale = 0;
			col.max_length = 9;	 // DECIMAL(20,0) needs 9 bytes
		}

		// Handle HUGEINT as DECIMAL(38,0) — #177 (SUM() aggregates return HUGEINT).
		// COLMETADATA precision must match the codec's EncodeDecimal byte size.
		if (source_types[i].id() == LogicalTypeId::HUGEINT) {
			col.precision = 38;
			col.scale = 0;
			col.max_length = 17;  // DECIMAL(38,0) needs 17 bytes
		}

		// Handle scale for time types. Scale MUST match the source's native
		// precision so the BCP wire COLMETADATA agrees with how the codec
		// encodes the value (see codec::datetime::ComputeDatetime2Components +
		// EncodeDatetime2Raw). Mismatch silently truncates on the server (e.g.
		// declaring scale=0 for a TIMESTAMP_MS value drops every millisecond).
		switch (source_types[i].id()) {
		case LogicalTypeId::TIME:
			col.scale = 6;		 // µs (DuckDB native)
			col.max_length = 5;	 // time(6) alone — no date part, unlike DATETIME2
			break;
		case LogicalTypeId::TIMESTAMP:
			col.scale = 6;		 // µs (DuckDB native)
			col.max_length = 8;	 // 5 bytes time(6) + 3 bytes date
			break;
		case LogicalTypeId::TIMESTAMP_MS:
			col.scale = 3;		 // ms
			col.max_length = 7;	 // 4 bytes time(3) + 3 bytes date
			break;
		case LogicalTypeId::TIMESTAMP_NS:
			col.scale = 7;		 // 100 ns (DATETIME2 max precision)
			col.max_length = 8;	 // 5 bytes time(7) + 3 bytes date
			break;
		case LogicalTypeId::TIMESTAMP_SEC:
			col.scale = 0;		 // s
			col.max_length = 6;	 // 3 bytes time(0) + 3 bytes date
			break;
		case LogicalTypeId::TIMESTAMP_TZ:
			col.scale = 7;		  // 100 ns — matches DATETIMEOFFSET(7) DDL emitted by codec
			col.max_length = 10;  // 5 bytes time(7) + 3 bytes date + 2 bytes offset
			break;
		default:
			break;
		}

		columns.push_back(std::move(col));
	}

	DebugLog(2, "GenerateColumnMetadata: generated %llu columns", (unsigned long long)columns.size());

	return columns;
}

//===----------------------------------------------------------------------===//
// TargetResolver::GetSQLServerTypeDeclaration
//===----------------------------------------------------------------------===//

string TargetResolver::GetSQLServerTypeDeclaration(const LogicalType &duckdb_type, const string &varchar_collation,
												   bool single_byte_text) {
	// Spec 060: an MSSQL_VARCHAR / MSSQL_NVARCHAR annotation states the target
	// column's type, whether it came from an explicit cast or was carried here by
	// the catalog from an attached source. The bound type is a plain DuckDB type
	// with the size in extension info, so nothing downstream of here changes —
	// only the DDL text.
	codec::TargetStringType target_string;
	if (codec::TryGetTargetStringType(duckdb_type, target_string)) {
		return codec::FormatTargetStringDdl(target_string, varchar_collation);
	}

	switch (duckdb_type.id()) {
	case LogicalTypeId::BOOLEAN:
		return "bit";

	case LogicalTypeId::TINYINT:
	case LogicalTypeId::UTINYINT:
		return "tinyint";

	case LogicalTypeId::SMALLINT:
		return "smallint";

	case LogicalTypeId::USMALLINT:
		return "int";  // USMALLINT (0-65535) needs int (4 bytes)

	case LogicalTypeId::INTEGER:
		return "int";

	case LogicalTypeId::UINTEGER:
		return "bigint";  // UINTEGER (0-4B) needs bigint (8 bytes)

	case LogicalTypeId::BIGINT:
		return "bigint";

	case LogicalTypeId::UBIGINT:
		return "decimal(20,0)";	 // UBIGINT (0-18e18) needs DECIMAL(20,0) for full range

	case LogicalTypeId::FLOAT:
		return "real";

	case LogicalTypeId::DOUBLE:
		return "float";

	case LogicalTypeId::DECIMAL: {
		uint8_t width, scale;
		duckdb_type.GetDecimalProperties(width, scale);
		return StringUtil::Format("decimal(%d,%d)", width, scale);
	}

	case LogicalTypeId::VARCHAR: {
		if (!single_byte_text) {
			return "nvarchar(max)";
		}
		// A single-byte target is only correct with a UTF-8 collation; an empty
		// one means the database default already is one (Fabric), which is the
		// case that matters most here since Fabric has no nvarchar at all.
		return varchar_collation.empty() ? "varchar(max)" : "varchar(max) COLLATE " + varchar_collation;
	}

	case LogicalTypeId::BLOB:
		return "varbinary(max)";

	case LogicalTypeId::UUID:
		return "uniqueidentifier";

	case LogicalTypeId::DATE:
		return "date";

	case LogicalTypeId::TIME:
		return "time(6)";  // DuckDB uses microsecond precision

	case LogicalTypeId::TIMESTAMP:
		return "datetime2(6)";	// µs — DuckDB native, exact match
	case LogicalTypeId::TIMESTAMP_MS:
		return "datetime2(3)";	// ms — exact (matches codec FormatDdlTypeName)
	case LogicalTypeId::TIMESTAMP_NS:
		return "datetime2(7)";	// 100 ns — DATETIME2 max precision
	case LogicalTypeId::TIMESTAMP_SEC:
		return "datetime2(0)";	// s — exact

	case LogicalTypeId::TIMESTAMP_TZ:
		return "datetimeoffset(7)";	 // 100 ns — matches codec FormatDdlTypeName

	case LogicalTypeId::HUGEINT:
		return "decimal(38,0)";	 // HUGEINT maps to max precision decimal

	case LogicalTypeId::INTERVAL:
		// Spec 045 FR-026: INTERVAL stored as canonical Interval::ToString text in NVARCHAR(50).
		return "nvarchar(50)";

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
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::BIGINT:
		return tds::TDS_TYPE_INTN;	// 0x26

	case LogicalTypeId::UBIGINT:
		return tds::TDS_TYPE_DECIMAL;  // 0x6A - UBIGINT needs DECIMAL(20,0) for full range

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

	case LogicalTypeId::INTERVAL:
		// Spec 045 FR-026: INTERVAL travels as NVARCHAR text on the BCP wire.
		return tds::TDS_TYPE_NVARCHAR;	// 0xE7

	default:
		throw NotImplementedException("MSSQL COPY: Unsupported DuckDB type for TDS: %s", duckdb_type.ToString());
	}
}

//===----------------------------------------------------------------------===//
// TargetResolver::GetTDSMaxLength
//===----------------------------------------------------------------------===//

uint16_t TargetResolver::GetTDSMaxLength(const LogicalType &duckdb_type) {
	// Spec 060: without this the INSERT BULK declaration says nvarchar(max) for a
	// column the same statement created as nvarchar(50), and the encoder's
	// overflow guard — which reads this number — never fires at all. CTAS reaches
	// this path for every column; COPY reaches it when replacing a table.
	//
	// Every char type travels as NVARCHAR, so the unit here is UTF-16 BYTES.
	// MSSQL_VARCHAR(n) counts n bytes of UTF-8 in the target column, and n UTF-8
	// bytes never hold more than n characters, so 2n is an upper bound on the wire
	// — permissive, never rejecting a value the column would have accepted.
	codec::TargetStringType target_string;
	if (codec::TryGetTargetStringType(duckdb_type, target_string)) {
		const int32_t wire_bytes = target_string.length * 2;
		// Past 8000 bytes the inline NVARCHAR form is out of room and the value
		// has to travel as PLP, exactly as an oversized existing column does.
		return wire_bytes > 8000 ? 0xFFFF : static_cast<uint16_t>(wire_bytes);
	}

	switch (duckdb_type.id()) {
	case LogicalTypeId::BOOLEAN:
		return 1;

	case LogicalTypeId::TINYINT:
	case LogicalTypeId::UTINYINT:
		return 1;

	case LogicalTypeId::SMALLINT:
		return 2;

	case LogicalTypeId::USMALLINT:
		return 4;  // USMALLINT (0-65535) maps to int (4 bytes)

	case LogicalTypeId::INTEGER:
		return 4;

	case LogicalTypeId::UINTEGER:
		return 8;  // UINTEGER (0-4B) maps to bigint (8 bytes)

	case LogicalTypeId::BIGINT:
		return 8;

	case LogicalTypeId::UBIGINT:
		return 9;  // DECIMAL(20,0) - precision 20 needs 9 bytes storage

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

	case LogicalTypeId::INTERVAL:
		// Spec 045 FR-026: nvarchar(50) = 100 UTF-16LE bytes of capacity.
		return 100;

	default:
		throw NotImplementedException("MSSQL COPY: Unsupported DuckDB type for max_length: %s", duckdb_type.ToString());
	}
}

}  // namespace mssql
}  // namespace duckdb
