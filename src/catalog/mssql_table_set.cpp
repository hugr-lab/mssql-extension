#include "catalog/mssql_table_set.hpp"
#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_schema_entry.hpp"
#include "catalog/mssql_table_entry.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

MSSQLTableSet::MSSQLTableSet(MSSQLSchemaEntry &schema) : schema_(schema), is_fully_loaded_(false) {}

//===----------------------------------------------------------------------===//
// Entry Access
//===----------------------------------------------------------------------===//

optional_ptr<CatalogEntry> MSSQLTableSet::GetEntry(ClientContext &context, const string &name) {
	// First check if already cached
	{
		std::lock_guard<std::mutex> lock(entry_mutex_);
		auto it = entries_.find(name);
		if (it != entries_.end()) {
			return it->second.get();
		}

		// If we've already tried to load this table and it wasn't found, return nullptr
		if (attempted_tables_.find(name) != attempted_tables_.end()) {
			return nullptr;
		}
	}

	// If fully loaded and not found, table doesn't exist
	if (is_fully_loaded_.load()) {
		return nullptr;
	}

	// Try to load just this single table
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
	EnsureLoaded(context);

	std::lock_guard<std::mutex> lock(entry_mutex_);
	for (const auto &pair : entries_) {
		callback(*pair.second);
	}
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

void MSSQLTableSet::LoadEntries(ClientContext &context) {
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

	// Get table names from cache (triggers lazy loading of table list)
	auto table_names = cache.GetTableNames(*connection, schema_.name);

	std::lock_guard<std::mutex> lock(entry_mutex_);
	// Don't clear entries_ - preserve already-loaded tables
	// But we need to load any tables we haven't seen yet

	for (const auto &table_name : table_names) {
		// Skip if already loaded
		if (entries_.find(table_name) != entries_.end()) {
			continue;
		}

		// GetTableMetadata triggers lazy column loading
		auto table_meta = cache.GetTableMetadata(*connection, schema_.name, table_name);
		if (!table_meta) {
			continue;
		}

		// Create table entry
		auto entry = CreateTableEntry(*table_meta);
		if (entry) {
			entries_[entry->name] = std::move(entry);
		}
	}

	pool.Release(std::move(connection));
}

bool MSSQLTableSet::IsLoaded() const {
	return is_fully_loaded_.load();
}

void MSSQLTableSet::Invalidate() {
	std::lock_guard<std::mutex> lock(load_mutex_);
	is_fully_loaded_.store(false);
	{
		std::lock_guard<std::mutex> entry_lock(entry_mutex_);
		entries_.clear();
		attempted_tables_.clear();
	}
}

//===----------------------------------------------------------------------===//
// Internal Methods
//===----------------------------------------------------------------------===//

unique_ptr<MSSQLTableEntry> MSSQLTableSet::CreateTableEntry(const MSSQLTableMetadata &metadata) {
	return make_uniq<MSSQLTableEntry>(schema_.catalog, schema_, metadata);
}

void MSSQLTableSet::EnsureLoaded(ClientContext &context) {
	// Double-checked locking pattern
	if (is_fully_loaded_.load()) {
		return;
	}

	std::lock_guard<std::mutex> lock(load_mutex_);
	if (is_fully_loaded_.load()) {
		return;
	}

	LoadEntries(context);
	is_fully_loaded_.store(true);
}

}  // namespace duckdb
