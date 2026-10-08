#include "catalog/mssql_metadata_cache.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unordered_set>
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "query/mssql_simple_query.hpp"
#include "query/mssql_sql_params.hpp"

// Debug logging for metadata cache operations
static int GetMetadataCacheDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
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

// PHYSICAL SHAPE COMES FROM sys.indexes, NOT sys.partitions (spec 071 W1).
//
// Every query below used to carry one pre-aggregated sys.partitions subquery that
// produced approx_rows, index_type and partition_count together. It was there for
// a real reason — sys.partitions holds one row PER PARTITION, so joining it raw
// multiplied every object and every column by the partition count, which is issue
// #85 ("Column with name <x> already exists!") — but the aggregate it was replaced
// with turned out to be the single most expensive thing in the catalog layer.
//
// sys.partitions reads sys.sysrowsets, which is clustered on rowsetid; object_id
// is derived and has no index. Measured on a 200,000-table catalog, a lookup BY
// object_id costs:
//
//     sys.partitions              sysrowsets   3200 logical reads (2 scans)
//     sys.dm_db_partition_stats   sysrowsets   1600 logical reads (1 scan)
//     sys.indexes                 sysidxstats     6 logical reads (SEEK)
//
// So the row count is O(catalog) however it is asked for and the index shape is
// O(1); they had no business sharing a subquery. Worse, what forced the GROUP BY
// was COUNT(*) AS partition_count — a value nothing in the tree ever read.
//
// The shape now comes from sys.indexes (seekable; a LEFT JOIN on index 0/1, not
// hypothetical, since spec 084), with "is this partitioned" answered by
// sys.partition_schemes rather than by counting partitions. index_id IN (0, 1) still selects the heap (0) or the
// clustered index (1); a table has exactly one of the two.
//
// Row counts come from OBJECTPROPERTYEX(object_id, 'Cardinality'), which answers
// out of object metadata and needs neither sys.partitions nor statistics to
// exist on the table. Measured against sys.partitions on the same objects it
// agrees exactly, including 0 for an empty table and NULL (-> 0) for a VIEW,
// which is the value the catalog already carried for a view. What it costs
// depends on the catalog: spec 071 measured 3 logical reads a call (84 ms for
// every count in the database) on tables without keys in one schema; spec 084
// measured ~110 a call (23 s for 200k objects) on tables with keys and
// constraints in 100 schemas. So it is called once per OBJECT, never per column
// row, and the whole-catalog load takes its counts from one pass over
// sys.dm_db_partition_stats instead (BuildBulkMetadataBatch).
//
// Dropping the count instead was tried and rejected: it is what gives the
// planner its cardinality (MSSQLCatalogScanCardinality reads the catalog's copy
// first), and scan_cardinality.test catches its loss. sys.sysindexes is equally
// cheap and was rejected as deprecated; sys.dm_db_stats_properties is cheap but
// returns nothing for a table with no statistics — which is exactly a table this
// extension has just created by CTAS or COPY.
//
// See specs/071-catalog-metadata-cost/spec.md.

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

// Issue #412: the names of every schema's tables and views, nothing else -- for
// DuckDB's "did you mean" hint, which reads entry names only.
//
// The schema's name by a join, not SCHEMA_NAME() per object: the query passes
// over every object in the database, and the per-row call is most of its CPU
// (measured on 200k objects in 100 schemas: 680-700 ms -> 297 ms; the result
// is identical, compared row by row on 7.5k objects).
static const char *TABLE_NAMES_SQL = R"(
SELECT s.name AS schema_name, o.name AS object_name
FROM sys.objects o
INNER JOIN sys.schemas s ON s.schema_id = o.schema_id
WHERE o.type IN ('U', 'V')
  AND o.is_ms_shipped = 0)";

// Single-table metadata, first statement of the batch: the OBJECT row -- type,
// row count, physical shape -- for ONE table. Its columns are the second
// statement (COLUMN_DISCOVERY_SQL_TEMPLATE) and its keys the third.
//
// Spec 084 F2/F5: the row count used to sit in the same SELECT list as the
// columns, and the server ran OBJECTPROPERTYEX once per COLUMN row, ~110 logical
// reads a call on a catalog with keys and constraints: 1.86 ms / 986 reads for
// one table of a 200k-object catalog, against 0.18 + 0.21 ms / 42 + 8 reads as
// two statements.
static const char *SINGLE_TABLE_OBJECT_SQL_TEMPLATE = R"(
SELECT
    o.type AS object_type,
    CAST(ISNULL(OBJECTPROPERTYEX(o.object_id, 'Cardinality'), 0) AS BIGINT) AS approx_rows,
    ISNULL(i.type, 0) AS index_type,
    CASE WHEN ps.data_space_id IS NULL THEN 0 ELSE 1 END AS is_partitioned
FROM sys.objects o
LEFT JOIN sys.indexes i ON i.object_id = o.object_id AND i.index_id IN (0, 1) AND i.is_hypothetical = 0
LEFT JOIN sys.partition_schemes ps ON ps.data_space_id = i.data_space_id
WHERE o.object_id = OBJECT_ID(QUOTENAME(@s) + N'.' + QUOTENAME(@t))
)";

// The bulk loads (spec 071 W2, spec 084 D1/D5): one batch, three result sets --
// the OBJECTS (one row each: object_id, schema, name, type, row count, shape)
// and their COLUMNS (object_id, then the column fields) -- grouped on object_id
// client-side. Two things this shape is for:
// - the row count is computed once per object. Beside the columns, the server
//   ran OBJECTPROPERTYEX once per COLUMN row: a schema of 2,000 tables in a
//   200k-object catalog took 1.8 s, the whole catalog 170 s;
// - the object fields are not repeated on every column row.
// No ORDER BY: rows are grouped by object_id in a hash map and each table's
// columns sorted by column_id before publishing (sorting millions of rows on
// the server overran the memory grant -- the reason the per-schema loop
// existed, spec 071 W2).
//
// `scope` is appended to all three statements' WHERE: the one schema
// (`s.schema_id = SCHEMA_ID(@s)`, not a per-object SCHEMA_NAME(), spec 084 F3)
// and the LIKE forms of schema_filter / table_filter.
//
// `rc_pass`: the row counts of the whole catalog from ONE pass over
// sys.dm_db_partition_stats into @rc (4.3 s at 200k objects against 23 s of
// per-object OBJECTPROPERTYEX, ~110 logical reads a call on a catalog with keys
// and constraints). Only for a whole-catalog load the server does not narrow
// -- the pass covers the whole database whatever the filters say -- and only
// where it is allowed (not Fabric / Synapse). The DMV needs VIEW DATABASE
// STATE, which OBJECTPROPERTYEX does not: without it, or on any error, @rc
// stays empty and every object takes the OBJECTPROPERTYEX branch, so a
// db_datareader-only login keeps its catalog. A view has no partition rows and takes that branch too; an
// indexed view agrees either way (measured), a memory-optimized table has no
// index 0/1 row and reads 0 from OBJECTPROPERTYEX as it always did. A deadlock
// victim (1205) is rethrown, for RunMetadataQuerySets to rerun the batch.
static const char *BULK_SCHEMA_EXCLUSIONS = R"(s.schema_id NOT IN (3, 4)
  AND s.principal_id != 0
  AND s.name NOT IN ('guest', 'INFORMATION_SCHEMA', 'sys', 'db_owner', 'db_accessadmin',
                     'db_securityadmin', 'db_ddladmin', 'db_backupoperator', 'db_datareader',
                     'db_datawriter', 'db_denydatareader', 'db_denydatawriter')
  AND o.type IN ('U', 'V')
  AND o.is_ms_shipped = 0)";

