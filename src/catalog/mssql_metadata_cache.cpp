#include "catalog/mssql_metadata_cache.hpp"
#include "query/mssql_simple_query.hpp"
#include "duckdb/common/exception.hpp"

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

static void ExecuteMetadataQuery(tds::TdsConnection &connection, const string &sql,
                                 MetadataRowCallback callback) {
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
	    });

	if (result.HasError()) {
		throw IOException("Metadata query failed: %s", result.error_message);
	}
}

//===----------------------------------------------------------------------===//
// Constructor
//===----------------------------------------------------------------------===//

MSSQLMetadataCache::MSSQLMetadataCache(int64_t ttl_seconds)
    : state_(MSSQLCacheState::EMPTY), ttl_seconds_(ttl_seconds) {
}

//===----------------------------------------------------------------------===//
// Cache Access
//===----------------------------------------------------------------------===//

vector<string> MSSQLMetadataCache::GetSchemaNames() {
	std::lock_guard<std::mutex> lock(mutex_);
	vector<string> names;
	for (const auto &pair : schemas_) {
		names.push_back(pair.first);
	}
	return names;
}

vector<string> MSSQLMetadataCache::GetTableNames(const string &schema_name) {
	std::lock_guard<std::mutex> lock(mutex_);
	vector<string> names;
	auto it = schemas_.find(schema_name);
	if (it != schemas_.end()) {
		for (const auto &pair : it->second.tables) {
			names.push_back(pair.first);
		}
	}
	return names;
}

const MSSQLTableMetadata *MSSQLMetadataCache::GetTableMetadata(const string &schema_name,
                                                               const string &table_name) {
	std::lock_guard<std::mutex> lock(mutex_);
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

		// Update state and timestamp
		state_ = MSSQLCacheState::LOADED;
		last_refresh_ = std::chrono::steady_clock::now();
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
	return state_ == MSSQLCacheState::EMPTY || state_ == MSSQLCacheState::STALE ||
	       state_ == MSSQLCacheState::INVALID;
}

void MSSQLMetadataCache::Invalidate() {
	std::lock_guard<std::mutex> lock(mutex_);
	if (state_ == MSSQLCacheState::LOADED) {
		state_ = MSSQLCacheState::INVALID;
	}
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

	ExecuteMetadataQuery(connection, query,
	                     [this, &table_metadata](const vector<string> &values) {
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

			                     MSSQLColumnInfo col_info(col_name, col_id, type_name, max_len, prec, scl, nullable,
			                                              collation, database_collation_);
			                     table_metadata.columns.push_back(std::move(col_info));
		                     }
	                     });
}

}  // namespace duckdb
