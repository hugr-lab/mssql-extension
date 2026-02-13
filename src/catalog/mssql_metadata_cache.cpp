#include "catalog/mssql_metadata_cache.hpp"
#include <cstdio>
#include <cstdlib>
#include "duckdb/common/exception.hpp"
#include "query/mssql_simple_query.hpp"

// Debug logging for metadata cache operations
static int GetMetadataCacheDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define CACHE_DEBUG(lvl, fmt, ...)                                     \
	do {                                                               \
		if (GetMetadataCacheDebugLevel() >= lvl)                       \
			fprintf(stderr, "[MSSQL CACHE] " fmt "\n", ##__VA_ARGS__); \
	} while (0)

namespace duckdb {

//===----------------------------------------------------------------------===//
// SQL Queries for Metadata Discovery
//===----------------------------------------------------------------------===//

// Query to discover all user schemas (including empty ones)
// Excludes system schemas: INFORMATION_SCHEMA (3), sys (4), and other built-in schemas
// Note: ORDER BY is appended dynamically after optional filter clauses
static const char *SCHEMA_DISCOVERY_SQL = R"(
SELECT s.name AS schema_name
FROM sys.schemas s
WHERE s.schema_id NOT IN (3, 4)
  AND s.principal_id != 0
  AND s.name NOT IN ('guest', 'INFORMATION_SCHEMA', 'sys', 'db_owner', 'db_accessadmin',
                     'db_securityadmin', 'db_ddladmin', 'db_backupoperator', 'db_datareader',
                     'db_datawriter', 'db_denydatareader', 'db_denydatawriter'))";

// Query to discover tables and views in a schema
// Uses simple string replacement for schema_name (safe for schema names)
// Note: ORDER BY is appended dynamically after optional filter clauses
static const char *TABLE_DISCOVERY_SQL_TEMPLATE = R"(
SELECT
    o.name AS object_name,
    o.type AS object_type,
    ISNULL(p.rows, 0) AS approx_rows
FROM sys.objects o
LEFT JOIN sys.partitions p ON o.object_id = p.object_id AND p.index_id IN (0, 1)
WHERE o.type IN ('U', 'V')
  AND o.is_ms_shipped = 0
  AND SCHEMA_NAME(o.schema_id) = '%s')";

// Single-table metadata query: loads object type, row count, and all columns for ONE table
// in a single round trip. Used by GetTableMetadata() to avoid loading all tables in schema.
static const char *SINGLE_TABLE_METADATA_SQL_TEMPLATE = R"(
SELECT
    o.type AS object_type,
    ISNULL(p.rows, 0) AS approx_rows,
    c.name AS column_name,
    c.column_id,
    t.name AS type_name,
    c.max_length,
    c.precision,
    c.scale,
    c.is_nullable,
    ISNULL(c.collation_name, '') AS collation_name
FROM sys.objects o
INNER JOIN sys.columns c ON c.object_id = o.object_id
JOIN sys.types t ON c.user_type_id = t.user_type_id
LEFT JOIN sys.partitions p ON o.object_id = p.object_id AND p.index_id IN (0, 1)
WHERE o.object_id = OBJECT_ID('%s')
ORDER BY c.column_id
)";

// Bulk metadata query scoped to a single schema
// Note: ORDER BY is appended dynamically after optional filter clauses
static const char *BULK_METADATA_SCHEMA_SQL_TEMPLATE = R"(
SELECT
    s.name AS schema_name,
    o.name AS object_name,
    o.type AS object_type,
    ISNULL(p.rows, 0) AS approx_rows,
    c.name AS column_name,
    c.column_id,
    t.name AS type_name,
    c.max_length,
    c.precision,
    c.scale,
    c.is_nullable,
    ISNULL(c.collation_name, '') AS collation_name
FROM sys.schemas s
INNER JOIN sys.objects o ON o.schema_id = s.schema_id
INNER JOIN sys.columns c ON c.object_id = o.object_id
JOIN sys.types t ON c.user_type_id = t.user_type_id
LEFT JOIN sys.partitions p ON o.object_id = p.object_id AND p.index_id IN (0, 1)
WHERE s.schema_id NOT IN (3, 4)
  AND s.principal_id != 0
  AND s.name NOT IN ('guest', 'INFORMATION_SCHEMA', 'sys', 'db_owner', 'db_accessadmin',
                     'db_securityadmin', 'db_ddladmin', 'db_backupoperator', 'db_datareader',
                     'db_datawriter', 'db_denydatareader', 'db_denydatawriter')
  AND o.type IN ('U', 'V')
  AND o.is_ms_shipped = 0
  AND s.name = '%s')";

// Query to discover columns in a table/view
// Note: ISNULL is used for collation_name to avoid NBCROW parsing issues with NULL values
static const char *COLUMN_DISCOVERY_SQL_TEMPLATE = R"(
SELECT
    c.name AS column_name,
    c.column_id,
    t.name AS type_name,
    c.max_length,
    c.precision,
    c.scale,
    c.is_nullable,
    ISNULL(c.collation_name, '') AS collation_name
FROM sys.columns c
JOIN sys.types t ON c.user_type_id = t.user_type_id
WHERE c.object_id = OBJECT_ID('%s')
ORDER BY c.column_id
)";

//===----------------------------------------------------------------------===//
// TTL Helper
//===----------------------------------------------------------------------===//

static bool IsTTLExpired(const std::chrono::steady_clock::time_point &last_refresh, int64_t ttl_seconds) {
	if (ttl_seconds <= 0) {
		return false;  // TTL disabled
	}
	auto now = std::chrono::steady_clock::now();
	auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_refresh).count();
	return elapsed >= ttl_seconds;
}

//===----------------------------------------------------------------------===//
// Helper: Execute metadata query using MSSQLSimpleQuery
//===----------------------------------------------------------------------===//