static string BuildBulkMetadataBatch(const string &scope, bool rc_pass) {
	string batch;
	string row_count = "CAST(ISNULL(OBJECTPROPERTYEX(o.object_id, 'Cardinality'), 0) AS BIGINT)";
	string rc_join;
	if (rc_pass) {
		batch += R"(DECLARE @rc TABLE (object_id int PRIMARY KEY, rows_ bigint);
IF HAS_PERMS_BY_NAME(DB_NAME(), 'DATABASE', 'VIEW DATABASE STATE') = 1
BEGIN TRY
    INSERT INTO @rc (object_id, rows_)
    SELECT ps.object_id, SUM(ps.row_count)
    FROM sys.dm_db_partition_stats ps
    WHERE ps.index_id IN (0, 1)
    GROUP BY ps.object_id;
END TRY
BEGIN CATCH
    IF ERROR_NUMBER() = 1205 THROW;
END CATCH;
)";
		row_count = "CASE WHEN rc.object_id IS NOT NULL THEN rc.rows_ ELSE " + row_count + " END";
		rc_join = "LEFT JOIN @rc rc ON rc.object_id = o.object_id\n";
	}
	batch += "SELECT o.object_id, s.name AS schema_name, o.name AS object_name, o.type AS object_type,\n    " +
			 row_count +
			 " AS approx_rows,\n"
			 "    ISNULL(i.type, 0) AS index_type,\n"
			 "    CASE WHEN ps.data_space_id IS NULL THEN 0 ELSE 1 END AS is_partitioned\n"
			 "FROM sys.schemas s\n"
			 "INNER JOIN sys.objects o ON o.schema_id = s.schema_id\n" +
			 rc_join +
			 "LEFT JOIN sys.indexes i ON i.object_id = o.object_id AND i.index_id IN (0, 1) AND i.is_hypothetical = 0\n"
			 "LEFT JOIN sys.partition_schemes ps ON ps.data_space_id = i.data_space_id\n"
			 "WHERE " +
			 string(BULK_SCHEMA_EXCLUSIONS) + scope + ";\n";
	batch += R"(SELECT c.object_id,
    c.name AS column_name,
    c.column_id,
    ISNULL(TYPE_NAME(c.system_type_id), TYPE_NAME(c.user_type_id)) AS type_name,
    c.max_length,
    c.precision,
    c.scale,
    c.is_nullable,
    ISNULL(c.collation_name, '') AS collation_name,
    c.is_identity,
    CAST(COLLATIONPROPERTY(c.collation_name, 'CodePage') AS INT) AS code_page
FROM sys.schemas s
INNER JOIN sys.objects o ON o.schema_id = s.schema_id
INNER JOIN sys.columns c ON c.object_id = o.object_id
WHERE )" + string(BULK_SCHEMA_EXCLUSIONS) +
			 scope + ";\n";
	// Spec 084 D5: every object's usable unique keys, so a table a bulk load
	// published carries its rowid key (pk_loaded) and a transaction does not
	// discover it again per table on the pinned connection (measured: 22 key
	// queries over 15 transactions after a preload, 2 without). 3.1-3.4 s over a
	// 200k-object catalog, 57 ms for one schema of 2,000.
	batch += mssql::RowIdKeyInfo::BulkDiscoverySql(
		"FROM sys.schemas s\n"
		"INNER JOIN sys.objects o ON o.schema_id = s.schema_id\n"
		"INNER JOIN sys.indexes i ON i.object_id = o.object_id",
		string(BULK_SCHEMA_EXCLUSIONS) + scope);
	return batch;
}

// Query to discover columns in a table/view
// Note: ISNULL is used for collation_name to avoid NBCROW parsing issues with NULL values
static const char *COLUMN_DISCOVERY_SQL_TEMPLATE = R"(
SELECT
    c.name AS column_name,
    c.column_id,
    ISNULL(TYPE_NAME(c.system_type_id), TYPE_NAME(c.user_type_id)) AS type_name,
    c.max_length,
    c.precision,
    c.scale,
    c.is_nullable,
    ISNULL(c.collation_name, '') AS collation_name,
    c.is_identity,
    CAST(COLLATIONPROPERTY(c.collation_name, 'CodePage') AS INT) AS code_page
FROM sys.columns c
WHERE c.object_id = OBJECT_ID(QUOTENAME(@s) + N'.' + QUOTENAME(@t))
ORDER BY c.column_id
)";

// COLLATIONPROPERTY(name, 'CodePage') as the server sends it: an int, NULL for a
// non-text column (and for a collation the server has no page for, which
// then declares as nvarchar — the safe side).
static int32_t ParseCodePage(const vector<string> &values, idx_t idx) {
	if (values.size() <= idx || values[idx].empty()) {
		return 0;
	}
	try {
		return static_cast<int32_t>(std::stoi(values[idx]));
	} catch (...) {
		return 0;
	}
}

// A bit column as the simple-query layer renders it.
static bool FlagIsSet(const string &value) {
	return value == "1" || value == "true" || value == "True";
}

// One column out of a metadata row: column_name, column_id, type_name,
// max_length, precision, scale, is_nullable, collation_name at values[base] ..
// values[base + 7], and is_identity / code_page where the query puts them (the
// bulk queries carry the table's shape between collation_name and is_identity).
static MSSQLColumnInfo ColumnFromRow(const vector<string> &values, idx_t base, idx_t identity_idx, idx_t code_page_idx,
									 const string &database_collation, int32_t database_code_page) {
	int32_t col_id = 0;
	try {
		col_id = static_cast<int32_t>(std::stoi(values[base + 1]));
	} catch (...) {
	}
	int16_t max_len = 0;
	try {
		max_len = static_cast<int16_t>(std::stoi(values[base + 3]));
	} catch (...) {
	}
	uint8_t prec = 0;
	try {
		prec = static_cast<uint8_t>(std::stoi(values[base + 4]));
	} catch (...) {
	}
	uint8_t scl = 0;
	try {
		scl = static_cast<uint8_t>(std::stoi(values[base + 5]));
	} catch (...) {
	}
	MSSQLColumnInfo col_info(values[base], col_id, values[base + 2], max_len, prec, scl, FlagIsSet(values[base + 6]),
							 values[base + 7], database_collation);
	// sys.columns.is_identity (spec 062 W4, issue #327): what the INSERT planner
	// needs to keep an explicit identity value on the statement path.
	col_info.is_identity = values.size() > identity_idx && FlagIsSet(values[identity_idx]);
	// COLLATIONPROPERTY(collation_name, 'CodePage') (issue #361): the page a
	// varchar parameter is converted to; 0 when the server cannot say.
	col_info.code_page = ParseCodePage(values, code_page_idx);
	col_info.database_code_page = database_code_page;
	return col_info;
}

