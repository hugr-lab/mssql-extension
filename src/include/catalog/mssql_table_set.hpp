#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "catalog/mssql_metadata_cache.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace duckdb {

//===----------------------------------------------------------------------===//
// Forward declarations
//===----------------------------------------------------------------------===//

class MSSQLSchemaEntry;
class MSSQLTableEntry;

//===----------------------------------------------------------------------===//
// MSSQLTableSet - Lazy-loaded set of tables/views in a schema
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

	// Get table/view entry by name (loads if not cached)
	optional_ptr<CatalogEntry> GetEntry(ClientContext &context, const string &name);

	// Scan all entries (loads all if not cached)
	void Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback);

	//===----------------------------------------------------------------------===//
	// Entry Loading
	//===----------------------------------------------------------------------===//

	// Load entries from metadata cache
	void LoadEntries(ClientContext &context);

	// Check if entries are loaded
	bool IsLoaded() const;

	// Invalidate cached entries (force reload on next access)
	void Invalidate();

private:
	//===----------------------------------------------------------------------===//
	// Internal Methods
	//===----------------------------------------------------------------------===//

	// Create table entry from metadata
	unique_ptr<MSSQLTableEntry> CreateTableEntry(const MSSQLTableMetadata &metadata);

	// Ensure entries are loaded
	void EnsureLoaded(ClientContext &context);

	//===----------------------------------------------------------------------===//
	// Member Variables
	//===----------------------------------------------------------------------===//

	MSSQLSchemaEntry &schema_;                                        // Parent schema
	std::atomic<bool> is_loaded_;                                     // Load state
	std::mutex load_mutex_;                                           // Loading synchronization
	std::mutex entry_mutex_;                                          // Entry access synchronization
	unordered_map<string, unique_ptr<MSSQLTableEntry>> entries_;      // Cached entries
};

}  // namespace duckdb
