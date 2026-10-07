#include "catalog/mssql_catalog.hpp"

#include "catalog/mssql_catalog_debug.hpp"
#include "catalog/mssql_schema_entry.hpp"
#include "catalog/mssql_statistics.hpp"
#include "catalog/mssql_transaction.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "connection/mssql_settings.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Metadata cache: invalidation, transaction metadata, preload / refresh
//===----------------------------------------------------------------------===//

void MSSQLCatalog::InvalidateMetadataCache() {
	if (metadata_cache_) {
		metadata_cache_->Invalidate();
	}

	// The statistics cache too (job 1124). It was invalidated by NOTHING —
	// InvalidateAll/InvalidateTable/InvalidateSchema had no callers at all — which
	// was invisible while row counts fed only GetStorageInfo, a number the planner
	// ignores for a table function. Since the spec-070 cardinality callback they
	// ARE what the optimizer plans on, so a stale count now survives an explicit
	// mssql_invalidate_cache() and drives join order for the whole TTL.
	if (statistics_provider_) {
		statistics_provider_->InvalidateAll();
	}

	// Also clear the local schema entry cache.
	// Spec 052 (Option D): in-flight binders are anchored in their
	// ClientContext's MSSQLBindAnchors; dropping entries_ here just
	// decrements refcount.
	std::lock_guard<std::mutex> lock(schema_mutex_);
	for (auto &entry : schema_entries_) {
		entry.second->GetTableSet().Invalidate();
	}
}

void MSSQLCatalog::InvalidateSchemaTableSet(const string &schema_name) {
	// Invalidate the schema's table list in the metadata cache
	if (metadata_cache_) {
		metadata_cache_->InvalidateSchema(schema_name);
	}

	// Also invalidate the local schema entry's table set if it exists.
	// Spec 052 (Option D): MSSQLBindAnchors holds in-flight binder refs.
	std::lock_guard<std::mutex> lock(schema_mutex_);
	auto it = schema_entries_.find(schema_name);
	if (it != schema_entries_.end()) {
		it->second->GetTableSet().Invalidate();
	}

	// And its row counts (job 1193). THIS is the path almost every size-changing
	// event takes — COPY/bulk load, CTAS, CREATE/DROP TABLE, and the two-argument
	// mssql_invalidate_cache(ctx, schema) — so without it InvalidateSchema still
	// had zero callers and a 1M-row COPY left the pre-load count driving plans
	// (and duckdb_tables()) for the whole TTL.
	if (statistics_provider_) {
		statistics_provider_->InvalidateSchema(schema_name);
	}
}

void MSSQLCatalog::InvalidateTableEntry(const string &schema_name, const string &table_name) {
	if (metadata_cache_) {
		// Re-fetch this table's columns (ALTER) ...
		metadata_cache_->InvalidateTable(schema_name, table_name);
		// ... and re-check the schema's table list for existence (CREATE/DROP/RENAME), but
		// WITHOUT dropping every other table's cached columns.
		metadata_cache_->InvalidateSchemaTableList(schema_name);
	}

	// Point-invalidate this table's row count as well (job 1124), so a DDL or a
	// load that changes its size is reflected in the next plan rather than after
	// the statistics TTL.
	if (statistics_provider_) {
		statistics_provider_->InvalidateTable(schema_name, table_name);
	}

	// Evict the single bound entry from the schema's table set (keeps the rest).
	std::lock_guard<std::mutex> lock(schema_mutex_);
	auto it = schema_entries_.find(schema_name);
	if (it != schema_entries_.end()) {
		it->second->GetTableSet().InvalidateEntry(table_name);
	}
}

unique_ptr<MSSQLMetadataCache> MSSQLCatalog::CreateTransactionMetadataCache(ClientContext &context) {
	auto cache = make_uniq<MSSQLMetadataCache>(0);
	if (catalog_filter_.HasFilters()) {
		cache->SetFilter(&catalog_filter_);
	}
	cache->SetMetadataTimeout(LoadMetadataTimeout(context));
	cache->SetTestFailAfterRows(LoadTestFailMetadataAfterRows(context));
	cache->SetDatabaseCollation(database_collation_, database_code_page_);
	// No DMV pass inside a transaction: it would run on the pinned connection,
	// in the user's server transaction, and read sysrowsets, where a deadlock
	// victim rolls that transaction back (spec 084 Risks).
	cache->SetRowCountPass(false);
	return cache;
}