//===----------------------------------------------------------------------===//
// Physical shape of the object, parsed out of the correlated sys.indexes lookup.
// index_type and is_partitioned sit after the per-column fields in every
// SELECT list (is_identity follows them since spec 062 W4), so each caller
// passes their own indices.
//
// The two values are parsed into locals and published together: a malformed
// is_partitioned must not discard an index_type that parsed fine.
//===----------------------------------------------------------------------===//

static void ParseTableShape(const vector<string> &values, idx_t index_type_idx, idx_t is_partitioned_idx,
							MSSQLTableMetadata &table_meta) {
	const MSSQLIndexKind kind = MSSQLIndexKindFromSysIndexesType(values[index_type_idx]);
	// The server sends the CASE result, so anything but "1" is "not partitioned"
	// — including a value that is missing or unparseable, which is the honest
	// answer when the shape lookup produced nothing.
	const bool partitioned = values[is_partitioned_idx] == "1";
	table_meta.index_kind = kind;
	table_meta.is_partitioned = partitioned;
}

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

using MetadataSetRowCallback = std::function<void(idx_t result_set, const vector<string> &values)>;

static void RunMetadataQuerySets(tds::TdsConnection &connection, const tds::Request &sql,
								 MetadataSetRowCallback callback, int timeout_ms, const std::function<void()> &reset);

static void RunMetadataQuery(tds::TdsConnection &connection, const tds::Request &sql, MetadataRowCallback callback,
							 int timeout_ms, const std::function<void()> &reset) {
	RunMetadataQuerySets(
		connection, sql, [&callback](idx_t, const vector<string> &values) { callback(values); }, timeout_ms, reset);
}

static void RunMetadataQuerySets(tds::TdsConnection &connection, const tds::Request &sql,
								 MetadataSetRowCallback callback, int timeout_ms, const std::function<void()> &reset) {
	// Log the query being executed (truncated for readability)
	CACHE_DEBUG(1, "RunMetadataQuery: timeout=%dms, sql=%.120s%s", timeout_ms, sql.sql.c_str(),
				sql.sql.size() > 120 ? "..." : "");

	// Deadlock victim (server error 1205) is retried: metadata queries are pure
	// reads and the server's own message says "Rerun the transaction". DuckDB
	// 2.0's higher scan/sink parallelism overlaps a test's DDL (Sch-M) with
	// catalog loads on sibling pooled connections often enough to make 1205 a
	// per-run event (PR #267 review).
	//
	// The rerun used to be allowed ONLY before the first row, because after it
	// the callback holds state a second pass would duplicate ("Column with name
	// x already exists!"). That guard made the retry useless exactly where 1205
	// actually lands: the bulk catalog query streams one row per COLUMN of every
	// table, so by the time two sessions collide it has almost always delivered
	// rows. `reset` closes that — the caller undoes its partial accumulation and
	// the query is re-run from the top.
	//
	// Note what this is and is not. It does not stop the deadlock; the cycle is
	// between our multi-table catalog join (holding a key in sys.sysschobjs,
	// wanting sys.sysrowsets) and a concurrent DROP TABLE holding the reverse,
	// and no client-side isolation setting reaches it — catalog reads ignore the
	// session isolation level. This only makes losing the race recoverable.
	constexpr int MAX_ATTEMPTS = 6;
	auto start = std::chrono::steady_clock::now();
	for (int attempt = 1;; attempt++) {
		idx_t rows_delivered = 0;
		auto result = MSSQLSimpleQuery::ExecuteWithSetCallback(
			connection, sql,
			[&callback, &rows_delivered](idx_t result_set, const std::vector<std::string> &row) {
				// Convert std::vector to duckdb::vector
				vector<string> duckdb_row;
				duckdb_row.reserve(row.size());
				for (const auto &val : row) {
					duckdb_row.push_back(val);
				}
				rows_delivered++;
				callback(result_set, duckdb_row);
				return true;  // continue processing
			},
			timeout_ms);

		auto elapsed =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

		if (!result.HasError()) {
			CACHE_DEBUG(1, "RunMetadataQuery: completed in %lldms", (long long)elapsed);
			return;
		}
		if (result.error_number == 1205 && attempt < MAX_ATTEMPTS && (rows_delivered == 0 || reset)) {
			CACHE_DEBUG(1, "RunMetadataQuery: deadlock victim (1205) after %llu row(s), attempt %d/%d — rerunning",
						(unsigned long long)rows_delivered, attempt, MAX_ATTEMPTS);
			if (rows_delivered > 0) {
				reset();
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(150 * attempt));
			continue;
		}
		CACHE_DEBUG(1, "RunMetadataQuery: FAILED after %lldms — %s", (long long)elapsed, result.error_message.c_str());
		throw IOException("Metadata query failed: %s", result.error_message);
	}
}

//===----------------------------------------------------------------------===//
// Constructor
//===----------------------------------------------------------------------===//

MSSQLMetadataCache::MSSQLMetadataCache(int64_t ttl_seconds)
	: state_(MSSQLCacheState::EMPTY),
	  ttl_seconds_(ttl_seconds),
	  // Pre-ATTACH placeholder only: EnsureCacheLoaded / RefreshCache overwrite
	  // this from the mssql_metadata_timeout setting on every catalog lookup.
	  metadata_timeout_ms_(tds::DEFAULT_METADATA_TIMEOUT * 1000) {}

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

	std::lock_guard<std::mutex> lock(mutex_);
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