using MetadataRowCallback = std::function<void(const vector<string> &values)>;

static void RunMetadataQuery(tds::TdsConnection &connection, const string &sql, MetadataRowCallback callback,
							 int timeout_ms) {
	// Log the query being executed (truncated for readability)
	CACHE_DEBUG(1, "RunMetadataQuery: timeout=%dms, sql=%.120s%s", timeout_ms, sql.c_str(),
				sql.size() > 120 ? "..." : "");

	auto start = std::chrono::steady_clock::now();
	auto result = MSSQLSimpleQuery::ExecuteWithCallback(
		connection, sql,
		[&callback](const std::vector<std::string> &row) {
			// Convert std::vector to duckdb::vector
			vector<string> duckdb_row;
			duckdb_row.reserve(row.size());
			for (const auto &val : row) {
				duckdb_row.push_back(val);
			}
			callback(duckdb_row);
			return true;  // continue processing
		},
		timeout_ms);

	auto elapsed =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

	if (result.HasError()) {
		CACHE_DEBUG(1, "RunMetadataQuery: FAILED after %lldms — %s", (long long)elapsed, result.error_message.c_str());
		throw IOException("Metadata query failed: %s", result.error_message);
	}

	CACHE_DEBUG(1, "RunMetadataQuery: completed in %lldms", (long long)elapsed);
}

//===----------------------------------------------------------------------===//
// Move constructors for structs with mutex (T007, T008)
//===----------------------------------------------------------------------===//

MSSQLTableMetadata::MSSQLTableMetadata(MSSQLTableMetadata &&other) noexcept
	: name(std::move(other.name)),
	  object_type(other.object_type),
	  columns(std::move(other.columns)),
	  approx_row_count(other.approx_row_count),
	  columns_load_state(other.columns_load_state),
	  columns_last_refresh(other.columns_last_refresh) {
	// Note: load_mutex is default-constructed (not moved)
}

MSSQLTableMetadata &MSSQLTableMetadata::operator=(MSSQLTableMetadata &&other) noexcept {
	if (this != &other) {
		name = std::move(other.name);
		object_type = other.object_type;
		columns = std::move(other.columns);
		approx_row_count = other.approx_row_count;
		columns_load_state = other.columns_load_state;
		columns_last_refresh = other.columns_last_refresh;
		// Note: load_mutex is not moved
	}
	return *this;
}

MSSQLSchemaMetadata::MSSQLSchemaMetadata(MSSQLSchemaMetadata &&other) noexcept
	: name(std::move(other.name)),
	  tables(std::move(other.tables)),
	  tables_load_state(other.tables_load_state),
	  tables_last_refresh(other.tables_last_refresh) {
	// Note: load_mutex is default-constructed (not moved)
}

MSSQLSchemaMetadata &MSSQLSchemaMetadata::operator=(MSSQLSchemaMetadata &&other) noexcept {
	if (this != &other) {
		name = std::move(other.name);
		tables = std::move(other.tables);
		tables_load_state = other.tables_load_state;
		tables_last_refresh = other.tables_last_refresh;
		// Note: load_mutex is not moved
	}
	return *this;
}

//===----------------------------------------------------------------------===//
// Constructor
//===----------------------------------------------------------------------===//

MSSQLMetadataCache::MSSQLMetadataCache(int64_t ttl_seconds)
	: state_(MSSQLCacheState::EMPTY), ttl_seconds_(ttl_seconds) {}

//===----------------------------------------------------------------------===//
// Cache Access (with lazy loading) - T016, T017, T018
//===----------------------------------------------------------------------===//

void MSSQLMetadataCache::SetFilter(const MSSQLCatalogFilter *filter) {
	filter_ = filter;
}

const MSSQLCatalogFilter *MSSQLMetadataCache::GetFilter() const {
	return filter_;
}

vector<string> MSSQLMetadataCache::GetSchemaNames(tds::TdsConnection &connection) {
	// Trigger lazy loading of schema list
	EnsureSchemasLoaded(connection);

	std::lock_guard<std::mutex> lock(schemas_mutex_);
	vector<string> names;
	for (const auto &pair : schemas_) {
		// Apply schema filter if set
		if (filter_ && !filter_->MatchesSchema(pair.first)) {
			continue;
		}
		names.push_back(pair.first);
	}
	return names;
}

vector<string> MSSQLMetadataCache::GetTableNames(tds::TdsConnection &connection, const string &schema_name) {
	// Trigger lazy loading of schemas and tables for this schema
	EnsureTablesLoaded(connection, schema_name);

	std::lock_guard<std::mutex> lock(schemas_mutex_);
	vector<string> names;
	auto it = schemas_.find(schema_name);
	if (it != schemas_.end()) {
		for (const auto &pair : it->second.tables) {
			// Apply table filter if set
			if (filter_ && !filter_->MatchesTable(pair.first)) {
				continue;
			}
			names.push_back(pair.first);
		}
	}
	return names;
}