std::mutex &MSSQLCatalog::MaterializeMutexFor(ClientContext &context) {
	if (context.transaction.IsAutoCommit()) {
		return materialize_mutex_;
	}
	return MSSQLTransaction::Get(context, *this).MaterializeMutex();
}

void MSSQLCatalog::NoteTransactionChange(ClientContext &context, const string &schema, const string &table) {
	if (context.transaction.IsAutoCommit()) {
		return;
	}
	MSSQLTransaction::Get(context, *this).Metadata(context).MarkChanged(schema, table);
}

void MSSQLCatalog::NoteTransactionChangeLocally(ClientContext &context) {
	if (context.transaction.IsAutoCommit()) {
		return;
	}
	MSSQLTransaction::Get(context, *this).Metadata(context).MarkChangedLocally();
}

void MSSQLCatalog::ForgetTransactionChanges(MSSQLTransactionMetadata &metadata) {
	auto changes = metadata.GetChanges();
	if (changes.all) {
		InvalidateMetadataCache();
		return;
	}
	for (const auto &schema : changes.schemas) {
		InvalidateSchemaTableSet(schema);
	}
	for (const auto &table : changes.tables) {
		InvalidateTableEntry(table.first, table.second);
	}
}

void MSSQLCatalog::PublishTransactionMetadata(MSSQLTransactionMetadata &metadata) noexcept {
	try {
		if (!metadata_cache_ || metadata.IsAllChanged()) {
			return;
		}
		if (StringUtil::Contains(StringUtil::Upper(TransactionIsolationStatement()), "UNCOMMITTED")) {
			return;
		}
		const uint64_t epoch = metadata.SharedEpochAtStart();
		vector<string> schema_names;
		std::chrono::steady_clock::time_point schemas_loaded_at;
		if (metadata.Cache().TryGetLoadedSchemaNames(schema_names, schemas_loaded_at)) {
			metadata_cache_->PublishSchemaNames(schema_names, schemas_loaded_at, epoch);
		}
		idx_t published = 0;
		for (const auto &key : metadata.GetLoadedTables()) {
			if (metadata.IsChanged(key.first, key.second) || !catalog_filter_.MatchesSchema(key.first) ||
				!catalog_filter_.MatchesTable(key.second)) {
				continue;
			}
			MSSQLTableMetadata meta;
			if (metadata.Cache().TryGetLoadedTableMetadata(key.first, key.second, meta) &&
				metadata_cache_->PublishTableMetadata(key.first, meta, epoch)) {
				published++;
			}
		}
		MSSQL_CATALOG_DEBUG_LOG(1, "PublishTransactionMetadata: %llu table(s) published",
								(unsigned long long)published);
	} catch (...) {
		// In memory and opportunistic: nothing here may fail a COMMIT that has
		// already happened on the server. The names stay unloaded, as before.
	}
}

void MSSQLCatalog::Preload(ClientContext &context, const string &schema_name, const char *caller, idx_t &schema_count,
						   idx_t &table_count, idx_t &column_count) {
	// Refused inside an explicit transaction that has used ANY MSSQL catalog
	// (issue #380): a forced load either blocks on the transaction's own
	// uncommitted DDL (a pool connection waits on its schema lock until the
	// metadata timeout) or, on the pinned connection, would publish the
	// transaction's uncommitted view into the cache every other connection
	// reads. Not scoped per catalog -- aliases of one database are independent
	// catalogs, and alias A's uncommitted DDL would hang a load on alias B (the
	// ATTACH of such an alias included). Invalidation stays allowed.
	if (ConnectionProvider::HasUsedAnyMSSQLCatalogInTransaction(context)) {
		throw InvalidInputException(
			"%s cannot run inside a transaction that has used an MSSQL catalog: it would load '%s' while the "
			"transaction may hold uncommitted changes. Use mssql_invalidate_cache() inside the transaction, or "
			"preload after COMMIT",
			caller, context_name_);
	}
	// The session's cache settings -- TTL, mssql_metadata_timeout, the test
	// lever -- before the load, not the constructor's placeholders.
	EnsureCacheLoaded(context);
	std::string why;
	auto connection = connection_pool_->Acquire(-1, &why);
	if (!connection) {
		throw IOException("%s: failed to acquire connection: %s", caller, why);
	}
	try {
		metadata_cache_->BulkLoadAll(*connection, schema_name, schema_count, table_count, column_count);
	} catch (...) {
		connection_pool_->Release(std::move(connection));
		throw;
	}
	connection_pool_->Release(std::move(connection));
	// Pre-populate the statistics cache with approx_row_count from the load: it
	// saves a DMV query per table when DuckDB calls GetStorageInfo().
	metadata_cache_->ForEachTable([&](const string &schema, const string &table, idx_t row_count) {
		statistics_provider_->PreloadRowCount(schema, table, row_count);
	});
	// The loaded columns are the server's now; a pushed statement's shape
	// described before may not be (preload does not move the epoch).
	describe_cache_.Clear();
	MSSQL_CATALOG_DEBUG_LOG(1, "Preload (%s): %llu schemas, %llu tables, %llu columns", caller,
							(unsigned long long)schema_count, (unsigned long long)table_count,
							(unsigned long long)column_count);
}

