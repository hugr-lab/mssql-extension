#include "catalog/mssql_table_set.hpp"
#include <cstdio>
#include <cstdlib>
#include "catalog/mssql_bind_anchors.hpp"
#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_catalog_filter.hpp"
#include "catalog/mssql_schema_entry.hpp"
#include "catalog/mssql_statistics.hpp"
#include "catalog/mssql_table_entry.hpp"
#include "duckdb/common/exception.hpp"

// Debug logging for catalog operations
static int GetCatalogDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
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

	// Spec 052 Option D: stash a copy of the shared_ptr in the per-context
	// MSSQLBindAnchors; it's released at QueryEnd. This keeps the entry alive
	// from LookupEntry's return through any binder dereferences AND through
	// GetScanFunction (no separate bind_data anchor needed). If a concurrent
	// Invalidate moves entries_ to nothing, our anchor still holds the entry
	// for the rest of this query.
	shared_ptr<MSSQLTableEntry> anchored;

	// 1. Check cached entries (fast path)
	{
		std::lock_guard<std::mutex> lock(entry_mutex_);
		auto it = entries_.find(name);
		if (it != entries_.end()) {
			CATALOG_DEBUG(2, "  -> cache hit for '%s'", name.c_str());
			anchored = it->second;	// shared_ptr copy under lock — refcount inc
		} else if (attempted_tables_.find(name) != attempted_tables_.end()) {
			CATALOG_DEBUG(2, "  -> already attempted, not found");
			return nullptr;
		}
	}
	if (anchored) {
		MSSQLBindAnchors::For(context, schema_.GetMSSQLCatalog()).AnchorTable(anchored);
		return anchored.get();
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
			anchored = it->second;	// shared_ptr copy under lock
		}
	}
	if (anchored) {
		MSSQLBindAnchors::For(context, schema_.GetMSSQLCatalog()).AnchorTable(anchored);
		return anchored.get();
	}
	return nullptr;
}

void MSSQLTableSet::Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback) {
	CATALOG_DEBUG(1, "Scan('%s') — bulk loading all table metadata", schema_.name.c_str());

	// Issue #178: snapshot the invalidation epoch BEFORE loading; the trailing
	// names_loaded_/is_fully_loaded_ publication is skipped if it changed.
	const uint64_t epoch_at_start = invalidation_epoch_.load();

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

	// Phase 1: populate entries_ under entry_mutex_ and capture shared_ptr
	// references for the callback phase. We collect refs rather than invoking
	// callback() here so the lock can be released before user code runs —
	// callbacks frequently call back into DuckDB binder paths that resolve
	// virtual columns or sibling tables, which would re-enter LoadSingleEntry
	// and block on entry_mutex_ (spec 052 singleflight grabs it first).
	// shared_ptr copies in the snapshot keep entries alive even if a sibling
	// thread fires Invalidate() between phase 1 and phase 2.
	//
	// Issue #178 (D7): known_table_names_ is guarded by names_mutex_ everywhere
	// else (GetEntry, EnsureNamesLoaded, Invalidate*); the insert below used to
	// run under entry_mutex_ only — TSan caught it racing Invalidate()'s clear.
	// names_mutex_ is taken BEFORE entry_mutex_ to match GetEntry's nesting
	// order (its step-4 block acquires entry_mutex_ while holding names_mutex_).
	vector<shared_ptr<MSSQLTableEntry>> snapshot;
	{
		std::lock_guard<std::mutex> nlock(names_mutex_);
		std::lock_guard<std::mutex> lock(entry_mutex_);
		cache.ForEachTableInSchema(schema_.name, [&](const string &table_name, const MSSQLTableMetadata &table_meta) {
			stats_provider.PreloadRowCount(schema_.name, table_name, table_meta.approx_row_count);
			known_table_names_.insert(table_name);

			auto it = entries_.find(table_name);
			if (it != entries_.end()) {
				snapshot.push_back(it->second);
				return;
			}

			auto entry = CreateTableEntry(table_meta);
			if (entry) {
				// Spec 052 T007: emplace-only (winner wins on race
				// vs LoadSingleEntry). C++11-compatible pair access
				// (CLAUDE.md ODR rule: no structured bindings).
				auto insert_result = entries_.emplace(entry->name, std::move(entry));
				snapshot.push_back(insert_result.first->second);
			}
		});
	}

	// Spec 052 (Option D): anchor each entry in the per-ClientContext
	// MSSQLBindAnchors BEFORE calling the user callback. The local snapshot
	// only keeps entries alive for the duration of this Scan call, but
	// DuckDB's catalog walkers (duckdb_tables, duckdb_schemas, etc.) collect
	// raw pointers via callback in PHASE 1, then read columns / properties
	// from those pointers in PHASE 2 after Scan returns. Without anchoring
	// into BindAnchors, a concurrent Invalidate between phase 1 and phase 2
	// turns those pointers into UAFs (ASan-caught in CI scenario 6 against
	// duckdb_tables.cpp:111).
	auto &bind_anchors = MSSQLBindAnchors::For(context, schema_.GetMSSQLCatalog());

	// Phase 2: callback runs outside entry_mutex_. Snapshot holds shared_ptr
	// so entries cannot disappear even if Invalidate() retires them mid-loop;
	// BindAnchors then keeps them alive for the rest of the query (= until
	// QueryEnd).
	for (auto &entry_ptr : snapshot) {
		bind_anchors.AnchorTable(entry_ptr);
		callback(*entry_ptr);
	}

	// Issue #178: publish the loaded flags ONLY if no Invalidate() landed since
	// we started filling. An unconditional store here stomped a concurrent
	// invalidation: flags TRUE over just-cleared containers made GetEntry
	// answer "not found" for existing tables. load_mutex_ excludes
	// Invalidate/InvalidateEntry while we compare-and-publish.
	{
		std::lock_guard<std::mutex> llock(load_mutex_);
		if (invalidation_epoch_.load() == epoch_at_start) {
			names_loaded_.store(true);
			is_fully_loaded_.store(true);
		}
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
	invalidation_epoch_.fetch_add(1);  // Issue #178: defeats a concurrent Scan's trailing flag publication
	is_fully_loaded_.store(false);
	names_loaded_.store(false);
	// Spec 052 (Option D): just clear entries_. In-flight binders that
	// looked up an entry BEFORE this Invalidate are already anchored in
	// their ClientContext's MSSQLBindAnchors (per the LookupEntry / GetEntry
	// path that auto-anchors). Dropping our shared_ptr here decrements
	// refcount, but the anchor's shared_ptr keeps the underlying entry alive
	// until QueryEnd. New binds resolve fresh metadata (FR-006).
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

void MSSQLTableSet::InvalidateEntry(const string &name) {
	// Mirrors Invalidate()'s lock order (load_mutex_ -> entry_mutex_ -> names_mutex_) but
	// evicts only `name`. Force the (cheap) name list to be re-checked so a CREATE/DROP/RENAME
	// of this table is reflected on next access; every OTHER table's entry (and its cached
	// column metadata) is preserved, so the expensive per-table metadata is not re-fetched.
	std::lock_guard<std::mutex> lock(load_mutex_);
	invalidation_epoch_.fetch_add(1);  // Issue #178: same guard as Invalidate()
	is_fully_loaded_.store(false);
	names_loaded_.store(false);
	{
		std::lock_guard<std::mutex> elock(entry_mutex_);
		entries_.erase(name);
		attempted_tables_.erase(name);
	}
	{
		std::lock_guard<std::mutex> nlock(names_mutex_);
		known_table_names_.erase(name);
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