const MSSQLTableMetadata *MSSQLMetadataCache::GetTableMetadata(tds::TdsConnection &connection,
															   const string &schema_name, const string &table_name) {
	// Only load schemas (fast — just schema names, no tables)
	EnsureSchemasLoaded(connection);

	std::lock_guard<std::mutex> lock(schemas_mutex_);

	// Ensure schema entry exists
	auto schema_it = schemas_.find(schema_name);
	if (schema_it == schemas_.end()) {
		CACHE_DEBUG(1, "GetTableMetadata('%s.%s') — schema not found", schema_name.c_str(), table_name.c_str());
		return nullptr;
	}

	auto &schema = schema_it->second;

	// Check if table already cached with columns loaded
	auto table_it = schema.tables.find(table_name);
	if (table_it != schema.tables.end() && table_it->second.columns_load_state == CacheLoadState::LOADED &&
		!IsTTLExpired(table_it->second.columns_last_refresh, ttl_seconds_)) {
		CACHE_DEBUG(2, "GetTableMetadata('%s.%s') — cache hit (%zu columns)", schema_name.c_str(), table_name.c_str(),
					table_it->second.columns.size());
		return &table_it->second;
	}

	// Single-table query: load object type + columns in one round trip
	CACHE_DEBUG(1, "GetTableMetadata('%s.%s') — loading from SQL Server (single query)", schema_name.c_str(),
				table_name.c_str());

	string full_name = "[" + schema_name + "].[" + table_name + "]";
	string query = StringUtil::Format(SINGLE_TABLE_METADATA_SQL_TEMPLATE, full_name);

	MSSQLTableMetadata table_meta;
	table_meta.name = table_name;
	bool first_row = true;

	ExecuteMetadataQuery(connection, query, [this, &table_meta, &first_row](const vector<string> &values) {
		if (values.size() < 10) {
			return;
		}

		// First row: extract object type and row count
		if (first_row) {
			first_row = false;
			if (!values[0].empty() && values[0][0] == 'V') {
				table_meta.object_type = MSSQLObjectType::VIEW;
			} else {
				table_meta.object_type = MSSQLObjectType::TABLE;
			}
			try {
				table_meta.approx_row_count = static_cast<idx_t>(std::stoll(values[1]));
			} catch (...) {
				table_meta.approx_row_count = 0;
			}
		}

		// Parse column info
		string col_name = values[2];
		int32_t col_id = 0;
		try {
			col_id = static_cast<int32_t>(std::stoi(values[3]));
		} catch (...) {
		}
		string type_name = values[4];
		int16_t max_len = 0;
		try {
			max_len = static_cast<int16_t>(std::stoi(values[5]));
		} catch (...) {
		}
		uint8_t prec = 0;
		try {
			prec = static_cast<uint8_t>(std::stoi(values[6]));
		} catch (...) {
		}
		uint8_t scl = 0;
		try {
			scl = static_cast<uint8_t>(std::stoi(values[7]));
		} catch (...) {
		}
		bool nullable = (values[8] == "1" || values[8] == "true" || values[8] == "True");
		string collation = values[9];

		MSSQLColumnInfo col_info(col_name, col_id, type_name, max_len, prec, scl, nullable, collation,
								 database_collation_);
		table_meta.columns.push_back(std::move(col_info));
	});

	// If no rows returned, table doesn't exist
	if (first_row) {
		CACHE_DEBUG(1, "GetTableMetadata('%s.%s') — table not found on SQL Server", schema_name.c_str(),
					table_name.c_str());
		return nullptr;
	}

	// Cache the result
	auto now = std::chrono::steady_clock::now();
	table_meta.columns_load_state = CacheLoadState::LOADED;
	table_meta.columns_last_refresh = now;

	CACHE_DEBUG(1, "GetTableMetadata('%s.%s') — loaded %zu columns", schema_name.c_str(), table_name.c_str(),
				table_meta.columns.size());

	// Insert or replace: if entry already exists (e.g. invalidated), overwrite it
	auto existing_it = schema.tables.find(table_name);
	if (existing_it != schema.tables.end()) {
		existing_it->second = std::move(table_meta);
		return &existing_it->second;
	}
	auto result = schema.tables.emplace(table_name, std::move(table_meta));
	return &result.first->second;
}

bool MSSQLMetadataCache::HasSchema(const string &schema_name) {
	std::lock_guard<std::mutex> lock(mutex_);
	return schemas_.find(schema_name) != schemas_.end();
}

bool MSSQLMetadataCache::HasTable(const string &schema_name, const string &table_name) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto schema_it = schemas_.find(schema_name);
	if (schema_it == schemas_.end()) {
		return false;
	}
	return schema_it->second.tables.find(table_name) != schema_it->second.tables.end();
}

bool MSSQLMetadataCache::TryGetCachedSchemaNames(vector<string> &out_names) {
	std::lock_guard<std::mutex> lock(schemas_mutex_);

	// T036: Return cached schema names only if schemas are loaded and not expired
	if (schemas_load_state_ != CacheLoadState::LOADED) {
		return false;
	}

	// Check TTL expiration
	if (ttl_seconds_ > 0) {
		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - schemas_last_refresh_).count();
		if (elapsed >= ttl_seconds_) {
			return false;  // Expired, need to reload
		}
	}

	// Populate output with cached schema names (apply filter)
	out_names.clear();
	out_names.reserve(schemas_.size());
	for (const auto &pair : schemas_) {
		if (filter_ && !filter_->MatchesSchema(pair.first)) {
			continue;
		}
		out_names.push_back(pair.first);
	}
	return true;
}