void MSSQLCatalog::EnsureCacheLoaded(ClientContext &context) {
	// Check if catalog integration is disabled
	if (!catalog_enabled_) {
		throw CatalogException(
			"MSSQL catalog '%s' is attached with catalog=false (catalog disabled). "
			"Schema discovery and direct table access are not available. "
			"Use mssql_scan('%s', 'SELECT ...') or mssql_exec('%s', 'SQL') for raw queries.",
			context_name_, context_name_, context_name_);
	}

	if (!connection_pool_) {
		throw IOException("MSSQL connection pool not initialized - cannot refresh cache");
	}

	// Load cache TTL from settings and apply it
	// Lazy loading will handle actual metadata loading on first access
	int64_t cache_ttl = LoadCatalogCacheTTL(context);
	metadata_cache_->SetTTL(cache_ttl);
	metadata_cache_->SetMetadataTimeout(LoadMetadataTimeout(context));
	metadata_cache_->SetTestFailAfterRows(LoadTestFailMetadataAfterRows(context));
	metadata_cache_->SetDatabaseCollation(database_collation_, database_code_page_);

	// Note: No eager Refresh() call - lazy loading handles this
	// Each cache level (schemas, tables, columns) loads independently on first access
}

void MSSQLCatalog::RefreshCache(ClientContext &context) {
	// Check if catalog integration is disabled
	if (!catalog_enabled_) {
		throw CatalogException(
			"MSSQL catalog '%s' is attached with catalog=false (catalog disabled). "
			"Cache refresh not available. "
			"Use mssql_scan('%s', 'SELECT ...') or mssql_exec('%s', 'SQL') for raw queries.",
			context_name_, context_name_, context_name_);
	}

	if (!connection_pool_) {
		throw IOException("MSSQL connection pool not initialized - cannot refresh cache");
	}

	// Load cache TTL and metadata timeout from settings
	int64_t cache_ttl = LoadCatalogCacheTTL(context);
	metadata_cache_->SetTTL(cache_ttl);

	// A refresh means "forget what you think you know", so the statistics cache is
	// cleared with it.
	//
	// It does NOT set the TTL (job 1216). mssql_refresh_cache() is session-scoped
	// while the provider is one object per catalog, so doing that here let one
	// session's `SET mssql_statistics_cache_ttl_seconds` govern every other
	// session's lookups — the same leak the planner path had. Every reader now
	// passes its own TTL in at the read instead.
	if (statistics_provider_) {
		statistics_provider_->InvalidateAll();
	}
	metadata_cache_->SetMetadataTimeout(LoadMetadataTimeout(context));
	metadata_cache_->SetTestFailAfterRows(LoadTestFailMetadataAfterRows(context));

	// Acquire connection for full cache refresh
	std::string why;
	auto connection = connection_pool_->Acquire(-1, &why);
	if (!connection) {
		throw IOException("Failed to acquire connection for cache refresh: " + why);
	}

	// Perform full eager cache refresh.
	// Exception-safe: if Refresh throws (TDS hiccup under stress — 1-in-300
	// odds during scenario 5's 318 invalidations × 30s soak), the connection
	// must be returned to the pool BEFORE the exception propagates, or
	// ~ConnectionPool fires its debug-only D_ASSERT about checked-out
	// connections during catalog teardown and the process aborts on Linux.
	try {
		metadata_cache_->Refresh(*connection, database_collation_);
	} catch (...) {
		connection_pool_->Release(std::move(connection));
		throw;
	}

	// Release connection
	connection_pool_->Release(std::move(connection));

	// Invalidate all schema table sets to pick up any changes.
	// Spec 052 (Option D): in-flight binders are anchored in
	// MSSQLBindAnchors per ClientContext (released at QueryEnd).
	std::lock_guard<std::mutex> lock(schema_mutex_);
	for (auto &entry : schema_entries_) {
		entry.second->GetTableSet().Invalidate();
	}
}

}  // namespace duckdb
