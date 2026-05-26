#include "catalog/mssql_table_set.hpp"
#include <cstdio>
#include <cstdlib>
#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_catalog_filter.hpp"
#include "catalog/mssql_schema_entry.hpp"
#include "catalog/mssql_statistics.hpp"
#include "catalog/mssql_table_entry.hpp"
#include "duckdb/common/exception.hpp"

// Debug logging for catalog operations
static int GetCatalogDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define CATALOG_DEBUG(lvl, fmt, ...)                                     \
	do {                                                                 \
		if (GetCatalogDebugLevel() >= lvl)                               \
			fprintf(stderr, "[MSSQL CATALOG] " fmt "\n", ##__VA_ARGS__); \
	} while (0)

namespace duckdb {

MSSQLTableSet::MSSQLTableSet(MSSQLSchemaEntry &schema)
	: schema_(schema), names_loaded_(false), is_fully_loaded_(false) {}

//===----------------------------------------------------------------------===//
// Entry Access
//===----------------------------------------------------------------------===//

optional_ptr<CatalogEntry> MSSQLTableSet::GetEntry(ClientContext &context, const string &name) {
	CATALOG_DEBUG(2, "GetEntry('%s.%s')", schema_.name.c_str(), name.c_str());
	// 1. Check cached entries (fast path)
	{
		std::lock_guard<std::mutex> lock(entry_mutex_);
		auto it = entries_.find(name);
		if (it != entries_.end()) {
			CATALOG_DEBUG(2, "  -> cache hit for '%s'", name.c_str());
			return it->second.get();
		}

		// If we've already tried to load this table and it wasn't found, return nullptr
		if (attempted_tables_.find(name) != attempted_tables_.end()) {
			CATALOG_DEBUG(2, "  -> already attempted, not found");
			return nullptr;
		}
	}

	// 2. If fully loaded and not found, table doesn't exist
	if (is_fully_loaded_.load()) {
		CATALOG_DEBUG(2, "  -> fully loaded, table not found");
		return nullptr;
	}

	// 3. Check table filter — filtered-out tables return not found
	{
		auto &catalog = schema_.GetMSSQLCatalog();
		auto &filter = catalog.GetCatalogFilter();
		if (filter.HasTableFilter() && !filter.MatchesTable(name)) {
			CATALOG_DEBUG(2, "  -> filtered out by table_filter");
			std::lock_guard<std::mutex> elock(entry_mutex_);
			attempted_tables_.insert(name);
			return nullptr;
		}
	}

	// 4. Check names list — if names loaded and name not in list, table doesn't exist
	//    This avoids an expensive round trip to SQL Server for nonexistent tables
	if (names_loaded_.load()) {
		std::lock_guard<std::mutex> nlock(names_mutex_);
		if (known_table_names_.find(name) == known_table_names_.end()) {
			CATALOG_DEBUG(2, "  -> not in known_table_names_ (%zu names loaded)", known_table_names_.size());
			std::lock_guard<std::mutex> elock(entry_mutex_);
			attempted_tables_.insert(name);
			return nullptr;
		}
	}

	// 5. Load single entry with full metadata (columns included)
	CATALOG_DEBUG(1, "  -> loading columns for '%s.%s' (single table)", schema_.name.c_str(), name.c_str());
	if (LoadSingleEntry(context, name)) {
		std::lock_guard<std::mutex> lock(entry_mutex_);
		auto it = entries_.find(name);
		if (it != entries_.end()) {
			return it->second.get();
		}
	}
	return nullptr;
}

void MSSQLTableSet::Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback) {
	CATALOG_DEBUG(1, "Scan('%s') — bulk loading all table metadata", schema_.name.c_str());

	auto &catalog = schema_.GetMSSQLCatalog();
	auto &cache = catalog.GetMetadataCache();
	auto &pool = catalog.GetConnectionPool();

	catalog.EnsureCacheLoaded(context);
	auto connection = pool.Acquire();
	if (!connection) {
		throw IOException("Failed to acquire connection for table scan");
	}

	// Load all tables + columns for this schema in one bulk query (or from cache if already loaded)
	try {
		cache.LoadAllTableMetadata(*connection, schema_.name);
	} catch (...) {
		pool.Release(std::move(connection));
		throw;
	}

	pool.Release(std::move(connection));

	// Build entries from fully-loaded cache and pre-populate statistics.
	// Pre-populating statistics avoids 200K+ individual DMV queries when
	// duckdb_tables() calls GetStorageInfo() on each table entry.
	auto &stats_provider = catalog.GetStatisticsProvider();
	std::lock_guard<std::mutex> lock(entry_mutex_);

	cache.ForEachTableInSchema(schema_.name, [&](const string &table_name, const MSSQLTableMetadata &table_meta) {
		// Pre-populate statistics cache with approx_row_count from bulk metadata
		stats_provider.PreloadRowCount(schema_.name, table_name, table_meta.approx_row_count);

		// Update known_table_names_
		known_table_names_.insert(table_name);

		// Skip if already loaded as entry
		if (entries_.find(table_name) != entries_.end()) {
			callback(*entries_[table_name]);
			return;
		}

		auto entry = CreateTableEntry(table_meta);
		if (entry) {
			// Spec 052 T007: emplace-only (winner wins on race vs LoadSingleEntry).
			// On collision the new entry is discarded; the in-place entry already
			// served any concurrent first-load caller. C++11-compatible pair
			// destructuring (CLAUDE.md ODR rule: no structured bindings).
			auto insert_result = entries_.emplace(entry->name, std::move(entry));
			callback(*insert_result.first->second);
		}
	});

	names_loaded_.store(true);
	is_fully_loaded_.store(true);
}

//===----------------------------------------------------------------------===//
// Entry Loading
//===----------------------------------------------------------------------===//

bool MSSQLTableSet::LoadSingleEntry(ClientContext &context, const string &name) {
	auto &catalog = schema_.GetMSSQLCatalog();
	auto &cache = catalog.GetMetadataCache();
	auto &pool = catalog.GetConnectionPool();

	// Ensure cache settings are loaded (sets TTL)
	catalog.EnsureCacheLoaded(context);

	// Spec 052 singleflight (FR-007): coordinate concurrent first-loads of the
	// same table so only ONE thread issues the SQL Server round trip. Waiters
	// re-check the cache after the owner finishes; on cache hit they return
	// the owner's entry without a redundant fetch.
	{
		std::unique_lock<std::mutex> lock(entry_mutex_);
		load_cv_.wait(lock, [this, &name]() { return loads_in_progress_.find(name) == loads_in_progress_.end(); });
		// Owner-before-us may have already loaded or confirmed not-exists.
		if (entries_.find(name) != entries_.end()) {
			return true;
		}
		if (attempted_tables_.find(name) != attempted_tables_.end()) {
			return false;
		}
		// Take the load slot. From here we are the sole fetcher for `name`.
		loads_in_progress_.insert(name);
	}

	// Owner of the load slot. The SQL Server round trip happens with no mutex
	// held so different tables can load in parallel. The slot is released at
	// the end (success, table-not-exists, or exception) under entry_mutex_,
	// followed by notify_all() on load_cv_ to wake any waiters.
	std::exception_ptr propagated;
	shared_ptr<MSSQLTableEntry> fetched_entry;
	bool table_exists = false;

	try {
		auto connection = pool.Acquire();
		if (!connection) {
			throw IOException("Failed to acquire connection for table loading");
		}
		const MSSQLTableMetadata *table_meta = nullptr;
		try {
			table_meta = cache.GetTableMetadata(*connection, schema_.name, name);
		} catch (...) {
			pool.Release(std::move(connection));
			throw;
		}
		pool.Release(std::move(connection));

		if (table_meta) {
			table_exists = true;
			fetched_entry = CreateTableEntry(*table_meta);
		}
	} catch (...) {
		propagated = std::current_exception();
	}

	// Publish results + release the load slot. attempted_tables_ is set on
	// success and on confirmed-not-exists; on exception it is NOT set so the
	// next caller retries the fetch.
	{
		std::unique_lock<std::mutex> lock(entry_mutex_);
		if (table_exists && fetched_entry) {
			entries_.emplace(fetched_entry->name, std::move(fetched_entry));
			attempted_tables_.insert(name);
		} else if (!propagated && !table_exists) {
			attempted_tables_.insert(name);
		}
		loads_in_progress_.erase(name);
	}
	load_cv_.notify_all();

	if (propagated) {
		std::rethrow_exception(propagated);
	}
	return table_exists;
}

bool MSSQLTableSet::IsLoaded() const {
	return is_fully_loaded_.load();
}

void MSSQLTableSet::Invalidate() {
	std::lock_guard<std::mutex> lock(load_mutex_);
	is_fully_loaded_.store(false);
	names_loaded_.store(false);
	// Spec 052 T011: move retired entries into the catalog's graveyard
	// instead of dropping them outright. In-flight binders still hold raw
	// pointers obtained before this Invalidate() — the graveyard's shared_ptr
	// keeps the underlying MSSQLTableEntry objects alive until either the
	// bind data releases its anchor (US2 T015) or ~MSSQLCatalog drains the
	// graveyard. Subsequent LoadSingleEntry calls see an empty entries_ map
	// and reload from SQL Server (FR-006 — pre-Invalidate binders observe
	// stale metadata; new binds resolve fresh metadata).
	vector<shared_ptr<MSSQLTableEntry>> retired;
	{
		std::lock_guard<std::mutex> elock(entry_mutex_);
		retired.reserve(entries_.size());
		for (auto &kv : entries_) {
			retired.push_back(std::move(kv.second));
		}
		entries_.clear();
		attempted_tables_.clear();
	}
	schema_.GetMSSQLCatalog().AppendToTableGraveyard(std::move(retired));
	{
		std::lock_guard<std::mutex> nlock(names_mutex_);
		known_table_names_.clear();
	}
}

//===----------------------------------------------------------------------===//
// Internal Methods
//===----------------------------------------------------------------------===//

shared_ptr<MSSQLTableEntry> MSSQLTableSet::CreateTableEntry(const MSSQLTableMetadata &metadata) {
	// Spec 052: make_shared_ptr (DuckDB's shared_ptr wrapper) so the entry is
	// owned via shared_ptr from the moment of construction. Required for
	// enable_shared_from_this to work in MSSQLTableEntry::GetScanFunction's
	// bind-data anchor (shared_from_this()).
	return make_shared_ptr<MSSQLTableEntry>(schema_.catalog, schema_, metadata);
}

void MSSQLTableSet::EnsureNamesLoaded(ClientContext &context) {
	// Fast path: names already loaded
	if (names_loaded_.load()) {
		CATALOG_DEBUG(2, "EnsureNamesLoaded('%s') — already loaded", schema_.name.c_str());
		return;
	}
	CATALOG_DEBUG(1, "EnsureNamesLoaded('%s') — loading table names from SQL Server", schema_.name.c_str());

	// Double-checked locking
	std::lock_guard<std::mutex> lock(names_mutex_);
	if (names_loaded_.load()) {
		return;
	}

	auto &catalog = schema_.GetMSSQLCatalog();
	auto &cache = catalog.GetMetadataCache();
	auto &pool = catalog.GetConnectionPool();

	catalog.EnsureCacheLoaded(context);
	auto connection = pool.Acquire();
	if (!connection) {
		throw IOException("Failed to acquire connection for table name loading");
	}

	// Only loads table names (fast, no column queries)
	vector<string> table_names;
	try {
		table_names = cache.GetTableNames(*connection, schema_.name);
	} catch (...) {
		pool.Release(std::move(connection));
		throw;
	}

	pool.Release(std::move(connection));

	for (const auto &name : table_names) {
		known_table_names_.insert(name);
	}
	CATALOG_DEBUG(1, "EnsureNamesLoaded('%s') — loaded %zu table names (no column queries)", schema_.name.c_str(),
				  known_table_names_.size());
	names_loaded_.store(true);
}

}  // namespace duckdb