bool MSSQLMetadataCache::GetTableMetadata(tds::TdsConnection &connection, const string &schema_name,
										  const string &table_name, MSSQLTableMetadata &out_meta) {
	// Only load schemas (fast — just schema names, no tables)
	EnsureSchemasLoaded(connection);

	std::lock_guard<std::mutex> lock(mutex_);

	// Ensure schema entry exists
	auto schema_it = schemas_.find(schema_name);
	if (schema_it == schemas_.end()) {
		CACHE_DEBUG(1, "GetTableMetadata('%s.%s') — schema not found", schema_name.c_str(), table_name.c_str());
		return false;
	}

	auto &schema = schema_it->second;

	// Check if table already cached with columns loaded
	auto table_it = schema.tables.find(table_name);
	if (table_it != schema.tables.end() && IsColumnsFreshLocked(table_it->second)) {
		CACHE_DEBUG(2, "GetTableMetadata('%s.%s') — cache hit (%zu columns)", schema_name.c_str(), table_name.c_str(),
					table_it->second.columns.size());
		out_meta = table_it->second;  // copy under mutex_ — see header contract
		return true;
	}

	// Single-table batch: object row, columns and keys in one round trip
	CACHE_DEBUG(1, "GetTableMetadata('%s.%s') — loading from SQL Server (one batch)", schema_name.c_str(),
				table_name.c_str());

	// Spec 075 W4 (#334): the names travel as sp_executesql parameters, so the
	// text -- and the server's cached plan -- is the same for every table.
	// Spec 076 W2: the primary key in the same batch, off the same @s / @t, so a
	// fresh table pays one round trip. Spec 084 D1: three result sets -- the
	// object row, its columns, its keys -- so the row count is computed once,
	// not once per column.
	const tds::Request query_request =
		mssql::BuildExecuteSqlRequest(string(SINGLE_TABLE_OBJECT_SQL_TEMPLATE) + ";\n" + COLUMN_DISCOVERY_SQL_TEMPLATE +
										  ";\n" + mssql::RowIdKeyInfo::DiscoverySqlTemplate(),
									  "@s sysname, @t sysname", {{"s", schema_name}, {"t", table_name}});

	// Populate the cache slot in place so we never take the address of a stack
	// local that gets moved out. GCC's -Wreturn-local-addr can't prove the
	// lambda's `&table_meta` capture doesn't escape via the std::function
	// inside ExecuteMetadataQuery, so it flagged this as a false-positive
	// even though both return paths returned addresses of map elements (post
	// std::move). Building directly in the map sidesteps the analysis and
	// avoids one move per call.
	//
	// C++11-compatible: erase any stale entry first, then emplace a fresh
	// default-constructed one. Schema mutex is held for the duration so
	// partial state isn't visible to other threads.
	// (NOTE: insert_or_assign is C++17; the project targets C++11 for ODR
	//  compat with DuckDB.)
	schema.tables.erase(table_name);
	auto insert_result = schema.tables.emplace(table_name, MSSQLTableMetadata());
	auto slot_it = insert_result.first;
	MSSQLTableMetadata &table_meta = slot_it->second;
	table_meta.name = table_name;

	bool have_object = false;
	// Restartable: the slot was created empty by the erase/emplace above, so
	// putting it back to that state is the whole undo.
	auto reset_slot = [&table_meta, &table_name, &have_object]() {
		table_meta = MSSQLTableMetadata();
		table_meta.name = table_name;
		have_object = false;
	};
	// A failure that is not retried must not leave the half-filled slot in the
	// shared map: a concurrent listing would copy it and build an entry with
	// partial columns and no key (review of spec 084 step 4). The slot was this
	// call's own, so erasing it is the whole undo.
	try {
		ExecuteMetadataQuerySets(
			connection, query_request,
			[this, &table_meta, &have_object](idx_t result_set, const vector<string> &values) {
				// Routed by which statement of the batch produced the row, never by
				// its width (review of #345 -- a column added to one query must not
				// silently land in another's parser).
				if (result_set == 0) {
					// The object row: object_type, approx_rows, index_type,
					// is_partitioned. approx_rows has to stay --
					// MSSQLCatalogScanCardinality reads the catalog's copy before
					// anything else, so removing it plans every direct query at ~1 row.
					if (values.size() < 4) {
						return;
					}
					have_object = true;
					table_meta.object_type =
						(!values[0].empty() && values[0][0] == 'V') ? MSSQLObjectType::VIEW : MSSQLObjectType::TABLE;
					try {
						table_meta.approx_row_count = static_cast<idx_t>(std::stoll(values[1]));
					} catch (...) {
						table_meta.approx_row_count = 0;
					}
					// values[2] is sys.indexes.type -- 1 clustered rowstore, 5 clustered
					// COLUMNSTORE, 0 heap -- and values[3] says whether the object sits
					// on a partition scheme. Both drive the write path's TABLOCK and
					// sort decisions.
					ParseTableShape(values, 2, 3, table_meta);
					CACHE_DEBUG(2, "table shape: %s kind=%d partitioned=%d", table_meta.name.c_str(),
								(int)table_meta.index_kind, (int)table_meta.is_partitioned);
					return;
				}
				if (result_set == 1) {
					// COLUMN_DISCOVERY_SQL_TEMPLATE: the eight column fields, then
					// is_identity and code_page.
					if (values.size() >= 8) {
						table_meta.columns.push_back(
							ColumnFromRow(values, 0, 8, 9, database_collation_, database_code_page_));
					}
					return;
				}
				if (result_set == 2) {
					mssql::RowIdKeyInfo::AppendCandidateRow(table_meta.pk_info, values);
				}
			},
			reset_slot);
	} catch (...) {
		schema.tables.erase(slot_it);
		throw;
	}

	// Found means an object row AND at least one column, as when the columns were
	// a join on the object: OBJECT_ID also resolves a synonym, a procedure or a
	// scalar function, which have no columns and are not tables.
	if (!have_object || table_meta.columns.empty()) {
		schema.tables.erase(slot_it);
		CACHE_DEBUG(1, "GetTableMetadata('%s.%s') — table not found on SQL Server", schema_name.c_str(),
					table_name.c_str());
		return false;
	}

	// Cache the result (slot is already in the map)
	// Spec 077 W1: the key result set was every unique index; choose now.
	table_meta.pk_info.FinalizeChoice(database_collation_);
	table_meta.pk_loaded = true;
	table_meta.columns_load_state = CacheLoadState::LOADED;
	table_meta.columns_last_refresh = std::chrono::steady_clock::now();

	CACHE_DEBUG(1, "GetTableMetadata('%s.%s') — loaded %zu columns, PK with %zu column(s)", schema_name.c_str(),
				table_name.c_str(), table_meta.columns.size(), table_meta.pk_info.columns.size());

	out_meta = table_meta;	// copy under mutex_ — see header contract
	return true;
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
	std::lock_guard<std::mutex> lock(mutex_);

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
		std::lock_guard<std::mutex> lock(mutex_);
		auto schema_it = schemas_.find(schema_name);
		if (schema_it == schemas_.end()) {
			CACHE_DEBUG(1, "LoadAllTableMetadata('%s') — schema not found", schema_name.c_str());
			return;
		}

		auto &schema = schema_it->second;

		// If tables are loaded and all have columns loaded (not invalidated), use cache
		if (schema.tables_load_state == CacheLoadState::LOADED &&
			!IsTTLExpired(schema.tables_last_refresh, ttl_seconds_)) {
			bool all_columns_loaded = true;
			for (const auto &table_pair : schema.tables) {
				if (table_pair.second.columns_load_state != CacheLoadState::LOADED ||
					IsTTLExpired(table_pair.second.columns_last_refresh, ttl_seconds_)) {
					all_columns_loaded = false;
					break;
				}
			}
			// An EMPTY schema counts too (issue #412): the whole-catalog load marks
			// it LOADED for exactly this, and every Scan of it -- DuckDB's "did you
			// mean" walks every schema on a missing table -- reloaded the whole
			// catalog. A failed load publishes nothing (issue #317), so an empty
			// LOADED schema is a genuinely empty one.
			if (all_columns_loaded) {
				CACHE_DEBUG(1, "LoadAllTableMetadata('%s') — all %zu tables already loaded", schema_name.c_str(),
							schema.tables.size());
				return;
			}
		}
	}

	// Not cached: load the WHOLE catalog, not just this schema (spec 071 W2).
	//
	// Counter-intuitive only until the cost is measured. Listing one schema costs
	// a full pass over sys.objects whatever the predicate says, because metadata
	// visibility is filtered per object in the DATABASE — an EMPTY schema measured
	// 484 ms of server CPU at 200K objects. DuckDB drives this once per schema, so
	// the per-schema shape is O(schemas x objects); one query for all of them
	// measured 1917 ms total. Break-even is under five schemas.
	//
	// The plain `SELECT * FROM db.sch.tbl` path does NOT come here — it goes to
	// GetTableMetadata, one table, one seek — so ordinary queries stay lazy.
	CACHE_DEBUG(1, "LoadAllTableMetadata('%s') — loading every schema in one query", schema_name.c_str());
	idx_t all_schemas = 0, all_tables = 0, all_columns = 0;
	LoadAllSchemasMetadata(connection, all_schemas, all_tables, all_columns);
}

