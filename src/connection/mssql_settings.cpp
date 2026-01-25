#include "connection/mssql_settings.hpp"
#include "dml/mssql_dml_config.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/config.hpp"
#include "insert/mssql_insert_config.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Setting Validators
//===----------------------------------------------------------------------===//

static void ValidatePositive(ClientContext &context, SetScope scope, Value &parameter) {
	auto val = parameter.GetValue<int64_t>();
	if (val < 1) {
		throw InvalidInputException("Value must be >= 1, got: %lld", val);
	}
}

static void ValidateNonNegative(ClientContext &context, SetScope scope, Value &parameter) {
	auto val = parameter.GetValue<int64_t>();
	if (val < 0) {
		throw InvalidInputException("Value must be >= 0, got: %lld", val);
	}
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

void RegisterMSSQLSettings(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);

	// mssql_connection_limit - Maximum connections per attached database context
	config.AddExtensionOption("mssql_connection_limit", "Maximum connections per attached mssql database",
							  LogicalType::BIGINT, Value::BIGINT(tds::DEFAULT_CONNECTION_LIMIT), ValidatePositive,
							  SetScope::GLOBAL);

	// mssql_connection_cache - Enable connection pooling and reuse
	config.AddExtensionOption("mssql_connection_cache", "Enable connection pooling and reuse", LogicalType::BOOLEAN,
							  Value::BOOLEAN(tds::DEFAULT_CONNECTION_CACHE), nullptr, SetScope::GLOBAL);

	// mssql_connection_timeout - TCP connection timeout in seconds
	config.AddExtensionOption("mssql_connection_timeout", "TCP connection timeout in seconds", LogicalType::BIGINT,
							  Value::BIGINT(tds::DEFAULT_CONNECTION_TIMEOUT), ValidateNonNegative, SetScope::GLOBAL);

	// mssql_idle_timeout - Idle connection timeout in seconds
	config.AddExtensionOption("mssql_idle_timeout", "Idle connection timeout in seconds (0 = no timeout)",
							  LogicalType::BIGINT, Value::BIGINT(tds::DEFAULT_IDLE_TIMEOUT), ValidateNonNegative,
							  SetScope::GLOBAL);

	// mssql_min_connections - Minimum connections to maintain per context
	config.AddExtensionOption("mssql_min_connections", "Minimum connections to maintain per context",
							  LogicalType::BIGINT, Value::BIGINT(tds::DEFAULT_MIN_CONNECTIONS), ValidateNonNegative,
							  SetScope::GLOBAL);

	// mssql_acquire_timeout - Connection acquire timeout in seconds
	config.AddExtensionOption("mssql_acquire_timeout", "Connection acquire timeout in seconds (0 = fail immediately)",
							  LogicalType::BIGINT, Value::BIGINT(tds::DEFAULT_ACQUIRE_TIMEOUT), ValidateNonNegative,
							  SetScope::GLOBAL);

	// mssql_catalog_cache_ttl - Metadata cache TTL in seconds (0 = manual refresh only)
	config.AddExtensionOption("mssql_catalog_cache_ttl", "Metadata cache TTL in seconds (0 = manual refresh only)",
							  LogicalType::BIGINT,
							  Value::BIGINT(0),	 // Default: disabled, manual refresh only
							  ValidateNonNegative, SetScope::GLOBAL);

	//===----------------------------------------------------------------------===//
	// Statistics Settings
	//===----------------------------------------------------------------------===//

	// mssql_enable_statistics - Enable statistics collection for optimizer
	config.AddExtensionOption("mssql_enable_statistics",
							  "Enable statistics collection from SQL Server for query optimizer", LogicalType::BOOLEAN,
							  Value::BOOLEAN(DEFAULT_STATISTICS_ENABLED), nullptr, SetScope::GLOBAL);

	// mssql_statistics_level - Statistics detail level (0=rowcount, 1=+histogram, 2=+NDV)
	config.AddExtensionOption("mssql_statistics_level",
							  "Statistics detail level: 0=row count, 1=+histogram min/max, 2=+NDV", LogicalType::BIGINT,
							  Value::BIGINT(DEFAULT_STATISTICS_LEVEL), ValidateNonNegative, SetScope::GLOBAL);

	// mssql_statistics_use_dbcc - Allow DBCC SHOW_STATISTICS for column stats
	config.AddExtensionOption(
		"mssql_statistics_use_dbcc", "Allow DBCC SHOW_STATISTICS for column statistics (requires permissions)",
		LogicalType::BOOLEAN, Value::BOOLEAN(DEFAULT_STATISTICS_USE_DBCC), nullptr, SetScope::GLOBAL);

	// mssql_statistics_cache_ttl_seconds - Statistics cache TTL
	config.AddExtensionOption("mssql_statistics_cache_ttl_seconds", "Statistics cache TTL in seconds",
							  LogicalType::BIGINT, Value::BIGINT(DEFAULT_STATISTICS_CACHE_TTL), ValidateNonNegative,
							  SetScope::GLOBAL);

	//===----------------------------------------------------------------------===//
	// INSERT Settings
	//===----------------------------------------------------------------------===//

	// mssql_insert_batch_size - Maximum rows per INSERT statement
	// SQL Server limits VALUES clause to 1000 rows per INSERT
	config.AddExtensionOption("mssql_insert_batch_size", "Maximum rows per INSERT statement (SQL Server limit: 1000)",
							  LogicalType::BIGINT, Value::BIGINT(MSSQL_DEFAULT_INSERT_BATCH_SIZE), ValidatePositive,
							  SetScope::GLOBAL);

	// mssql_insert_max_rows_per_statement - Hard cap on rows per INSERT statement
	config.AddExtensionOption("mssql_insert_max_rows_per_statement",
							  "Hard cap on rows per INSERT statement (SQL Server limit: 1000)", LogicalType::BIGINT,
							  Value::BIGINT(MSSQL_DEFAULT_INSERT_MAX_ROWS_PER_STATEMENT), ValidatePositive,
							  SetScope::GLOBAL);

	// mssql_insert_max_sql_bytes - Maximum SQL statement size in bytes
	config.AddExtensionOption("mssql_insert_max_sql_bytes", "Maximum SQL statement size in bytes", LogicalType::BIGINT,
							  Value::BIGINT(MSSQL_DEFAULT_INSERT_MAX_SQL_BYTES), ValidatePositive, SetScope::GLOBAL);

	// mssql_insert_use_returning_output - Use OUTPUT INSERTED for RETURNING clause
	config.AddExtensionOption("mssql_insert_use_returning_output", "Use OUTPUT INSERTED for RETURNING clause",
							  LogicalType::BOOLEAN, Value::BOOLEAN(MSSQL_DEFAULT_INSERT_USE_RETURNING_OUTPUT), nullptr,
							  SetScope::GLOBAL);

	//===----------------------------------------------------------------------===//
	// DML (UPDATE/DELETE) Settings
	//===----------------------------------------------------------------------===//

	// mssql_dml_batch_size - Maximum rows per UPDATE/DELETE batch
	// Conservative default (500) to stay well under SQL Server's ~2100 parameter limit
	config.AddExtensionOption(
		"mssql_dml_batch_size", "Maximum rows per UPDATE/DELETE batch (default: 500, affects parameter count)",
		LogicalType::BIGINT, Value::BIGINT(MSSQL_DEFAULT_DML_BATCH_SIZE), ValidatePositive, SetScope::GLOBAL);

	// mssql_dml_max_parameters - Maximum parameters per DML statement
	// SQL Server limit is approximately 2100, we use 2000 for safety margin
	config.AddExtensionOption(
		"mssql_dml_max_parameters", "Maximum parameters per UPDATE/DELETE statement (SQL Server limit ~2100)",
		LogicalType::BIGINT, Value::BIGINT(MSSQL_DEFAULT_DML_MAX_PARAMETERS), ValidatePositive, SetScope::GLOBAL);

	// mssql_dml_use_prepared - Use prepared statements for DML operations
	config.AddExtensionOption("mssql_dml_use_prepared", "Use prepared statements for UPDATE/DELETE operations",
							  LogicalType::BOOLEAN, Value::BOOLEAN(MSSQL_DEFAULT_DML_USE_PREPARED), nullptr,
							  SetScope::GLOBAL);
}