void MSSQLMetadataCache::LoadAllTableMetadata(tds::TdsConnection &connection, const string &schema_name) {
	EnsureSchemasLoaded(connection);

	// Check if all tables in this schema already have columns loaded
	{
		std::lock_guard<std::mutex> lock(schemas_mutex_);
		auto schema_it = schemas_.find(schema_name);
		if (schema_it == schemas_.end()) {
			CACHE_DEBUG(1, "LoadAllTableMetadata('%s') — schema not found", schema_name.c_str());
			return;
		}

		auto &schema = schema_it->second;

		// If tables are loaded and all have columns loaded (not invalidated), use cache
		if (schema.tables_load_state == CacheLoadState::LOADED) {
			bool all_columns_loaded = true;
			for (const auto &table_pair : schema.tables) {
				if (table_pair.second.columns_load_state != CacheLoadState::LOADED ||
					IsTTLExpired(table_pair.second.columns_last_refresh, ttl_seconds_)) {
					all_columns_loaded = false;
					break;
				}
			}
			if (all_columns_loaded && !schema.tables.empty()) {
				CACHE_DEBUG(1, "LoadAllTableMetadata('%s') — all %zu tables already loaded", schema_name.c_str(),
							schema.tables.size());
				return;
			}
		}
	}

	// Bulk load all tables + columns for this schema in one query
	CACHE_DEBUG(1, "LoadAllTableMetadata('%s') — bulk loading from SQL Server", schema_name.c_str());

	string sql = StringUtil::Format(BULK_METADATA_SCHEMA_SQL_TEMPLATE, schema_name);

	// Push table filter to SQL Server if convertible to LIKE
	if (filter_ && filter_->HasTableFilter()) {
		string like_clause = MSSQLCatalogFilter::TryRegexToSQLLike(filter_->GetTablePattern(), "o.name");
		if (!like_clause.empty()) {
			sql += " AND " + like_clause;
			CACHE_DEBUG(1, "LoadAllTableMetadata('%s') — server-side table filter: %s", schema_name.c_str(),
						like_clause.c_str());
		}
	}
	sql += "\nORDER BY s.name, o.name, c.column_id";

	// Streaming group-by parse (same as BulkLoadAll but for one schema)
	string current_table;
	MSSQLTableMetadata *current_table_meta = nullptr;
	idx_t table_count = 0;
	idx_t column_count = 0;

	std::lock_guard<std::mutex> lock(schemas_mutex_);
	auto schema_it = schemas_.find(schema_name);
	if (schema_it == schemas_.end()) {
		return;
	}
	auto &schema = schema_it->second;

	// Clear old table entries — bulk reload replaces everything
	schema.tables.clear();

	ExecuteMetadataQuery(connection, sql, [&](const vector<string> &values) {
		if (values.size() < 12) {
			return;
		}

		string row_table = values[1];
		string row_type = values[2];
		string row_approx_rows = values[3];
		string col_name = values[4];
		string col_id_str = values[5];
		string type_name = values[6];
		string max_len_str = values[7];
		string prec_str = values[8];
		string scale_str = values[9];
		string nullable_str = values[10];
		string collation = values[11];

		// Apply table filter
		if (filter_ && !filter_->MatchesTable(row_table)) {
			return;
		}

		// New table group?
		if (row_table != current_table) {
			current_table = row_table;

			MSSQLTableMetadata table_meta;
			table_meta.name = current_table;
			if (!row_type.empty() && row_type[0] == 'V') {
				table_meta.object_type = MSSQLObjectType::VIEW;
			} else {
				table_meta.object_type = MSSQLObjectType::TABLE;
			}
			try {
				table_meta.approx_row_count = static_cast<idx_t>(std::stoll(row_approx_rows));
			} catch (...) {
				table_meta.approx_row_count = 0;
			}
			schema.tables.emplace(current_table, std::move(table_meta));
			auto table_it = schema.tables.find(current_table);
			current_table_meta = &table_it->second;
			table_count++;
		}

		// Parse column
		int32_t col_id = 0;
		try {
			col_id = static_cast<int32_t>(std::stoi(col_id_str));
		} catch (...) {
		}
		int16_t max_len = 0;
		try {
			max_len = static_cast<int16_t>(std::stoi(max_len_str));
		} catch (...) {
		}
		uint8_t prec = 0;
		try {
			prec = static_cast<uint8_t>(std::stoi(prec_str));
		} catch (...) {
		}
		uint8_t scl = 0;
		try {
			scl = static_cast<uint8_t>(std::stoi(scale_str));
		} catch (...) {
		}
		bool nullable = (nullable_str == "1" || nullable_str == "true" || nullable_str == "True");

		MSSQLColumnInfo col_info(col_name, col_id, type_name, max_len, prec, scl, nullable, collation,
								 database_collation_);
		current_table_meta->columns.push_back(std::move(col_info));
		column_count++;
	});

	// Mark all tables as loaded
	auto now = std::chrono::steady_clock::now();
	schema.tables_load_state = CacheLoadState::LOADED;
	schema.tables_last_refresh = now;
	for (auto &table_pair : schema.tables) {
		table_pair.second.columns_load_state = CacheLoadState::LOADED;
		table_pair.second.columns_last_refresh = now;
	}

	CACHE_DEBUG(1, "LoadAllTableMetadata('%s') — loaded %llu tables, %llu columns in one query", schema_name.c_str(),
				(unsigned long long)table_count, (unsigned long long)column_count);
}

//===----------------------------------------------------------------------===//
// Bulk Catalog Preload (Spec 033: US5)
//===----------------------------------------------------------------------===//