//===----------------------------------------------------------------------===//
// Bulk Catalog Preload (Spec 033: US5)
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Whole-catalog metadata load (spec 071 W2)
//===----------------------------------------------------------------------===//

void MSSQLMetadataCache::LoadAllSchemasMetadata(tds::TdsConnection &connection, idx_t &schema_count, idx_t &table_count,
												idx_t &column_count) {
	std::lock_guard<std::mutex> lock(mutex_);
	LoadAllSchemasMetadataLocked(connection, schema_count, table_count, column_count);
}

void MSSQLMetadataCache::LoadAllSchemasMetadataLocked(tds::TdsConnection &connection, idx_t &schema_count,
													  idx_t &table_count, idx_t &column_count) {
	schema_count = 0;
	table_count = 0;
	column_count = 0;

	// Nothing is written into schemas_ until the query has RETURNED. Issue #317:
	// this used to clear each schema's table map from inside the row callback and
	// only re-publish tables_load_state afterwards, so a non-retryable throw
	// mid-query -- a metadata timeout, a reset connection, any TDS error -- left
	// every schema the callback had already touched EMPTIED while still carrying
	// its previous load state. The reset lambda covered the 1205 retry only.
	//
	// It is the same shape the issue #178 review fixed for Refresh(), and spec
	// 071 widened the blast radius from one schema to every schema in the catalog
	// by replacing the per-schema loop with a single query.
	//
	// It was LATENT rather than live, recorded so nobody "simplifies" this back
	// after finding the same reassurance. Two things hid it, neither of them
	// here: the publication of each table's columns_load_state is ALSO deferred
	// to after the query, so a touched schema comes out of a failed load either
	// empty or holding tables whose columns say NOT_LOADED -- and
	// LoadAllTableMetadata rejected both (`all_columns_loaded &&
	// !tables.empty()`) and reloaded. The emptiness half of that test is gone
	// (issue #412: it reloaded the whole catalog on every Scan of an empty
	// schema); the staging below is what keeps a failed load from publishing.
	// (The one reader that would have trusted the broken state,
	// EnsureTablesLoaded, had no callers and was deleted in spec 084.)
	//
	// So the old code was safe by three deferrals happening to line up, on
	// layers that do not know about each other. This one is safe because this
	// function does not publish until it has an answer.
	//
	// The cost is one extra copy of the table metadata for the duration of a
	// load, PROPORTIONAL TO THE CATALOG and held while mutex_ is held. Measured
	// +~2 MB peak RSS on a warm reload of 2039 tables / 16202 columns -- which is
	// ~1% of the scale this file is written for (the 200K-object figures at the
	// top and on LoadAllTableMetadata), so read it as a rate, not a ceiling.
	//
	// Staging the TABLE MAPS rather than whole MSSQLSchemaMetadata values keeps
	// each schema's other fields (its name, and any state a future field adds)
	// instead of rebuilding them from nothing.
	BulkLoadStage stage;
	LoadObjectsAndColumnsLocked(connection, nullptr, stage);
	auto &staged = stage.tables;
	table_count = stage.table_count;
	column_count = stage.column_count;
	const auto now = std::chrono::steady_clock::now();

	// Publish. A schema's old table map is replaced, not merged, so the query's
	// answer is the whole answer. Every schema in the answer counts, as its tables
	// and columns do (issue #375) -- not only the schemas new to the cache, which
	// is none of them once BulkLoadAll has loaded the schema list first (#376).
	schema_count = staged.size();
	for (auto &pair : staged) {
		auto schema_it = schemas_.find(pair.first);
		if (schema_it == schemas_.end()) {
			schema_it = schemas_.emplace(pair.first, MSSQLSchemaMetadata(pair.first)).first;
		}
		auto &schema = schema_it->second;
		schema.tables = std::move(pair.second);
		schema.tables_load_state = CacheLoadState::LOADED;
		schema.tables_last_refresh = now;
	}
	// A schema the query returned NO rows for is still loaded — it is empty, and
	// saying so is what stops the next Scan from asking again.
	for (auto &pair : schemas_) {
		if (filter_ && !filter_->MatchesSchema(pair.first)) {
			continue;
		}
		if (staged.find(pair.first) == staged.end()) {
			pair.second.tables.clear();
			pair.second.tables_load_state = CacheLoadState::LOADED;
			pair.second.tables_last_refresh = now;
		}
	}

	CACHE_DEBUG(1, "LoadAllSchemasMetadata — %llu schema(s), %llu table(s), %llu column(s) in ONE query",
				(unsigned long long)staged.size(), (unsigned long long)table_count, (unsigned long long)column_count);
}

