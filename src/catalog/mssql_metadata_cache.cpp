#include "catalog/mssql_metadata_cache.hpp"
#include "duckdb/common/exception.hpp"
#include "query/mssql_simple_query.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// SQL Queries for Metadata Discovery
//===----------------------------------------------------------------------===//

// Query to discover all user schemas (including empty ones)
// Excludes system schemas: INFORMATION_SCHEMA (3), sys (4), and other built-in schemas
static const char *SCHEMA_DISCOVERY_SQL = R"(
SELECT s.name AS schema_name
FROM sys.schemas s
WHERE s.schema_id NOT IN (3, 4)
  AND s.principal_id != 0
  AND s.name NOT IN ('guest', 'INFORMATION_SCHEMA', 'sys', 'db_owner', 'db_accessadmin',
                     'db_securityadmin', 'db_ddladmin', 'db_backupoperator', 'db_datareader',
                     'db_datawriter', 'db_denydatareader', 'db_denydatawriter')
ORDER BY s.name
)";

// Query to discover tables and views in a schema
// Uses simple string replacement for schema_name (safe for schema names)
static const char *TABLE_DISCOVERY_SQL_TEMPLATE = R"(
SELECT
    o.name AS object_name,
    o.type AS object_type,
    ISNULL(p.rows, 0) AS approx_rows
FROM sys.objects o
LEFT JOIN sys.partitions p ON o.object_id = p.object_id AND p.index_id IN (0, 1)
WHERE o.type IN ('U', 'V')
  AND o.is_ms_shipped = 0
  AND SCHEMA_NAME(o.schema_id) = '%s'
ORDER BY o.name
)";

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
// Helper: Execute metadata query using MSSQLSimpleQuery
//===----------------------------------------------------------------------===//

using MetadataRowCallback = std::function<void(const vector<string> &values)>;

static void ExecuteMetadataQuery(tds::TdsConnection &connection, const string &sql, MetadataRowCallback callback) {
	auto result =
		MSSQLSimpleQuery::ExecuteWithCallback(connection, sql, [&callback](const std::vector<std::string> &row) {
			// Convert std::vector to duckdb::vector
			vector<string> duckdb_row;
			duckdb_row.reserve(row.size());
			for (const auto &val : row) {
				duckdb_row.push_back(val);
			}
			callback(duckdb_row);
			return true;  // continue processing
		});

	if (result.HasError()) {
		throw IOException("Metadata query failed: %s", result.error_message);
	}
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

vector<string> MSSQLMetadataCache::GetSchemaNames(tds::TdsConnection &connection) {
	// Trigger lazy loading of schema list
	EnsureSchemasLoaded(connection);

	std::lock_guard<std::mutex> lock(schemas_mutex_);
	vector<string> names;
	for (const auto &pair : schemas_) {
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
			names.push_back(pair.first);
		}
	}
	return names;
}

const MSSQLTableMetadata *MSSQLMetadataCache::GetTableMetadata(tds::TdsConnection &connection,
															   const string &schema_name, const string &table_name) {
	// Trigger lazy loading of schemas, tables, and columns
	EnsureColumnsLoaded(connection, schema_name, table_name);

	std::lock_guard<std::mutex> lock(schemas_mutex_);
	auto schema_it = schemas_.find(schema_name);
	if (schema_it == schemas_.end()) {
		return nullptr;
	}
	auto table_it = schema_it->second.tables.find(table_name);
	if (table_it == schema_it->second.tables.end()) {
		return nullptr;
	}
	return &table_it->second;
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

	// Populate output with cached schema names
	out_names.clear();
	out_names.reserve(schemas_.size());
	for (const auto &pair : schemas_) {
		out_names.push_back(pair.first);
	}
	return true;
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

//===----------------------------------------------------------------------===//
// TTL Helper (T009)
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
// Incremental Cache Loading - Lazy Loading (T010-T012)
//===----------------------------------------------------------------------===//

void MSSQLMetadataCache::EnsureSchemasLoaded(tds::TdsConnection &connection) {
	// Fast path: already loaded and not expired
	if (schemas_load_state_ == CacheLoadState::LOADED && !IsTTLExpired(schemas_last_refresh_, ttl_seconds_)) {
		return;
	}

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
		ExecuteMetadataQuery(connection, SCHEMA_DISCOVERY_SQL, [this](const vector<string> &values) {
			if (!values.empty()) {
				string schema_name = values[0];
				// Create schema with only name - tables NOT loaded (tables_load_state = NOT_LOADED)
				schemas_.emplace(schema_name, MSSQLSchemaMetadata(schema_name));
			}
		});

		// Update state
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
		return;
	}

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
		schema.tables_load_state = CacheLoadState::LOADED;
		schema.tables_last_refresh = std::chrono::steady_clock::now();
	} catch (...) {
		schema.tables_load_state = CacheLoadState::NOT_LOADED;
		throw;
	}
}

void MSSQLMetadataCache::EnsureColumnsLoaded(tds::TdsConnection &connection, const string &schema_name,
											 const string &table_name) {
	// First ensure tables are loaded for this schema
	EnsureTablesLoaded(connection, schema_name);

	// Find schema and table
	auto schema_it = schemas_.find(schema_name);
	if (schema_it == schemas_.end()) {
		return;	 // Schema doesn't exist
	}

	auto table_it = schema_it->second.tables.find(table_name);
	if (table_it == schema_it->second.tables.end()) {
		return;	 // Table doesn't exist
	}

	MSSQLTableMetadata &table = table_it->second;

	// Fast path: already loaded and not expired
	if (table.columns_load_state == CacheLoadState::LOADED && !IsTTLExpired(table.columns_last_refresh, ttl_seconds_)) {
		return;
	}

	// Slow path: acquire table's lock and double-check
	std::lock_guard<std::mutex> lock(table.load_mutex);

	// Double-check after acquiring lock
	if (table.columns_load_state == CacheLoadState::LOADED && !IsTTLExpired(table.columns_last_refresh, ttl_seconds_)) {
		return;
	}

	// Mark as loading
	table.columns_load_state = CacheLoadState::LOADING;

	try {
		// Clear existing columns
		table.columns.clear();

		// Build fully qualified object name
		string full_name = "[" + schema_name + "].[" + table_name + "]";
		string query = StringUtil::Format(COLUMN_DISCOVERY_SQL_TEMPLATE, full_name);

		ExecuteMetadataQuery(connection, query, [this, &table](const vector<string> &values) {
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
				table.columns.push_back(std::move(col_info));
			}
		});

		// Update state
		table.columns_load_state = CacheLoadState::LOADED;
		table.columns_last_refresh = std::chrono::steady_clock::now();
	} catch (...) {
		table.columns_load_state = CacheLoadState::NOT_LOADED;
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
	ExecuteMetadataQuery(connection, SCHEMA_DISCOVERY_SQL, [this](const vector<string> &values) {
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