void MSSQLMetadataCache::BulkLoadAll(tds::TdsConnection &connection, const string &schema_name, idx_t &schema_count,
									 idx_t &table_count, idx_t &column_count) {
	schema_count = 0;
	table_count = 0;
	column_count = 0;

	// Determine which schemas to load.
	// When schema_name is empty, we iterate per-schema instead of one massive cross-schema
	// query. This avoids SQL Server tempdb sort spills on large catalogs (200K+ tables)
	// where the ORDER BY s.name, o.name, c.column_id on millions of rows exceeds the
	// memory grant and causes non-linear performance degradation.
	vector<string> schemas_to_load;
	if (!schema_name.empty()) {
		schemas_to_load.push_back(schema_name);
	} else {
		// Load schema names first (fast, lightweight query — no lock needed yet)
		string schema_sql = SCHEMA_DISCOVERY_SQL;
		if (filter_ && filter_->HasSchemaFilter()) {
			string like_clause = MSSQLCatalogFilter::TryRegexToSQLLike(filter_->GetSchemaPattern(), "s.name");
			if (!like_clause.empty()) {
				schema_sql += " AND " + like_clause;
			}
		}
		schema_sql += "\nORDER BY s.name";

		RunMetadataQuery(
			connection, schema_sql,
			[&](const vector<string> &values) {
				if (!values.empty()) {
					schemas_to_load.push_back(values[0]);
				}
			},
			metadata_timeout_ms_);

		CACHE_DEBUG(1, "BulkLoadAll: discovered %zu schemas to load", schemas_to_load.size());
	}

	std::lock_guard<std::mutex> lock(schemas_mutex_);

	// Load metadata per schema — each query sorts only within one schema,
	// keeping the result set small enough to avoid tempdb spills
	for (const auto &target_schema : schemas_to_load) {
		// Ensure schema entry exists
		auto schema_it = schemas_.find(target_schema);
		if (schema_it == schemas_.end()) {
			schemas_.emplace(target_schema, MSSQLSchemaMetadata(target_schema));
			schema_it = schemas_.find(target_schema);
			schema_count++;
		}
		auto &schema = schema_it->second;

		// Build per-schema query
		string sql = StringUtil::Format(BULK_METADATA_SCHEMA_SQL_TEMPLATE, target_schema);

		// Push table filter to SQL Server if convertible to LIKE
		if (filter_ && filter_->HasTableFilter()) {
			string like_clause = MSSQLCatalogFilter::TryRegexToSQLLike(filter_->GetTablePattern(), "o.name");
			if (!like_clause.empty()) {
				sql += " AND " + like_clause;
			}
		}
		sql += "\nORDER BY s.name, o.name, c.column_id";

		// Streaming group-by parse for this schema
		string current_table;
		MSSQLTableMetadata *current_table_meta = nullptr;
		idx_t schema_tables = 0;
		idx_t schema_columns = 0;

		ExecuteMetadataQuery(connection, sql, [&](const vector<string> &values) {
			if (values.size() < 12) {
				return;
			}

			string row_table = values[1];
			string row_type = values[2];
			string row_approx_rows = values[3];
			string col_name = values[4];
			string col_id_str = values[5];
			string type_name = values[6];
			string max_len_str = values[7];
			string prec_str = values[8];
			string scale_str = values[9];
			string nullable_str = values[10];
			string collation = values[11];

			// Apply table filter
			if (filter_ && !filter_->MatchesTable(row_table)) {
				return;
			}

			// New table group?
			if (row_table != current_table) {
				current_table = row_table;

				auto &tables = schema.tables;
				auto table_it = tables.find(current_table);
				if (table_it == tables.end()) {
					MSSQLTableMetadata table_meta;
					table_meta.name = current_table;

					// Object type
					if (!row_type.empty() && row_type[0] == 'V') {
						table_meta.object_type = MSSQLObjectType::VIEW;
					} else {
						table_meta.object_type = MSSQLObjectType::TABLE;
					}

					// Approximate row count
					try {
						table_meta.approx_row_count = static_cast<idx_t>(std::stoll(row_approx_rows));
					} catch (...) {
						table_meta.approx_row_count = 0;
					}

					tables.emplace(current_table, std::move(table_meta));
					table_it = tables.find(current_table);
					schema_tables++;
					table_count++;
				} else {
					// Table already exists (e.g. columns loaded by a prior single-table query).
					// Clear columns to avoid duplicates, since we're reloading from bulk query.
					table_it->second.columns.clear();
				}
				current_table_meta = &table_it->second;
			}

			// Parse column info
			int32_t col_id = 0;
			try {
				col_id = static_cast<int32_t>(std::stoi(col_id_str));
			} catch (...) {
			}
			int16_t max_len = 0;
			try {
				max_len = static_cast<int16_t>(std::stoi(max_len_str));
			} catch (...) {
			}
			uint8_t prec = 0;
			try {
				prec = static_cast<uint8_t>(std::stoi(prec_str));
			} catch (...) {
			}
			uint8_t scl = 0;
			try {
				scl = static_cast<uint8_t>(std::stoi(scale_str));
			} catch (...) {
			}
			bool nullable = (nullable_str == "1" || nullable_str == "true" || nullable_str == "True");

			MSSQLColumnInfo col_info(col_name, col_id, type_name, max_len, prec, scl, nullable, collation,
									 database_collation_);
			current_table_meta->columns.push_back(std::move(col_info));
			schema_columns++;
			column_count++;
		});

		CACHE_DEBUG(1, "BulkLoadAll: schema '%s' — %llu tables, %llu columns", target_schema.c_str(),
					(unsigned long long)schema_tables, (unsigned long long)schema_columns);
	}

	// Mark all load states as LOADED
	auto now = std::chrono::steady_clock::now();
	schemas_load_state_ = CacheLoadState::LOADED;
	schemas_last_refresh_ = now;

	for (auto &schema_pair : schemas_) {
		schema_pair.second.tables_load_state = CacheLoadState::LOADED;
		schema_pair.second.tables_last_refresh = now;

		for (auto &table_pair : schema_pair.second.tables) {
			table_pair.second.columns_load_state = CacheLoadState::LOADED;
			table_pair.second.columns_last_refresh = now;
		}
	}

	// Update backward-compat state
	state_ = MSSQLCacheState::LOADED;
	last_refresh_ = now;
}

void MSSQLMetadataCache::ForEachTable(
	const std::function<void(const string &, const string &, idx_t)> &callback) const {
	std::lock_guard<std::mutex> lock(schemas_mutex_);
	for (const auto &schema_pair : schemas_) {
		for (const auto &table_pair : schema_pair.second.tables) {
			callback(schema_pair.first, table_pair.first, table_pair.second.approx_row_count);
		}
	}
}

