#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include "catalog/mssql_metadata_cache.hpp"
#include "duckdb/catalog/catalog_entry.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Forward declarations
//===----------------------------------------------------------------------===//

class MSSQLSchemaEntry;
class MSSQLTableEntry;

//===----------------------------------------------------------------------===//
// MSSQLTableSet - Lazy-loaded set of tables/views in a schema
//
// Supports two loading strategies:
// 1. Single-table loading: GetEntry loads only the requested table
// 2. Full loading: Scan loads all tables in the schema
//
// This enables fast queries on specific tables while still supporting
// information_schema queries that need to enumerate all tables.
//===----------------------------------------------------------------------===//

class MSSQLTableSet {
public:
	// Constructor
	explicit MSSQLTableSet(MSSQLSchemaEntry &schema);

	~MSSQLTableSet() = default;

	// Non-copyable
	MSSQLTableSet(const MSSQLTableSet &) = delete;
	MSSQLTableSet &operator=(const MSSQLTableSet &) = delete;

	//===----------------------------------------------------------------------===//
	// Entry Access
	//===----------------------------------------------------------------------===//

	// Get table/view entry by name (loads only the requested table if not cached)
	optional_ptr<CatalogEntry> GetEntry(ClientContext &context, const string &name);

	// Scan all entries (loads all tables if not already fully loaded)
	void Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback);

	//===----------------------------------------------------------------------===//
	// Entry Loading
	//===----------------------------------------------------------------------===//

	// Load ALL entries from metadata cache (for Scan operations)
	void LoadEntries(ClientContext &context);

	// Load a single table entry by name (for GetEntry operations)
	// Returns true if the table exists and was loaded, false otherwise
	bool LoadSingleEntry(ClientContext &context, const string &name);

	// Check if ALL entries are loaded
	bool IsLoaded() const;

	// Invalidate cached entries (force reload on next access)
	void Invalidate();

private:
	//===----------------------------------------------------------------------===//
	// Internal Methods
	//===----------------------------------------------------------------------===//

	// Create table entry from metadata
	unique_ptr<MSSQLTableEntry> CreateTableEntry(const MSSQLTableMetadata &metadata);

	// Ensure ALL entries are loaded (called by Scan)
	void EnsureLoaded(ClientContext &context);

	//===----------------------------------------------------------------------===//
	// Member Variables
	//===----------------------------------------------------------------------===//

	MSSQLSchemaEntry &schema_;									  // Parent schema
	std::atomic<bool> is_fully_loaded_;							  // True when ALL tables are loaded
	std::mutex load_mutex_;										  // Loading synchronization
	std::mutex entry_mutex_;									  // Entry access synchronization
	unordered_map<string, unique_ptr<MSSQLTableEntry>> entries_;  // Cached entries
	std::unordered_set<string> attempted_tables_;				  // Tables we've tried to load (including non-existent)
};

}  // namespace duckdb
