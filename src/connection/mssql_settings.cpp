#include "connection/mssql_settings.hpp"
#include "copy/bcp_config.hpp"
#include "dml/ctas/mssql_ctas_config.hpp"
#include "dml/insert/mssql_insert_config.hpp"
#include "dml/mssql_dml_config.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/config.hpp"

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

// A collation name goes into generated DDL as a bare identifier — COLLATE takes
// no quoting in T-SQL — so it is checked here rather than concatenated blind.
// SQL Server's own collation names are letters, digits and underscores only.
static void ValidateCollationName(ClientContext &context, SetScope scope, Value &parameter) {
	if (parameter.IsNull()) {
		return;
	}
	const auto name = parameter.ToString();
	for (const char c : name) {
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
		if (!ok) {
			throw InvalidInputException("Invalid collation name '%s': expected letters, digits and underscores", name);
		}
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

	// mssql_reset_connection - reset a pooled connection's SESSION before reuse
	// (issue #189). See DEFAULT_RESET_CONNECTION for what `false` takes on: this
	// is "I own this session's state", not "keep my temp tables".
	config.AddExtensionOption("mssql_reset_connection",
							  "Reset session state (temp tables, SET options, open transactions) when a connection "
							  "returns to the pool; false hands that state to the user",
							  LogicalType::BOOLEAN, Value::BOOLEAN(tds::DEFAULT_RESET_CONNECTION), nullptr,
							  SetScope::GLOBAL);

	// mssql_connection_timeout - TCP connection timeout in seconds
	config.AddExtensionOption("mssql_connection_timeout", "TCP connection timeout in seconds", LogicalType::BIGINT,
							  Value::BIGINT(tds::DEFAULT_CONNECTION_TIMEOUT), ValidateNonNegative, SetScope::GLOBAL);

	// mssql_login7_max_packet - TEST-ONLY (issue #138). Lowers the LOGIN7 / SSPI
	// integrated-auth fragmentation boundary so the multi-packet send path can be
	// exercised without an AD-sized Kerberos PAC. 0 = production default (4096);
	// effective values are clamped to [256, 32767] when the connection is built.
	config.AddExtensionOption("mssql_login7_max_packet",
							  "TEST-ONLY: max LOGIN7 TDS packet size in bytes for integrated auth (0 = default 4096)",
							  LogicalType::BIGINT, Value::BIGINT(0), ValidateNonNegative, SetScope::GLOBAL);

	// mssql_tds_packet_size - TDS frame size requested in LOGIN7 (spec 055).
	// The server replies with min(requested, its own maximum) in the PACKETSIZE
	// ENVCHANGE and never raises it on its own, so this value is the ceiling for
	// every subsequent packet in BOTH directions: it bounds how many recv() calls
	// a result set costs and how many send() calls a BCP batch costs.
	// Protocol range is [512, 32767]; values outside it are clamped when the
	// connection is built.
	//
	// Default raised from the 4096 the extension had always requested. Measured
	// against SQL Server 2022 (500k rows x 15 mixed columns, medians of 3, see
	// test/bench/bench_results_live_server.md): client CPU per value 106.4 -> 76.8
	// on read and 52.7 -> 38.7 on write, read wall 2.47s -> 1.40s. That server
	// caps the grant at 16384, so requesting 32767 measured identically and we
	// have no evidence either way about servers that would grant more.
	// The cost is server-side memory: SQL Server allocates network buffers per
	// session, so this is 16 KB per pooled connection rather than 4 KB.
	config.AddExtensionOption("mssql_tds_packet_size",
							  "TDS frame size in bytes requested at login, clamped to [512, 32767] (default: 16384)",
							  LogicalType::BIGINT, Value::BIGINT(static_cast<int64_t>(tds::TDS_PREFERRED_PACKET_SIZE)),
							  ValidateNonNegative, SetScope::GLOBAL);

	// mssql_utf8_support - advertise UTF8SUPPORT in LOGIN7 (issue #225).
	// With it, a column whose collation is a UTF-8 one arrives as UTF-8 (0xA7)
	// instead of being transcoded to UTF-16 (0xE7) for us: half the wire bytes for
	// ASCII-heavy data, and the client copies the bytes straight into the vector
	// instead of running the UTF-16 batch decode. Measured on 1M rows of ~41-char
	// values: 85.8 MB -> 43.9 MB on the wire, 0.44 s -> 0.25 s wall.
	// Requesting it is safe everywhere — a server that does not support the feature
	// simply omits the acknowledgement and nothing changes — so this exists to turn
	// the request OFF, not on.
	config.AddExtensionOption("mssql_utf8_support",
							  "Advertise the UTF8SUPPORT feature in LOGIN7 so UTF-8 collation columns arrive as "
							  "UTF-8 instead of UTF-16 (default: true)",
							  LogicalType::BOOLEAN, Value::BOOLEAN(true), nullptr, SetScope::GLOBAL);

	// mssql_browser_timeout_seconds - SQL Server Browser UDP query timeout (spec 045)
	// Used when resolving named instances (host\instance) via MC-SQLR.
	// Short by design — Browser is on the critical path of every named-instance attach.
	config.AddExtensionOption(
		"mssql_browser_timeout_seconds",
		"SQL Server Browser UDP query timeout in seconds for named-instance resolution (default: 3)",
		LogicalType::BIGINT, Value::BIGINT(3), ValidatePositive, SetScope::GLOBAL);

	// mssql_named_instance_resolution - Enable host\instance resolution via SQL Browser (spec 045)
	// Escape hatch for environments that strip outbound UDP 1434.
	config.AddExtensionOption("mssql_named_instance_resolution",
							  "Enable SQL Server Browser (UDP 1434) resolution of host\\instance connection strings",
							  LogicalType::BOOLEAN, Value::BOOLEAN(true), nullptr, SetScope::GLOBAL);

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

	// mssql_attach_validation_timeout - Spec 047 (US2): timeout for the
	// eager TCP + LOGIN7 round-trip that ATTACH runs to surface bad
	// credentials / unreachable host / wrong DB up front. 0 means use
	// mssql_connection_timeout. Distinct from connection_timeout so
	// operators can give ATTACH a shorter ceiling than steady-state
	// queries (ATTACH is on the user's interactive path; query pool
	// fills can tolerate a longer wait).
	config.AddExtensionOption("mssql_attach_validation_timeout",
							  "Timeout in seconds for the ATTACH-time credential round trip "
							  "(0 = inherit mssql_connection_timeout). Spec 047 / US2.",
							  LogicalType::BIGINT, Value::BIGINT(0), ValidateNonNegative, SetScope::GLOBAL);

	// mssql_query_timeout - Query execution timeout in seconds (0 = no timeout)
	config.AddExtensionOption("mssql_query_timeout", "Query execution timeout in seconds (0 = no timeout, default: 30)",
							  LogicalType::BIGINT, Value::BIGINT(tds::DEFAULT_QUERY_TIMEOUT), ValidateNonNegative,
							  SetScope::GLOBAL);

	// mssql_metadata_timeout - Metadata query timeout in seconds (0 = no timeout)
	// Large catalogs (100K+ tables) may need several minutes for bulk metadata discovery
	config.AddExtensionOption(
		"mssql_metadata_timeout",
		"Metadata query timeout in seconds (default: 300, 0 = no timeout). Increase for very large catalogs",
		LogicalType::BIGINT, Value::BIGINT(tds::DEFAULT_METADATA_TIMEOUT), ValidateNonNegative, SetScope::GLOBAL);

	// mssql_catalog_cache_ttl - Metadata cache TTL in seconds (0 = manual refresh only)
	config.AddExtensionOption("mssql_catalog_cache_ttl", "Metadata cache TTL in seconds (0 = manual refresh only)",
							  LogicalType::BIGINT,
							  Value::BIGINT(0),	 // Default: disabled, manual refresh only
							  ValidateNonNegative, SetScope::GLOBAL);

	// mssql_exec_invalidate_cache - Auto-invalidate the catalog cache after DDL run via mssql_exec()
	// (issue #151). DEFAULT FALSE: like the Postgres extension's postgres_execute, mssql_exec() does
	// not touch the cache by default — invalidate manually with mssql_invalidate_cache() /
	// mssql_refresh_cache() after schema-changing DDL. Set true to auto-invalidate.
	config.AddExtensionOption(
		"mssql_exec_invalidate_cache",
		"Invalidate the catalog cache after DDL executed via mssql_exec() (CREATE/DROP/ALTER/TRUNCATE/RENAME/EXEC)",
		LogicalType::BOOLEAN, Value::BOOLEAN(false), nullptr, SetScope::GLOBAL);

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

	//===----------------------------------------------------------------------===//
	// CTAS (CREATE TABLE AS SELECT) Settings
	//===----------------------------------------------------------------------===//

	// mssql_ctas_drop_on_failure - Drop table if CTAS insert phase fails
	config.AddExtensionOption("mssql_ctas_drop_on_failure",
							  "Drop table if CTAS insert phase fails (default: false, table remains for debugging)",
							  LogicalType::BOOLEAN, Value::BOOLEAN(false), nullptr, SetScope::GLOBAL);

	// mssql_ctas_text_type - Text column type for CTAS: NVARCHAR or VARCHAR
	// Governs BOTH table-creating data paths — CTAS and COPY — so the two cannot
	// disagree about what an unannotated DuckDB VARCHAR becomes (spec 060 D7).
	// The name is historical: it predates COPY honouring it.
	config.AddExtensionOption("mssql_ctas_text_type",
							  "Type given to an unannotated VARCHAR column by CTAS and COPY: NVARCHAR (Unicode, "
							  "default) or VARCHAR (single-byte, needs a UTF-8 collation)",
							  LogicalType::VARCHAR, Value("NVARCHAR"), nullptr, SetScope::GLOBAL);

	// mssql_default_string_length - length given to an unannotated VARCHAR column
	// created by CTAS or COPY (spec 060 D9). 0 means MAX, which is what a plain
	// VARCHAR has always meant, so nothing changes for anyone who sets nothing.
	//
	// It exists because nvarchar(max) is a poor default for a loaded table: it
	// measured 4.1x slower to load than a sized target. A value states "no string
	// in this data is longer than n" — SQL Server rejects anything longer, and
	// the extension's own guard catches it before the batch is sent. Above the
	// inline limit (4000 for NVARCHAR, 8000 for VARCHAR) the column stays MAX.
	//
	// Per-column control is a cast — MSSQL_NVARCHAR(n) / MSSQL_VARCHAR(n) — and
	// beats this for anything but a uniform schema.
	config.AddExtensionOption("mssql_default_string_length",
							  "Length for an unannotated VARCHAR column created by CTAS/COPY (0 = MAX, the default)",
							  LogicalType::BIGINT, Value::BIGINT(0), nullptr, SetScope::GLOBAL);

	// mssql_utf8_collation - collation for VARCHAR targets (issue #225).
	// Only consulted when mssql_ctas_text_type is VARCHAR and the server granted
	// UTF8SUPPORT at login. The default is BIN2 — binary comparison, no
	// linguistic rules, so case- and accent-SENSITIVE — because that is what
	// Fabric Warehouse defaults to, and a binary collation has no locale to be
	// wrong about (spec 064 D0). Users who want linguistic matching set a CI_AS
	// UTF-8 collation here or per column via MSSQL_VARCHAR(n, 'collation'); the
	// collation is stored in the schema and governs every later comparison
	// against the column. Empty means "add no COLLATE clause", which lets the
	// column inherit the database default — right when that default is already
	// UTF-8, and the way back to the pre-#225 behaviour otherwise. The name is
	// not validated here: SQL Server rejects an unknown collation by name, which
	// is a clearer error than anything this could say.
	config.AddExtensionOption(
		"mssql_utf8_collation",
		"Collation given to VARCHAR columns created by CTAS when the server supports UTF-8 "
		"(default: Latin1_General_100_BIN2_UTF8, matching Fabric; empty inherits the database default)",
		LogicalType::VARCHAR, Value(mssql::MSSQL_DEFAULT_UTF8_COLLATION), ValidateCollationName, SetScope::GLOBAL);

	// mssql_catalog_native_types - report MSSQL_VARCHAR(n) / MSSQL_NVARCHAR(n) for
	// the string columns of attached tables instead of a bare VARCHAR (spec 060).
	// On by default, because it is what lets a target inherit its source's
	// declared lengths with no cast written by anyone: COPY (SELECT * FROM
	// d.dbo.Src) TO 'd.dbo.Dst' reproduces Src's column types. The cost is that
	// DESCRIBE and duckdb_columns() print the annotated name, so tooling that
	// matches on the literal string "VARCHAR" sees something new — that is what
	// this setting turns off. The values are ordinary DuckDB strings either way,
	// and n constrains nothing on the DuckDB side.
	//
	// Read when a table entry is built, so a change applies to entries loaded
	// afterwards; call mssql_invalidate_cache() to apply it to cached ones.
	config.AddExtensionOption("mssql_catalog_native_types",
							  "Report MSSQL_VARCHAR(n)/MSSQL_NVARCHAR(n) for attached string columns instead of "
							  "VARCHAR (default: true)",
							  LogicalType::BOOLEAN, Value::BOOLEAN(true), nullptr, SetScope::GLOBAL);

	// mssql_default_table_kind - shape of a table CTAS/COPY creates (spec 060 D9).
	// HEAP is SQL Server's own default and what the extension has always made.
	// COLUMNSTORE creates a clustered columnstore index right after the table,
	// which is where the large storage win lives for analytic loads; it is a
	// separate DDL statement because a columnstore index cannot be declared
	// inside CREATE TABLE. Per-statement control is the COPY table_kind option
	// or CREATE TABLE ... WITH (table_kind = '...').
	config.AddExtensionOption("mssql_default_table_kind",
							  "Shape of a table created by CTAS/COPY: HEAP (default) or COLUMNSTORE",
							  LogicalType::VARCHAR, Value("HEAP"), nullptr, SetScope::GLOBAL);

	// mssql_ctas_use_bcp - Use BCP protocol for CTAS data transfer
	// BCP is 2-10x faster than batched INSERT statements
	config.AddExtensionOption("mssql_ctas_use_bcp",
							  "Use BCP protocol for CTAS data transfer (default: true, 2-10x faster than INSERT)",
							  LogicalType::BOOLEAN, Value::BOOLEAN(DEFAULT_CTAS_USE_BCP), nullptr, SetScope::GLOBAL);

	//===----------------------------------------------------------------------===//
	// COPY/BCP Settings
	//===----------------------------------------------------------------------===//

	// mssql_copy_flush_rows - Rows before flushing to SQL Server
	// Controls memory usage on both DuckDB and SQL Server sides
	// 0 = no intermediate flushes (WARNING: high memory usage for large datasets)
	config.AddExtensionOption(
		"mssql_copy_flush_rows",
		"Rows per bulk-load batch sent to SQL Server (default: 102400 — SQL Server's threshold for writing "
		"compressed columnstore rowgroups directly; 0 = one batch at the end, high memory)",
		LogicalType::BIGINT, Value::BIGINT(MSSQL_DEFAULT_COPY_FLUSH_ROWS), ValidateNonNegative, SetScope::GLOBAL);

	// mssql_copy_parallel_writers — concurrent bulk-load connections per COPY
	// (spec 057 step 7). 0 derives it from DuckDB's thread count, capped.
	config.AddExtensionOption(
		"mssql_copy_parallel_writers",
		"Concurrent bulk-load connections a single COPY/CTAS may open (default: 0 = derive from DuckDB's "
		"thread count, capped at 8). 1 disables parallel loading. Ignored inside an explicit transaction, "
		"where the connection is pinned",
		LogicalType::BIGINT, Value::BIGINT(MSSQL_DEFAULT_COPY_PARALLEL_WRITERS), ValidateNonNegative, SetScope::GLOBAL);

	// mssql_copy_tablock — 'auto' | 'true' | 'false' (spec 057 step 1).
	//
	// Tri-state, and it has to be: the previous BOOLEAN could not express "the
	// user did not choose". Both auto-TABLOCK sites read `tablock_explicit`,
	// which was set from `TryGetCurrentSetting(...)` succeeding — and that
	// succeeds unconditionally for an option with a registered default, so the
	// flag was ALWAYS true and both auto branches were dead code from spec 030
	// (issue #45) until now. INSERT BULK went out with no TABLOCK on every
	// create-then-load, contradicting the documented behaviour.
	//
	// 'auto' decides from the target's shape, which is what the measurements
	// support and "newness" never did:
	//   heap                  -> ON  (4 concurrent loaders: 1.70 s vs 11.97 s;
	//                                 BU locks are mutually compatible)
	//   anything clustered    -> OFF (TABLOCK serialises the loaders — rowstore
	//                                 2.11 s/M with vs 1.20 s/M without;
	//                                 columnstore 8.92 s vs 5.23 s on 2M rows,
	//                                 the server pinned to ONE core with the hint
	//                                 and spread over three without it, and the
	//                                 compression identical either way)
	// Magnitudes are from an emulated server and are upper bounds; the
	// directions have mechanisms and hold.
	//
	// The columnstore half read ON until spec 057 step 7, on the guess that it
	// behaves like a heap. Nothing could test that while only one session ever
	// loaded; making the loads concurrent showed the opposite.
	//
	// `SET mssql_copy_tablock = true` still works — DuckDB casts the boolean to
	// this option's VARCHAR.
	config.AddExtensionOption("mssql_copy_tablock",
							  "TABLOCK hint for COPY/BCP: 'auto' (by target shape — heap on, anything "
							  "clustered off), 'true', or 'false'",
							  LogicalType::VARCHAR, Value("auto"), nullptr, SetScope::GLOBAL);

	//===----------------------------------------------------------------------===//
	// VARCHAR Encoding Settings (Spec 026)
	//===----------------------------------------------------------------------===//

	//===----------------------------------------------------------------------===//
	// ORDER BY Pushdown Settings (Spec 039)
	//===----------------------------------------------------------------------===//

	// mssql_order_pushdown - Enable ORDER BY pushdown to SQL Server
	// When enabled, ORDER BY clauses on simple column references and supported functions
	// are pushed to SQL Server. Disabled by default for backward compatibility.
	config.AddExtensionOption("mssql_order_pushdown", "Enable ORDER BY pushdown to SQL Server (default: false)",
							  LogicalType::BOOLEAN, Value::BOOLEAN(false), nullptr, SetScope::GLOBAL);

	// mssql_convert_varchar_max - Convert VARCHAR(MAX) to NVARCHAR(MAX) in table scans
	// When true: VARCHAR(MAX) with non-UTF8 collation is wrapped in CAST(... AS NVARCHAR(MAX))
	// When false: VARCHAR(MAX) is NOT converted (preserves 4096-byte TDS buffer capacity)
	// Note: This only applies to catalog table scans, not mssql_scan() raw queries
	config.AddExtensionOption(
		"mssql_convert_varchar_max",
		"Convert VARCHAR(MAX) to NVARCHAR(MAX) in table scans for UTF-8 compatibility (default: true)",
		LogicalType::BOOLEAN, Value::BOOLEAN(DEFAULT_CONVERT_VARCHAR_MAX), nullptr, SetScope::GLOBAL);
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

	if (context.TryGetCurrentSetting("mssql_query_timeout", val)) {
		config.query_timeout = static_cast<int>(val.GetValue<int64_t>());
	}

	if (context.TryGetCurrentSetting("mssql_metadata_timeout", val)) {
		config.metadata_timeout = static_cast<int>(val.GetValue<int64_t>());
	}

	if (context.TryGetCurrentSetting("mssql_login7_max_packet", val)) {
		config.login7_max_packet = val.GetValue<int64_t>();
	}

	if (context.TryGetCurrentSetting("mssql_tds_packet_size", val)) {
		config.tds_packet_size = val.GetValue<int64_t>();
	}

	if (context.TryGetCurrentSetting("mssql_utf8_support", val)) {
		config.utf8_support = val.GetValue<bool>();
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

int LoadQueryTimeout(ClientContext &context) {
	Value val;
	if (context.TryGetCurrentSetting("mssql_query_timeout", val)) {
		return static_cast<int>(val.GetValue<int64_t>());
	}
	return tds::DEFAULT_QUERY_TIMEOUT;	// Default: 30 seconds
}

int LoadMetadataTimeout(ClientContext &context) {
	Value val;
	if (context.TryGetCurrentSetting("mssql_metadata_timeout", val)) {
		return static_cast<int>(val.GetValue<int64_t>());
	}
	return tds::DEFAULT_METADATA_TIMEOUT;  // Default: 300 seconds
}

// Spec 047 (US2): timeout for the eager ATTACH-time credential round trip.
// 0 ⇒ fall back to mssql_connection_timeout (the steady-state ceiling).
int LoadAttachValidationTimeout(ClientContext &context) {
	Value val;
	if (context.TryGetCurrentSetting("mssql_attach_validation_timeout", val)) {
		auto seconds = static_cast<int>(val.GetValue<int64_t>());
		if (seconds > 0) {
			return seconds;
		}
	}
	if (context.TryGetCurrentSetting("mssql_connection_timeout", val)) {
		return static_cast<int>(val.GetValue<int64_t>());
	}
	return tds::DEFAULT_CONNECTION_TIMEOUT;
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

//===----------------------------------------------------------------------===//
// CTAS Configuration Loading
//===----------------------------------------------------------------------===//

namespace mssql {

CTASTextType CTASConfig::ParseTextType(const string &text_type_str) {
	auto upper = StringUtil::Upper(text_type_str);
	if (upper == "NVARCHAR") {
		return CTASTextType::NVARCHAR;
	} else if (upper == "VARCHAR") {
		return CTASTextType::VARCHAR;
	}
	throw InvalidInputException("Invalid mssql_ctas_text_type: '%s'. Must be 'NVARCHAR' or 'VARCHAR'", text_type_str);
}

CTASConfig CTASConfig::Load(ClientContext &context) {
	return LoadCTASConfig(context);
}

CTASConfig LoadCTASConfig(ClientContext &context) {
	CTASConfig config;
	Value val;

	// Load drop_on_failure setting
	if (context.TryGetCurrentSetting("mssql_ctas_drop_on_failure", val)) {
		config.drop_on_failure = val.GetValue<bool>();
	}

	// Load text_type setting
	if (context.TryGetCurrentSetting("mssql_ctas_text_type", val)) {
		config.text_type = CTASConfig::ParseTextType(val.ToString());
	}

	// Load the UTF-8 collation for VARCHAR targets (issue #225)
	if (context.TryGetCurrentSetting("mssql_utf8_collation", val)) {
		config.utf8_collation = val.IsNull() ? string() : val.ToString();
	}

	// Spec 060 D9: shape of the table CTAS creates
	config.table_options = MSSQLTableOptions::FromSettings(context);

	// Spec 060 D9: default length for an unannotated VARCHAR column
	if (context.TryGetCurrentSetting("mssql_default_string_length", val)) {
		const int64_t raw = val.IsNull() ? 0 : val.GetValue<int64_t>();
		config.default_string_length = raw <= 0 ? 0 : static_cast<int32_t>(std::min<int64_t>(raw, INT32_MAX));
	}

	// Inherit INSERT settings for batch insert phase (when use_bcp = false)
	if (context.TryGetCurrentSetting("mssql_insert_batch_size", val)) {
		config.batch_size = static_cast<idx_t>(val.GetValue<int64_t>());
	}

	if (context.TryGetCurrentSetting("mssql_insert_max_rows_per_statement", val)) {
		config.max_rows_per_statement = static_cast<idx_t>(val.GetValue<int64_t>());
	}

	if (context.TryGetCurrentSetting("mssql_insert_max_sql_bytes", val)) {
		config.max_sql_bytes = static_cast<idx_t>(val.GetValue<int64_t>());
	}

	//===----------------------------------------------------------------------===//
	// BCP Mode Settings (Spec 027)
	//===----------------------------------------------------------------------===//

	// Load use_bcp setting (default: true)
	if (context.TryGetCurrentSetting("mssql_ctas_use_bcp", val)) {
		config.use_bcp = val.GetValue<bool>();
	}

	// Inherit BCP settings from COPY configuration
	if (context.TryGetCurrentSetting("mssql_copy_flush_rows", val)) {
		config.bcp_flush_rows = static_cast<idx_t>(val.GetValue<int64_t>());
	}

	if (context.TryGetCurrentSetting("mssql_copy_tablock", val)) {
		// Tri-state — see BCPCopyConfig's peer in bcp_config.cpp. The success of
		// this call says nothing about whether the user chose; the value does.
		config.bcp_tablock_choice = MSSQLParseTablockChoice(val.IsNull() ? string() : val.ToString());
	}

	return config;
}

}  // namespace mssql

//===----------------------------------------------------------------------===//
// VARCHAR Encoding Configuration Loading (Spec 026)
//===----------------------------------------------------------------------===//

bool LoadConvertVarcharMax(ClientContext &context) {
	Value val;
	if (context.TryGetCurrentSetting("mssql_convert_varchar_max", val)) {
		return val.GetValue<bool>();
	}
	return DEFAULT_CONVERT_VARCHAR_MAX;
}

//===----------------------------------------------------------------------===//
// CTAS BCP Configuration Loading (Spec 027)
//===----------------------------------------------------------------------===//

bool LoadCTASUseBCP(ClientContext &context) {
	Value val;
	if (context.TryGetCurrentSetting("mssql_ctas_use_bcp", val)) {
		return val.GetValue<bool>();
	}
	return DEFAULT_CTAS_USE_BCP;
}

bool LoadExecInvalidateCache(ClientContext &context) {
	Value val;
	if (context.TryGetCurrentSetting("mssql_exec_invalidate_cache", val)) {
		return val.GetValue<bool>();
	}
	return false;  // Default: do NOT auto-invalidate; manual invalidation like postgres_execute (#151)
}

}  // namespace duckdb