void MSSQLMetadataCache::ForEachTableInSchema(
	const string &schema_name, const std::function<void(const string &, const MSSQLTableMetadata &)> &callback) const {
	std::lock_guard<std::mutex> lock(schemas_mutex_);
	auto schema_it = schemas_.find(schema_name);
	if (schema_it == schemas_.end()) {
		return;
	}
	for (const auto &table_pair : schema_it->second.tables) {
		callback(table_pair.first, table_pair.second);
	}
}

//===----------------------------------------------------------------------===//
// Cache Management
//===----------------------------------------------------------------------===//

void MSSQLMetadataCache::Refresh(tds::TdsConnection &connection, const string &database_collation) {
	std::lock_guard<std::mutex> lock(mutex_);

	// Mark as loading
	state_ = MSSQLCacheState::LOADING;

	// Clear existing data
	schemas_.clear();
	database_collation_ = database_collation;

	try {
		// Load schemas
		LoadSchemas(connection);

		// Load tables for each schema
		for (auto &pair : schemas_) {
			LoadTables(connection, pair.first);

			// Load columns for each table
			for (auto &table_pair : pair.second.tables) {
				LoadColumns(connection, pair.first, table_pair.first, table_pair.second);
			}
		}

		// Update state and timestamp (backward-compat)
		state_ = MSSQLCacheState::LOADED;
		last_refresh_ = std::chrono::steady_clock::now();

		// Update incremental cache timestamps for all levels
		auto now = std::chrono::steady_clock::now();
		schemas_load_state_ = CacheLoadState::LOADED;
		schemas_last_refresh_ = now;

		for (auto &schema_pair : schemas_) {
			schema_pair.second.tables_load_state = CacheLoadState::LOADED;
			schema_pair.second.tables_last_refresh = now;

			for (auto &table_pair : schema_pair.second.tables) {
				table_pair.second.columns_load_state = CacheLoadState::LOADED;
				table_pair.second.columns_last_refresh = now;
			}
		}
	} catch (...) {
		state_ = MSSQLCacheState::INVALID;
		throw;
	}
}

bool MSSQLMetadataCache::IsExpired() const {
	if (ttl_seconds_ <= 0) {
		return false;  // TTL disabled, never auto-expires
	}

	std::lock_guard<std::mutex> lock(mutex_);
	if (state_ != MSSQLCacheState::LOADED) {
		return true;
	}

	auto now = std::chrono::steady_clock::now();
	auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_refresh_).count();
	return elapsed >= ttl_seconds_;
}

bool MSSQLMetadataCache::NeedsRefresh() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return state_ == MSSQLCacheState::EMPTY || state_ == MSSQLCacheState::STALE || state_ == MSSQLCacheState::INVALID;
}

void MSSQLMetadataCache::Invalidate() {
	// Use InvalidateAll() to reset both backward-compat state and incremental cache states
	InvalidateAll();
}

MSSQLCacheState MSSQLMetadataCache::GetState() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return state_;
}

void MSSQLMetadataCache::SetTTL(int64_t ttl_seconds) {
	std::lock_guard<std::mutex> lock(mutex_);
	ttl_seconds_ = ttl_seconds;
}

int64_t MSSQLMetadataCache::GetTTL() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return ttl_seconds_;
}

void MSSQLMetadataCache::SetDatabaseCollation(const string &collation) {
	std::lock_guard<std::mutex> lock(mutex_);
	database_collation_ = collation;
}

const string &MSSQLMetadataCache::GetDatabaseCollation() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return database_collation_;
}

void MSSQLMetadataCache::SetMetadataTimeout(int timeout_seconds) {
	metadata_timeout_ms_ = timeout_seconds > 0 ? timeout_seconds * 1000 : 0;
}

int MSSQLMetadataCache::GetMetadataTimeoutMs() const {
	return metadata_timeout_ms_;
}

void MSSQLMetadataCache::ExecuteMetadataQuery(tds::TdsConnection &connection, const string &sql,
											  MSSQLMetadataCache::MetadataRowCallback callback) {
	RunMetadataQuery(connection, sql, std::move(callback), metadata_timeout_ms_);
}

//===----------------------------------------------------------------------===//
//===----------------------------------------------------------------------===//
// Incremental Cache Loading - Lazy Loading (T010-T012)
//===----------------------------------------------------------------------===//

void MSSQLMetadataCache::EnsureSchemasLoaded(tds::TdsConnection &connection) {
	// Fast path: already loaded and not expired
	if (schemas_load_state_ == CacheLoadState::LOADED && !IsTTLExpired(schemas_last_refresh_, ttl_seconds_)) {
		CACHE_DEBUG(2, "EnsureSchemasLoaded — already loaded (%zu schemas)", schemas_.size());
		return;
	}
	CACHE_DEBUG(1, "EnsureSchemasLoaded — loading schemas from SQL Server");

	// Slow path: acquire lock and double-check
	std::lock_guard<std::mutex> lock(schemas_mutex_);

	// Double-check after acquiring lock
	if (schemas_load_state_ == CacheLoadState::LOADED && !IsTTLExpired(schemas_last_refresh_, ttl_seconds_)) {
		return;
	}

	// Mark as loading
	schemas_load_state_ = CacheLoadState::LOADING;

	try {
		// Clear existing schemas (preserve database_collation_)
		schemas_.clear();

		// Load schema names only (no tables/columns)
		string schema_sql = SCHEMA_DISCOVERY_SQL;
		// Push schema filter to SQL Server if convertible to LIKE
		if (filter_ && filter_->HasSchemaFilter()) {
			string like_clause = MSSQLCatalogFilter::TryRegexToSQLLike(filter_->GetSchemaPattern(), "s.name");
			if (!like_clause.empty()) {
				schema_sql += " AND " + like_clause;
				CACHE_DEBUG(1, "EnsureSchemasLoaded — server-side schema filter: %s", like_clause.c_str());
			}
		}
		schema_sql += "\nORDER BY s.name";

		ExecuteMetadataQuery(connection, schema_sql, [this](const vector<string> &values) {
			if (!values.empty()) {
				string schema_name = values[0];
				// Create schema with only name - tables NOT loaded (tables_load_state = NOT_LOADED)
				schemas_.emplace(schema_name, MSSQLSchemaMetadata(schema_name));
			}
		});

		// Update state
		CACHE_DEBUG(1, "EnsureSchemasLoaded — loaded %zu schemas", schemas_.size());
		schemas_load_state_ = CacheLoadState::LOADED;
		schemas_last_refresh_ = std::chrono::steady_clock::now();

		// Update backward-compat state
		state_ = MSSQLCacheState::LOADED;
		last_refresh_ = schemas_last_refresh_;
	} catch (...) {
		schemas_load_state_ = CacheLoadState::NOT_LOADED;
		throw;
	}
}