void MSSQLMetadataCache::BulkLoadAll(tds::TdsConnection &connection, const string &schema_name, idx_t &schema_count,
									 idx_t &table_count, idx_t &column_count) {
	schema_count = 0;
	table_count = 0;
	column_count = 0;

	// The schema LIST first, on both paths (issue #376) -- one light query, and a
	// no-op when it is already loaded. Neither path below loads it: the
	// per-schema one used to mark it LOADED holding only the schema it had been
	// given, so every other schema of the database stopped existing for the rest
	// of the session; the whole-catalog one left it NOT_LOADED, so the first
	// catalog access after the preload ran EnsureSchemasLoaded, which clears
	// schemas_, and loaded the whole catalog a second time.
	//
	// ONE lock across the list and the load (review of #377): released in
	// between, an invalidation could clear the list the load then relies on.
	std::lock_guard<std::mutex> lock(mutex_);
	EnsureSchemasLoadedLocked(connection);

	// With no schema named, ONE query covers the catalog (spec 071 W2).
	//
	// This used to iterate per schema, and the reason recorded here was the sort:
	// "ORDER BY s.name, o.name, c.column_id on millions of rows exceeds the memory
	// grant and causes non-linear performance degradation". True, and the sort was
	// only ever there so a STREAMING parse could see table boundaries. Grouping on
	// object_id in a hash map removes the sort, and with it the reason to loop —
	// which was also the reason mssql_preload_catalog() was O(schemas x objects)
	// and could not rescue issue #86 however often it was recommended.
	if (schema_name.empty()) {
		LoadAllSchemasMetadataLocked(connection, schema_count, table_count, column_count);
		return;
	}

	// One schema: the exact name, else the ONE schema whose name matches it
	// ignoring case. Before #376 the name went to the server as @s, which under
	// the usual case-insensitive collation found `dbo` for 'DBO' (review of
	// #377). Several matches -- names that differ only in case, under a
	// case-sensitive collation -- need the exact name. A schema that is not in
	// the list is absent from the database or hidden by schema_filter; it is
	// refused by name rather than reported as an empty schema, and gets no
	// phantom entry.
	auto schema_it = schemas_.find(schema_name);
	if (schema_it == schemas_.end()) {
		idx_t matches = 0;
		for (auto it = schemas_.begin(); it != schemas_.end(); ++it) {
			if (StringUtil::CIEquals(it->first, schema_name)) {
				schema_it = it;
				matches++;
			}
		}
		if (matches > 1) {
			throw InvalidInputException(
				"mssql_preload_catalog: schema name '%s' matches %llu schemas that differ only "
				"in case; give the exact name",
				schema_name, (unsigned long long)matches);
		}
	}
	if (schema_it == schemas_.end() || (filter_ && !filter_->MatchesSchema(schema_it->first))) {
		throw InvalidInputException(
			"mssql_preload_catalog: schema '%s' does not exist in this catalog, or schema_filter hides it",
			schema_name);
	}
	auto &schema = schema_it->second;
	const string &target_schema = schema_it->first;

	// The schema's objects and columns, staged (issue #317: nothing reaches the
	// cache until the query has returned, so a failure mid-load leaves it as it
	// was).
	BulkLoadStage stage;
	LoadObjectsAndColumnsLocked(connection, &target_schema, stage);

	// Publish per table rather than wholesale: this query may not cover every
	// table the schema legitimately holds -- one excluded by table_filter, or
	// loaded singly and not matched here -- and replacing the map would drop
	// those. Every table in the answer counts, as every column does (#375).
	const auto now = std::chrono::steady_clock::now();
	for (auto &staged_schema : stage.tables) {
		for (auto &staged : staged_schema.second) {
			schema.tables[staged.first] = std::move(staged.second);
		}
	}
	table_count = stage.table_count;
	column_count = stage.column_count;

	CACHE_DEBUG(1, "BulkLoadAll: schema '%s' — %llu tables, %llu columns", target_schema.c_str(),
				(unsigned long long)table_count, (unsigned long long)column_count);

	// Only what this call loaded is marked (issue #376): the target schema's
	// table list, and the columns of the tables in its answer. This used to mark
	// EVERY schema's table list and EVERY table's columns LOADED, so a schema or
	// a table this query never touched turned up empty and claiming to be
	// complete.
	schema.tables_load_state = CacheLoadState::LOADED;
	schema.tables_last_refresh = now;
	schema_count = 1;
}

void MSSQLMetadataCache::LoadObjectsAndColumnsLocked(tds::TdsConnection &connection, const string *one_schema,
													 BulkLoadStage &stage) {
	// Both filters push to the server where they convert to LIKE -- the schema
	// one is the only thing that can make a whole-catalog load smaller than the
	// catalog. They go into all three statements; the regex is applied client-side
	// after, as before.
	string scope;
	if (one_schema) {
		scope += "\n  AND s.schema_id = SCHEMA_ID(@s)";
	}
	// The row-count pass is for a load that reads the whole catalog: a filter
	// the server applies makes it read less, while one that does not convert to
	// LIKE (`.*_prod$`) still lists every object and keeps the pass, which
	// costs the same whatever is listed (review of #417).
	bool narrowed = false;
	if (filter_ && filter_->HasSchemaFilter()) {
		string like_clause = MSSQLCatalogFilter::TryRegexToSQLLike(filter_->GetSchemaPattern(), "s.name");
		if (!like_clause.empty()) {
			scope += " AND " + like_clause;
			narrowed = true;
		}
	}
	if (filter_ && filter_->HasTableFilter()) {
		string like_clause = MSSQLCatalogFilter::TryRegexToSQLLike(filter_->GetTablePattern(), "o.name");
		if (!like_clause.empty()) {
			scope += " AND " + like_clause;
			narrowed = true;
		}
	}
	const bool rc_pass = !one_schema && !narrowed && row_count_pass_;
	const string batch = BuildBulkMetadataBatch(scope, rc_pass);
	const tds::Request request =
		one_schema ? mssql::BuildExecuteSqlRequest(batch, "@s sysname", {{"s", *one_schema}}) : tds::Request(batch);

	// object_id -> the entry it owns, pointing into stage.tables. Valid across
	// insertion because both containers are node-based unordered_maps.
	unordered_map<string, MSSQLTableMetadata *> by_object_id;
	ExecuteMetadataQuerySets(
		connection, request,
		[&](idx_t result_set, const vector<string> &values) {
			if (result_set == 0) {
				// The objects: object_id, schema, name, type, approx_rows,
				// index_type, is_partitioned.
				if (values.size() < 7) {
					return;
				}
				const string &schema_name = values[1];
				const string &table_name = values[2];
				if (filter_ && (!filter_->MatchesSchema(schema_name) || !filter_->MatchesTable(table_name))) {
					return;
				}
				auto &slot = stage.tables[schema_name][table_name];
				slot = MSSQLTableMetadata();
				slot.name = table_name;
				slot.object_type =
					(!values[3].empty() && values[3][0] == 'V') ? MSSQLObjectType::VIEW : MSSQLObjectType::TABLE;
				try {
					slot.approx_row_count = static_cast<idx_t>(std::stoll(values[4]));
				} catch (...) {
					slot.approx_row_count = 0;
				}
				// values[5] is sys.indexes.type -- 1 clustered rowstore, 5 clustered
				// COLUMNSTORE, 0 heap -- and values[6] whether the object sits on a
				// partition scheme: the write path's TABLOCK and sort decisions.
				ParseTableShape(values, 5, 6, slot);
				by_object_id[values[0]] = &slot;
				return;
			}
			if (result_set == 1) {
				// The columns: object_id, then the eight column fields,
				// is_identity and code_page. A column of an object the first
				// statement did not return (filtered out, or created between the
				// two statements) is dropped.
				if (values.size() < 9) {
					return;
				}
				auto found = by_object_id.find(values[0]);
				if (found == by_object_id.end()) {
					return;
				}
				found->second->columns.push_back(
					ColumnFromRow(values, 1, 9, 10, database_collation_, database_code_page_));
				stage.column_count++;
				return;
			}
			if (result_set == 2) {
				// The keys: object_id, then the key discovery's 17 columns.
				auto found = by_object_id.find(values.empty() ? string() : values[0]);
				if (found != by_object_id.end()) {
					mssql::RowIdKeyInfo::AppendCandidateRow(found->second->pk_info, values, 1);
				}
			}
		},
		[&]() {
			// Restartable (PR #308): a deadlock victim reruns from the top, so the
			// grouping state goes back. Nothing is published yet (issue #317).
			stage.tables.clear();
			stage.table_count = 0;
			stage.column_count = 0;
			by_object_id.clear();
		});
	by_object_id.clear();

	// Columns arrive in no particular order: put each table's list into
	// column_id order, since every consumer indexes by position. An object with
	// no columns is not published -- the two statements can disagree under
	// concurrent DDL (dropped between them), and the old single join could not
	// produce such a table either.
	const auto now = std::chrono::steady_clock::now();
	for (auto &staged_schema : stage.tables) {
		auto &tables = staged_schema.second;
		for (auto it = tables.begin(); it != tables.end();) {
			auto &columns = it->second.columns;
			if (columns.empty()) {
				it = tables.erase(it);
				continue;
			}
			std::sort(columns.begin(), columns.end(),
					  [](const MSSQLColumnInfo &a, const MSSQLColumnInfo &b) { return a.column_id < b.column_id; });
			// Spec 077 W1's choice over the unique indexes the third result set
			// listed -- "no usable key" is an answer too, so every table is
			// pk_loaded (MSSQLTableEntry seeds its key from it).
			it->second.pk_info.FinalizeChoice(database_collation_);
			it->second.pk_loaded = true;
			it->second.columns_load_state = CacheLoadState::LOADED;
			it->second.columns_last_refresh = now;
			stage.table_count++;
			++it;
		}
	}
}