//===----------------------------------------------------------------------===//
// Loading Configuration
//===----------------------------------------------------------------------===//

MSSQLPoolConfig LoadPoolConfig(ClientContext &context) {
	MSSQLPoolConfig config;
	Value val;

	if (context.TryGetCurrentSetting("mssql_connection_limit", val)) {
		config.connection_limit = static_cast<size_t>(val.GetValue<int64_t>());
	}

	if (context.TryGetCurrentSetting("mssql_connection_cache", val)) {
		config.connection_cache = val.GetValue<bool>();
	}

	if (context.TryGetCurrentSetting("mssql_connection_timeout", val)) {
		config.connection_timeout = static_cast<int>(val.GetValue<int64_t>());
	}

	if (context.TryGetCurrentSetting("mssql_idle_timeout", val)) {
		config.idle_timeout = static_cast<int>(val.GetValue<int64_t>());
	}

	if (context.TryGetCurrentSetting("mssql_min_connections", val)) {
		config.min_connections = static_cast<size_t>(val.GetValue<int64_t>());
	}

	if (context.TryGetCurrentSetting("mssql_acquire_timeout", val)) {
		config.acquire_timeout = static_cast<int>(val.GetValue<int64_t>());
	}

	return config;
}

int64_t LoadCatalogCacheTTL(ClientContext &context) {
	Value val;
	if (context.TryGetCurrentSetting("mssql_catalog_cache_ttl", val)) {
		return val.GetValue<int64_t>();
	}
	return 0;  // Default: manual refresh only
}