void MSSQLMetadataCache::EnsureTablesLoaded(tds::TdsConnection &connection, const string &schema_name) {
	// First ensure schemas are loaded
	EnsureSchemasLoaded(connection);

	// Find schema
	auto schema_it = schemas_.find(schema_name);
	if (schema_it == schemas_.end()) {
		return;	 // Schema doesn't exist
	}

	MSSQLSchemaMetadata &schema = schema_it->second;

	// Fast path: already loaded and not expired
	if (schema.tables_load_state == CacheLoadState::LOADED && !IsTTLExpired(schema.tables_last_refresh, ttl_seconds_)) {
		CACHE_DEBUG(2, "EnsureTablesLoaded('%s') — already loaded (%zu tables)", schema_name.c_str(),
					schema.tables.size());
		return;
	}
	CACHE_DEBUG(1, "EnsureTablesLoaded('%s') — loading table names from SQL Server", schema_name.c_str());

	// Slow path: acquire schema's lock and double-check
	std::lock_guard<std::mutex> lock(schema.load_mutex);

	// Double-check after acquiring lock
	if (schema.tables_load_state == CacheLoadState::LOADED && !IsTTLExpired(schema.tables_last_refresh, ttl_seconds_)) {
		return;
	}

	// Mark as loading
	schema.tables_load_state = CacheLoadState::LOADING;

	try {
		// Clear existing tables
		schema.tables.clear();

		// Build query with schema name
		string query = StringUtil::Format(TABLE_DISCOVERY_SQL_TEMPLATE, schema_name);
		// Push table filter to SQL Server if convertible to LIKE
		if (filter_ && filter_->HasTableFilter()) {
			string like_clause = MSSQLCatalogFilter::TryRegexToSQLLike(filter_->GetTablePattern(), "o.name");
			if (!like_clause.empty()) {
				query += " AND " + like_clause;
				CACHE_DEBUG(1, "EnsureTablesLoaded('%s') — server-side table filter: %s", schema_name.c_str(),
							like_clause.c_str());
			}
		}
		query += "\nORDER BY o.name";

		ExecuteMetadataQuery(connection, query, [&schema](const vector<string> &values) {
			if (values.size() >= 3) {
				MSSQLTableMetadata table_meta;
				table_meta.name = values[0];

				// Object type: 'U' = table, 'V' = view
				string type_char = values[1];
				if (!type_char.empty()) {
					char c = type_char[0];
					table_meta.object_type = (c == 'V') ? MSSQLObjectType::VIEW : MSSQLObjectType::TABLE;
				} else {
					table_meta.object_type = MSSQLObjectType::TABLE;
				}

				// Parse row count
				try {
					table_meta.approx_row_count = static_cast<idx_t>(std::stoll(values[2]));
				} catch (...) {
					table_meta.approx_row_count = 0;
				}

				// Note: columns NOT loaded (columns_load_state = NOT_LOADED by default)
				schema.tables.emplace(table_meta.name, std::move(table_meta));
			}
		});

		// Update state
		CACHE_DEBUG(1, "EnsureTablesLoaded('%s') — loaded %zu tables (no column queries)", schema_name.c_str(),
					schema.tables.size());
		schema.tables_load_state = CacheLoadState::LOADED;
		schema.tables_last_refresh = std::chrono::steady_clock::now();
	} catch (...) {
		schema.tables_load_state = CacheLoadState::NOT_LOADED;
		throw;
	}
}

//===----------------------------------------------------------------------===//
// Point Invalidation (T034, T040, T043)
//===----------------------------------------------------------------------===//

void MSSQLMetadataCache::InvalidateSchema(const string &schema_name) {
	std::lock_guard<std::mutex> lock(schemas_mutex_);
	auto it = schemas_.find(schema_name);
	if (it != schemas_.end()) {
		it->second.tables_load_state = CacheLoadState::NOT_LOADED;
		// Also invalidate all cached table column metadata in this schema
		// so that GetTableMetadata re-fetches columns from SQL Server
		for (auto &table_pair : it->second.tables) {
			table_pair.second.columns_load_state = CacheLoadState::NOT_LOADED;
		}
	}
}

void MSSQLMetadataCache::InvalidateTable(const string &schema_name, const string &table_name) {
	std::lock_guard<std::mutex> lock(schemas_mutex_);
	auto schema_it = schemas_.find(schema_name);
	if (schema_it == schemas_.end()) {
		return;
	}

	auto table_it = schema_it->second.tables.find(table_name);
	if (table_it != schema_it->second.tables.end()) {
		table_it->second.columns_load_state = CacheLoadState::NOT_LOADED;
	}
}