void MSSQLMetadataCache::ForEachTable(
	const std::function<void(const string &, const string &, idx_t)> &callback) const {
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto &schema_pair : schemas_) {
		for (const auto &table_pair : schema_pair.second.tables) {
			callback(schema_pair.first, table_pair.first, table_pair.second.approx_row_count);
		}
	}
}

void MSSQLMetadataCache::ForEachTableInSchema(
	const string &schema_name, const std::function<void(const string &, const MSSQLTableMetadata &)> &callback) const {
	std::lock_guard<std::mutex> lock(mutex_);
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
	invalidation_epoch_++;

	// Clear existing data
	schemas_.clear();
	database_collation_ = database_collation;

	try {
		// Load schemas
		LoadSchemas(connection);

		// Spec 084 D6: the whole catalog in one batch -- objects, columns, keys --
		// instead of one listing per schema and one columns query PER TABLE
		// (200k round trips on a 200k-object catalog, and no keys).
		idx_t schema_count = 0, table_count = 0, column_count = 0;
		LoadAllSchemasMetadataLocked(connection, schema_count, table_count, column_count);

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
		// Issue #178 review: schemas_ was cleared at the top of Refresh; if the
		// reload threw, schemas_load_state_ must NOT remain LOADED (a stale
		// LOADED over the emptied map made every later lookup report existing
		// tables as missing until a manual invalidation).
		schemas_load_state_ = CacheLoadState::NOT_LOADED;
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
	ttl_seconds_ = ttl_seconds;
}

int64_t MSSQLMetadataCache::GetTTL() const {
	return ttl_seconds_;
}

void MSSQLMetadataCache::SetDatabaseCollation(const string &collation, int32_t code_page) {
	std::lock_guard<std::mutex> lock(mutex_);
	database_collation_ = collation;
	database_code_page_ = code_page;
}

string MSSQLMetadataCache::GetDatabaseCollation() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return database_collation_;
}

void MSSQLMetadataCache::SetMetadataTimeout(int timeout_seconds) {
	metadata_timeout_ms_ = timeout_seconds > 0 ? timeout_seconds * 1000 : 0;
}

int MSSQLMetadataCache::GetMetadataTimeoutMs() const {
	return metadata_timeout_ms_;
}

void MSSQLMetadataCache::SetTestFailAfterRows(int64_t rows) {
	test_fail_after_rows_ = rows;
}

void MSSQLMetadataCache::ExecuteMetadataQuery(tds::TdsConnection &connection, const tds::Request &sql,
											  MSSQLMetadataCache::MetadataRowCallback callback,
											  MSSQLMetadataCache::MetadataResetCallback reset) {
	// Issue #317: the only deliberate way to a mid-query failure. Everything
	// that causes one in the wild — a metadata timeout, a reset connection, a
	// killed session — arrives from outside and cannot be asked for from SQL.
	// Off is the normal case and is free: the callback goes through unwrapped,
	// so the row loop carries no test.
	const int64_t fail_after = test_fail_after_rows_;
	if (fail_after > 0) {
		auto seen = make_shared_ptr<int64_t>(0);
		auto inner = std::move(callback);
		// Throw ON row N, not after it: `> fail_after` needed N+1 rows, so a
		// fixture returning exactly N made the whole lever a no-op that still
		// passed every non-error assertion written around it.
		callback = [inner, seen, fail_after](const vector<string> &values) {
			inner(values);
			if (++(*seen) >= fail_after) {
				throw IOException("mssql: injected metadata failure at row %lld (mssql_test_fail_metadata_after_rows)",
								  (long long)fail_after);
			}
		};
		// The reset lambda has to zero the counter too. RunMetadataQuery reruns
		// the query from the top on a deadlock victim (PR #308), and a count
		// carried over from attempt 1 would fire immediately on attempt 2 -- so
		// the lever would behave differently on exactly the retry path a
		// partial-load bug is most likely to be reasoned about.
		auto inner_reset = std::move(reset);
		reset = [inner_reset, seen]() {
			*seen = 0;
			if (inner_reset) {
				inner_reset();
			}
		};
	}
	RunMetadataQuery(connection, sql, std::move(callback), metadata_timeout_ms_, reset);
}

void MSSQLMetadataCache::ExecuteMetadataQuerySets(tds::TdsConnection &connection, const tds::Request &sql,
												  MSSQLMetadataCache::MetadataSetRowCallback callback,
												  MSSQLMetadataCache::MetadataResetCallback reset) {
	// The #317 lever, as in ExecuteMetadataQuery: counts rows across every
	// result set of the batch.
	const int64_t fail_after = test_fail_after_rows_;
	if (fail_after > 0) {
		auto seen = make_shared_ptr<int64_t>(0);
		auto inner = std::move(callback);
		callback = [inner, seen, fail_after](idx_t result_set, const vector<string> &values) {
			inner(result_set, values);
			if (++(*seen) >= fail_after) {
				throw IOException("mssql: injected metadata failure at row %lld (mssql_test_fail_metadata_after_rows)",
								  (long long)fail_after);
			}
		};
		auto inner_reset = std::move(reset);
		reset = [inner_reset, seen]() {
			*seen = 0;
			if (inner_reset) {
				inner_reset();
			}
		};
	}
	RunMetadataQuerySets(connection, sql, std::move(callback), metadata_timeout_ms_, reset);
}

//===----------------------------------------------------------------------===//
//===----------------------------------------------------------------------===//
// Incremental Cache Loading - Lazy Loading (T010-T012)
//===----------------------------------------------------------------------===//

void MSSQLMetadataCache::EnsureSchemasLoaded(tds::TdsConnection &connection) {
	// Issue #178 (D4): no unlocked fast path — the old pre-lock check read
	// schemas_load_state_ / schemas_last_refresh_ racily. The mutex is cheap
	// next to everything around it (a catalog lookup already did a settings
	// read and a pool interaction to get here).
	std::lock_guard<std::mutex> lock(mutex_);
	EnsureSchemasLoadedLocked(connection);
}