//===----------------------------------------------------------------------===//
// Statistics Configuration Loading
//===----------------------------------------------------------------------===//

bool LoadStatisticsEnabled(ClientContext &context) {
	Value val;
	if (context.TryGetCurrentSetting("mssql_enable_statistics", val)) {
		return val.GetValue<bool>();
	}
	return DEFAULT_STATISTICS_ENABLED;
}

int64_t LoadStatisticsLevel(ClientContext &context) {
	Value val;
	if (context.TryGetCurrentSetting("mssql_statistics_level", val)) {
		return val.GetValue<int64_t>();
	}
	return DEFAULT_STATISTICS_LEVEL;
}

bool LoadStatisticsUseDBCC(ClientContext &context) {
	Value val;
	if (context.TryGetCurrentSetting("mssql_statistics_use_dbcc", val)) {
		return val.GetValue<bool>();
	}
	return DEFAULT_STATISTICS_USE_DBCC;
}

int64_t LoadStatisticsCacheTTL(ClientContext &context) {
	Value val;
	if (context.TryGetCurrentSetting("mssql_statistics_cache_ttl_seconds", val)) {
		return val.GetValue<int64_t>();
	}
	return DEFAULT_STATISTICS_CACHE_TTL;
}

MSSQLStatisticsConfig LoadStatisticsConfig(ClientContext &context) {
	MSSQLStatisticsConfig config;
	config.enabled = LoadStatisticsEnabled(context);
	config.level = LoadStatisticsLevel(context);
	config.use_dbcc = LoadStatisticsUseDBCC(context);
	config.cache_ttl_seconds = LoadStatisticsCacheTTL(context);
	return config;
}

//===----------------------------------------------------------------------===//
// INSERT Configuration Loading
//===----------------------------------------------------------------------===//

MSSQLInsertConfig LoadInsertConfig(ClientContext &context) {
	MSSQLInsertConfig config;
	Value val;

	if (context.TryGetCurrentSetting("mssql_insert_batch_size", val)) {
		config.batch_size = static_cast<idx_t>(val.GetValue<int64_t>());
	}

	if (context.TryGetCurrentSetting("mssql_insert_max_rows_per_statement", val)) {
		config.max_rows_per_statement = static_cast<idx_t>(val.GetValue<int64_t>());
	}

	if (context.TryGetCurrentSetting("mssql_insert_max_sql_bytes", val)) {
		config.max_sql_bytes = static_cast<idx_t>(val.GetValue<int64_t>());
	}

	if (context.TryGetCurrentSetting("mssql_insert_use_returning_output", val)) {
		config.use_returning_output = val.GetValue<bool>();
	}

	// Validate loaded config
	config.Validate();

	return config;
}

}  // namespace duckdb