void MSSQLMetadataCache::InvalidateAll() {
	std::lock_guard<std::mutex> lock(schemas_mutex_);
	schemas_load_state_ = CacheLoadState::NOT_LOADED;
	for (auto &schema_entry : schemas_) {
		schema_entry.second.tables_load_state = CacheLoadState::NOT_LOADED;
		for (auto &table_entry : schema_entry.second.tables) {
			table_entry.second.columns_load_state = CacheLoadState::NOT_LOADED;
		}
	}
	// Update backward-compat state
	state_ = MSSQLCacheState::INVALID;
}

//===----------------------------------------------------------------------===//
// Cache State Queries (T015)
//===----------------------------------------------------------------------===//

CacheLoadState MSSQLMetadataCache::GetSchemasState() const {
	std::lock_guard<std::mutex> lock(schemas_mutex_);
	return schemas_load_state_;
}

CacheLoadState MSSQLMetadataCache::GetTablesState(const string &schema_name) const {
	std::lock_guard<std::mutex> lock(schemas_mutex_);
	auto it = schemas_.find(schema_name);
	if (it == schemas_.end()) {
		return CacheLoadState::NOT_LOADED;
	}
	return it->second.tables_load_state;
}

CacheLoadState MSSQLMetadataCache::GetColumnsState(const string &schema_name, const string &table_name) const {
	std::lock_guard<std::mutex> lock(schemas_mutex_);
	auto schema_it = schemas_.find(schema_name);
	if (schema_it == schemas_.end()) {
		return CacheLoadState::NOT_LOADED;
	}
	auto table_it = schema_it->second.tables.find(table_name);
	if (table_it == schema_it->second.tables.end()) {
		return CacheLoadState::NOT_LOADED;
	}
	return table_it->second.columns_load_state;
}

//===----------------------------------------------------------------------===//
// Internal Loading Methods
//===----------------------------------------------------------------------===//

void MSSQLMetadataCache::LoadSchemas(tds::TdsConnection &connection) {
	string sql = SCHEMA_DISCOVERY_SQL;
	// Push schema filter to SQL Server if convertible to LIKE
	if (filter_ && filter_->HasSchemaFilter()) {
		string like_clause = MSSQLCatalogFilter::TryRegexToSQLLike(filter_->GetSchemaPattern(), "s.name");
		if (!like_clause.empty()) {
			sql += " AND " + like_clause;
		}
	}
	sql += "\nORDER BY s.name";

	ExecuteMetadataQuery(connection, sql, [this](const vector<string> &values) {
		if (!values.empty()) {
			string schema_name = values[0];
			MSSQLSchemaMetadata schema_meta;
			schema_meta.name = schema_name;
			schemas_[schema_name] = std::move(schema_meta);
		}
	});
}

void MSSQLMetadataCache::LoadTables(tds::TdsConnection &connection, const string &schema_name) {
	// Build query with schema name (safe: schema names are identifiers, not user input)
	string query = StringUtil::Format(TABLE_DISCOVERY_SQL_TEMPLATE, schema_name);
	// Push table filter to SQL Server if convertible to LIKE
	if (filter_ && filter_->HasTableFilter()) {
		string like_clause = MSSQLCatalogFilter::TryRegexToSQLLike(filter_->GetTablePattern(), "o.name");
		if (!like_clause.empty()) {
			query += " AND " + like_clause;
		}
	}
	query += "\nORDER BY o.name";

	auto &schema_meta = schemas_[schema_name];

	ExecuteMetadataQuery(connection, query, [&schema_meta](const vector<string> &values) {
		if (values.size() >= 3) {
			MSSQLTableMetadata table_meta;
			table_meta.name = values[0];

			// Object type: 'U' = table, 'V' = view
			string type_char = values[1];
			if (!type_char.empty()) {
				// Trim whitespace from type (SQL Server pads char columns)
				char c = type_char[0];
				table_meta.object_type = (c == 'V') ? MSSQLObjectType::VIEW : MSSQLObjectType::TABLE;
			} else {
				table_meta.object_type = MSSQLObjectType::TABLE;
			}

			// Parse row count
			try {
				table_meta.approx_row_count = static_cast<idx_t>(std::stoll(values[2]));
			} catch (...) {
				table_meta.approx_row_count = 0;
			}

			schema_meta.tables[table_meta.name] = std::move(table_meta);
		}
	});
}

void MSSQLMetadataCache::LoadColumns(tds::TdsConnection &connection, const string &schema_name,
									 const string &table_name, MSSQLTableMetadata &table_metadata) {
	// Build fully qualified object name
	string full_name = "[" + schema_name + "].[" + table_name + "]";

	// Build query with object name
	string query = StringUtil::Format(COLUMN_DISCOVERY_SQL_TEMPLATE, full_name);

	ExecuteMetadataQuery(connection, query, [this, &table_metadata](const vector<string> &values) {
		if (values.size() >= 8) {
			string col_name = values[0];
			int32_t col_id = 0;
			try {
				col_id = static_cast<int32_t>(std::stoi(values[1]));
			} catch (...) {
			}
			string type_name = values[2];
			int16_t max_len = 0;
			try {
				max_len = static_cast<int16_t>(std::stoi(values[3]));
			} catch (...) {
			}
			uint8_t prec = 0;
			try {
				prec = static_cast<uint8_t>(std::stoi(values[4]));
			} catch (...) {
			}
			uint8_t scl = 0;
			try {
				scl = static_cast<uint8_t>(std::stoi(values[5]));
			} catch (...) {
			}
			bool nullable = (values[6] == "1" || values[6] == "true" || values[6] == "True");
			string collation = values[7];

			MSSQLColumnInfo col_info(col_name, col_id, type_name, max_len, prec, scl, nullable, collation,
									 database_collation_);
			table_metadata.columns.push_back(std::move(col_info));
		}
	});
}

}  // namespace duckdb
