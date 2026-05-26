#pragma once

#include <atomic>
#include <condition_variable>
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
// Uses two-level loading to avoid eagerly loading column metadata:
// Level 1 (Names): EnsureNamesLoaded() loads only table names (fast, no column queries)
// Level 2 (Entries): GetEntry() creates entries on-demand with full columns
//
// This enables fast queries on specific tables in large databases (65K+ tables)
// while still supporting SHOW TABLES and information_schema queries.
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
	// Spec 052: returns shared_ptr — entries co-owned by entries_ map and any
	// in-flight bind data anchor (MSSQLCatalogScanBindData::table_entry_anchor_).
	shared_ptr<MSSQLTableEntry> CreateTableEntry(const MSSQLTableMetadata &metadata);

	// Ensure table names are loaded (no column queries, fast)
	void EnsureNamesLoaded(ClientContext &context);

	//===----------------------------------------------------------------------===//
	// Member Variables
	//===----------------------------------------------------------------------===//

	MSSQLSchemaEntry &schema_;	// Parent schema

	// Level 1: Table names only (fast, no column queries)
	std::unordered_set<string> known_table_names_;	// Names of all tables in schema
	std::atomic<bool> names_loaded_;				// True when table names are loaded
	std::mutex names_mutex_;						// Names loading synchronization

	// Level 2: Full entries with columns (created on demand)
	std::atomic<bool> is_fully_loaded_;							  // True when ALL tables are loaded
	std::mutex load_mutex_;										  // Loading synchronization
	std::mutex entry_mutex_;									  // Entry access synchronization
	// Spec 052: shared_ptr (was unique_ptr) so retired entries can flow into
	// MSSQLCatalog::table_graveyard_ on Invalidate() while in-flight binders
	// retain validity. Insertion is emplace-only (winner wins on race).
	unordered_map<string, shared_ptr<MSSQLTableEntry>> entries_;
	std::unordered_set<string> attempted_tables_;  // Tables we've tried to load (including non-existent)

	// Spec 052 singleflight: per-table load coordination so concurrent
	// first-loads of the same table issue only ONE SQL Server round trip.
	// Owner thread inserts `name` into loads_in_progress_, releases entry_mutex_
	// for the fetch, then re-acquires and erases on completion. Waiters block
	// on load_cv_ (over entry_mutex_) and re-check the cache after wake-up.
	// Different tables can still load in parallel.
	std::condition_variable load_cv_;
	std::unordered_set<string> loads_in_progress_;
};

}  // namespace duckdb
