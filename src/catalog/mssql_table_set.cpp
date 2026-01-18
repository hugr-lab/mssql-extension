#include "catalog/mssql_table_set.hpp"
#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_schema_entry.hpp"
#include "catalog/mssql_table_entry.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Constructor
//===----------------------------------------------------------------------===//

MSSQLTableSet::MSSQLTableSet(MSSQLSchemaEntry &schema)
    : schema_(schema), is_loaded_(false) {
}

//===----------------------------------------------------------------------===//
// Entry Access
//===----------------------------------------------------------------------===//

optional_ptr<CatalogEntry> MSSQLTableSet::GetEntry(ClientContext &context, const string &name) {
	EnsureLoaded(context);

	std::lock_guard<std::mutex> lock(entry_mutex_);
	auto it = entries_.find(name);
	if (it == entries_.end()) {
		return nullptr;
	}
	return it->second.get();
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

void MSSQLTableSet::LoadEntries(ClientContext &context) {
	auto &catalog = schema_.GetMSSQLCatalog();
	auto &cache = catalog.GetMetadataCache();

	// Ensure cache is loaded
	catalog.EnsureCacheLoaded(context);

	// Get table names from cache
	auto table_names = cache.GetTableNames(schema_.name);

	std::lock_guard<std::mutex> lock(entry_mutex_);
	entries_.clear();

	for (const auto &table_name : table_names) {
		auto table_meta = cache.GetTableMetadata(schema_.name, table_name);
		if (!table_meta) {
			continue;
		}

		// Create table entry
		auto entry = CreateTableEntry(*table_meta);
		if (entry) {
			entries_[table_name] = std::move(entry);
		}
	}
}

bool MSSQLTableSet::IsLoaded() const {
	return is_loaded_.load();
}

void MSSQLTableSet::Invalidate() {
	std::lock_guard<std::mutex> lock(load_mutex_);
	is_loaded_.store(false);
	{
		std::lock_guard<std::mutex> entry_lock(entry_mutex_);
		entries_.clear();
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
	if (is_loaded_.load()) {
		return;
	}

	std::lock_guard<std::mutex> lock(load_mutex_);
	if (is_loaded_.load()) {
		return;
	}

	LoadEntries(context);
	is_loaded_.store(true);
}

}  // namespace duckdb