void MSSQLMetadataCache::EnsureSchemasLoadedLocked(tds::TdsConnection &connection) {
	if (schemas_load_state_ == CacheLoadState::LOADED && !IsTTLExpired(schemas_last_refresh_, ttl_seconds_)) {
		CACHE_DEBUG(2, "EnsureSchemasLoaded — already loaded (%zu schemas)", schemas_.size());
		return;
	}
	CACHE_DEBUG(1, "EnsureSchemasLoaded — loading schemas from SQL Server");

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

		ExecuteMetadataQuery(
			connection, schema_sql,
			[this](const vector<string> &values) {
				if (!values.empty()) {
					string schema_name = values[0];
					// Create schema with only name - tables NOT loaded (tables_load_state = NOT_LOADED)
					schemas_.emplace(schema_name, MSSQLSchemaMetadata(schema_name));
				}
			},
			[]() {
				// Nothing to undo: emplace() on an existing key is a no-op, so a
				// second pass over the same schema names converges on the same map.
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

//===----------------------------------------------------------------------===//
// Point Invalidation (T034, T040, T043)
//===----------------------------------------------------------------------===//

void MSSQLMetadataCache::InvalidateSchema(const string &schema_name) {
	std::lock_guard<std::mutex> lock(mutex_);
	invalidation_epoch_++;
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

void MSSQLMetadataCache::InvalidateSchemaTableList(const string &schema_name) {
	std::lock_guard<std::mutex> lock(mutex_);
	invalidation_epoch_++;
	auto it = schemas_.find(schema_name);
	if (it != schemas_.end()) {
		// Existence only — re-fetch the table list, but keep every table's cached
		// column metadata (the expensive part). Used by per-table invalidation.
		it->second.tables_load_state = CacheLoadState::NOT_LOADED;
	}
}

void MSSQLMetadataCache::InvalidateTable(const string &schema_name, const string &table_name) {
	std::lock_guard<std::mutex> lock(mutex_);
	invalidation_epoch_++;
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
	std::lock_guard<std::mutex> lock(mutex_);
	invalidation_epoch_++;
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
	std::lock_guard<std::mutex> lock(mutex_);
	return schemas_load_state_;
}

CacheLoadState MSSQLMetadataCache::GetTablesState(const string &schema_name) const {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = schemas_.find(schema_name);
	if (it == schemas_.end()) {
		return CacheLoadState::NOT_LOADED;
	}
	return it->second.tables_load_state;
}

bool MSSQLMetadataCache::IsColumnsFreshLocked(const MSSQLTableMetadata &table) const {
	return table.columns_load_state == CacheLoadState::LOADED &&
		   !IsTTLExpired(table.columns_last_refresh, ttl_seconds_);
}

bool MSSQLMetadataCache::TryGetLoadedTableMetadata(const string &schema_name, const string &table_name,
												   MSSQLTableMetadata &out_meta) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto schema_it = schemas_.find(schema_name);
	if (schema_it == schemas_.end()) {
		return false;
	}
	auto table_it = schema_it->second.tables.find(table_name);
	if (table_it == schema_it->second.tables.end() || !IsColumnsFreshLocked(table_it->second)) {
		return false;
	}
	out_meta = table_it->second;
	return true;
}

bool MSSQLMetadataCache::PublishTableMetadata(const string &schema_name, const MSSQLTableMetadata &meta,
											  uint64_t epoch) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (invalidation_epoch_.load() != epoch || meta.columns_load_state != CacheLoadState::LOADED) {
		return false;
	}
	auto schema_it = schemas_.find(schema_name);
	if (schema_it == schemas_.end()) {
		return false;
	}
	auto &tables = schema_it->second.tables;
	auto table_it = tables.find(meta.name);
	if (table_it != tables.end() && IsColumnsFreshLocked(table_it->second)) {
		return false;
	}
	tables[meta.name] = meta;
	return true;
}

bool MSSQLMetadataCache::TryGetLoadedSchemaNames(vector<string> &out_names,
												 std::chrono::steady_clock::time_point &out_loaded_at) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (schemas_load_state_ != CacheLoadState::LOADED || IsTTLExpired(schemas_last_refresh_, ttl_seconds_)) {
		return false;
	}
	out_names.clear();
	for (const auto &entry : schemas_) {
		out_names.push_back(entry.first);
	}
	out_loaded_at = schemas_last_refresh_;
	return true;
}

bool MSSQLMetadataCache::PublishSchemaNames(const vector<string> &names,
											std::chrono::steady_clock::time_point loaded_at, uint64_t epoch) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (invalidation_epoch_.load() != epoch) {
		return false;
	}
	if (schemas_load_state_ == CacheLoadState::LOADED && !IsTTLExpired(schemas_last_refresh_, ttl_seconds_)) {
		return false;
	}
	std::unordered_set<string> listed(names.begin(), names.end());
	for (auto it = schemas_.begin(); it != schemas_.end();) {
		it = listed.count(it->first) ? std::next(it) : schemas_.erase(it);
	}
	for (const auto &name : names) {
		schemas_.emplace(name, MSSQLSchemaMetadata(name));
	}
	schemas_load_state_ = CacheLoadState::LOADED;
	schemas_last_refresh_ = loaded_at;
	return true;
}

CacheLoadState MSSQLMetadataCache::GetColumnsState(const string &schema_name, const string &table_name) const {
	std::lock_guard<std::mutex> lock(mutex_);
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

	ExecuteMetadataQuery(
		connection, sql,
		[this](const vector<string> &values) {
			if (!values.empty()) {
				string schema_name = values[0];
				MSSQLSchemaMetadata schema_meta;
				schema_meta.name = schema_name;
				schemas_[schema_name] = std::move(schema_meta);
			}
		},
		[this]() {
			// schemas_[name] = ... overwrites, so re-running only rewrites the same
			// entries. Not cleared: schemas_ may hold tables loaded earlier that
			// this query does not re-report.
		});
}

bool MSSQLMetadataCache::TryGetTableNames(const string &schema_name, vector<string> &out_names) {
	std::lock_guard<std::mutex> lock(mutex_);
	out_names.clear();
	auto schema_it = schemas_.find(schema_name);
	if (schema_it != schemas_.end() && schema_it->second.tables_load_state == CacheLoadState::LOADED &&
		!IsTTLExpired(schema_it->second.tables_last_refresh, ttl_seconds_)) {
		for (const auto &pair : schema_it->second.tables) {
			out_names.push_back(pair.first);
		}
		return true;
	}
	if (!table_names_loaded_ || table_names_epoch_ != invalidation_epoch_.load() ||
		IsTTLExpired(table_names_loaded_at_, ttl_seconds_)) {
		return false;
	}
	auto names_it = table_names_.find(schema_name);
	if (names_it != table_names_.end()) {
		out_names = names_it->second;
	}
	return true;
}

void MSSQLMetadataCache::LoadAllTableNames(tds::TdsConnection &connection) {
	std::lock_guard<std::mutex> lock(mutex_);
	unordered_map<string, vector<string>> staged;
	ExecuteMetadataQuery(
		connection, TABLE_NAMES_SQL,
		[&](const vector<string> &values) {
			if (values.size() < 2) {
				return;
			}
			if (filter_ && (!filter_->MatchesSchema(values[0]) || !filter_->MatchesTable(values[1]))) {
				return;
			}
			staged[values[0]].push_back(values[1]);
		},
		[&]() { staged.clear(); });
	table_names_ = std::move(staged);
	table_names_loaded_ = true;
	table_names_epoch_ = invalidation_epoch_.load();
	table_names_loaded_at_ = std::chrono::steady_clock::now();
	CACHE_DEBUG(1, "LoadAllTableNames — %zu schema(s) with tables", table_names_.size());
}

}  // namespace duckdb
