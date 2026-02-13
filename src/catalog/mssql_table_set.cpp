#include "catalog/mssql_table_set.hpp"
#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_catalog_filter.hpp"
#include "catalog/mssql_schema_entry.hpp"
#include "catalog/mssql_table_entry.hpp"
#include "duckdb/common/exception.hpp"
#include <cstdio>
#include <cstdlib>

// Debug logging for catalog operations
static int GetCatalogDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define CATALOG_DEBUG(lvl, fmt, ...)                                        \
	do {                                                                   \
		if (GetCatalogDebugLevel() >= lvl)                                 \
			fprintf(stderr, "[MSSQL CATALOG] " fmt "\n", ##__VA_ARGS__);   \
	} while (0)

namespace duckdb {

MSSQLTableSet::MSSQLTableSet(MSSQLSchemaEntry &schema) : schema_(schema), names_loaded_(false), is_fully_loaded_(false) {}

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
	CATALOG_DEBUG(1, "Scan('%s') — loading table names (no columns)", schema_.name.c_str());
	// Step 1: Ensure table names are loaded (no column queries)
	EnsureNamesLoaded(context);

	// Step 2: For each known name, ensure entry exists (loads columns on demand)
	auto &catalog = schema_.GetMSSQLCatalog();
	auto &cache = catalog.GetMetadataCache();
	auto &pool = catalog.GetConnectionPool();

	catalog.EnsureCacheLoaded(context);
	auto connection = pool.Acquire();
	if (!connection) {
		throw IOException("Failed to acquire connection for table scan");
	}

	CATALOG_DEBUG(1, "Scan('%s') — iterating %zu known tables, loading columns per table",
				 schema_.name.c_str(), known_table_names_.size());
	std::lock_guard<std::mutex> lock(entry_mutex_);
	for (const auto &table_name : known_table_names_) {
		// Skip if already loaded
		if (entries_.find(table_name) != entries_.end()) {
			callback(*entries_[table_name]);
			continue;
		}

		// Load columns for this table and create entry
		auto table_meta = cache.GetTableMetadata(*connection, schema_.name, table_name);
		if (table_meta) {
			auto entry = CreateTableEntry(*table_meta);
			if (entry) {
				auto &ref = *entry;
				entries_[entry->name] = std::move(entry);
				callback(ref);
			}
		}
	}

	pool.Release(std::move(connection));
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

	// Acquire connection for lazy loading
	auto connection = pool.Acquire();
	if (!connection) {
		throw IOException("Failed to acquire connection for table loading");
	}

	// Get metadata for this specific table (triggers lazy column loading)
	auto table_meta = cache.GetTableMetadata(*connection, schema_.name, name);

	pool.Release(std::move(connection));

	if (!table_meta) {
		// Table doesn't exist - mark as attempted so we don't retry
		std::lock_guard<std::mutex> lock(entry_mutex_);
		attempted_tables_.insert(name);
		return false;
	}

	// Create table entry and add to cache
	auto entry = CreateTableEntry(*table_meta);
	if (entry) {
		std::lock_guard<std::mutex> lock(entry_mutex_);
		entries_[entry->name] = std::move(entry);
		attempted_tables_.insert(name);
		return true;
	}

	return false;
}

bool MSSQLTableSet::IsLoaded() const {
	return is_fully_loaded_.load();
}

void MSSQLTableSet::Invalidate() {
	std::lock_guard<std::mutex> lock(load_mutex_);
	is_fully_loaded_.store(false);
	names_loaded_.store(false);
	{
		std::lock_guard<std::mutex> elock(entry_mutex_);
		entries_.clear();
		attempted_tables_.clear();
	}
	{
		std::lock_guard<std::mutex> nlock(names_mutex_);
		known_table_names_.clear();
	}
}

//===----------------------------------------------------------------------===//
// Internal Methods
//===----------------------------------------------------------------------===//

unique_ptr<MSSQLTableEntry> MSSQLTableSet::CreateTableEntry(const MSSQLTableMetadata &metadata) {
	return make_uniq<MSSQLTableEntry>(schema_.catalog, schema_, metadata);
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
	auto table_names = cache.GetTableNames(*connection, schema_.name);

	pool.Release(std::move(connection));

	for (const auto &name : table_names) {
		known_table_names_.insert(name);
	}
	CATALOG_DEBUG(1, "EnsureNamesLoaded('%s') — loaded %zu table names (no column queries)",
				 schema_.name.c_str(), known_table_names_.size());
	names_loaded_.store(true);
}

}  // namespace duckdb
