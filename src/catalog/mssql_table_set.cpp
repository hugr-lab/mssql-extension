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
		// Issue #178 review: RE-CHECK the flag under the lock. The unlocked read
		// above can see TRUE while a concurrent Invalidate() (flag already
		// stored false, names clear pending on this mutex) is about to empty
		// the set — treating "flag true + empty set" as authoritative would
		// poison attempted_tables_ with a permanent negative for an existing
		// table. Invalidate stores the flag BEFORE clearing, so a false read
		// here reliably routes us to the load path instead.
		if (names_loaded_.load() && known_table_names_.find(name) == known_table_names_.end()) {
			CATALOG_DEBUG(2, "  -> not in known_table_names_ (%zu names loaded)", known_table_names_.size());
			std::lock_guard<std::mutex> elock(entry_mutex_);
			attempted_tables_.insert(name);
			return nullptr;
		}
	}

	// 5. Load single entry with full metadata (columns included). LoadSingleEntry
	// hands the freshly built entry back via out param when it skipped publishing
	// it into entries_ (an Invalidate raced the fetch) — this query still gets a
	// consistent entry; the next bind reloads fresh metadata.
	CATALOG_DEBUG(1, "  -> loading columns for '%s.%s' (single table)", schema_.name.c_str(), name.c_str());
	shared_ptr<MSSQLTableEntry> loaded;
	if (LoadSingleEntry(context, name, &loaded)) {
		if (loaded) {
			anchored = std::move(loaded);
		} else {
			std::lock_guard<std::mutex> lock(entry_mutex_);
			auto it = entries_.find(name);
			if (it != entries_.end()) {
				anchored = it->second;	// shared_ptr copy under lock
			}
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

	// Issue #178 review: three passes so the table-set locks (names_mutex_ /
	// entry_mutex_) are NEVER held while blocking on the cache-wide mutex —
	// another thread can hold that across a metadata round trip, and nesting
	// them stalled GetEntry's fast path (SELECTs on already-cached tables) for
	// the full round trip.

	// Pass A: snapshot which entries already exist (brief entry_mutex_ hold).
	std::unordered_set<string> have_entries;
	{
		std::lock_guard<std::mutex> lock(entry_mutex_);
		for (const auto &pair : entries_) {
			have_entries.insert(pair.first);
		}
	}

	// Pass B: walk the cache under ONLY the cache mutex. Collect the name list,
	// preload statistics, and COPY metadata for tables that need new entries
	// (CreateTableEntry copies it anyway, so this adds no asymptotic cost).
	vector<string> names_list;
	vector<MSSQLTableMetadata> to_create;
	cache.ForEachTableInSchema(schema_.name, [&](const string &table_name, const MSSQLTableMetadata &table_meta) {
		stats_provider.PreloadRowCount(schema_.name, table_name, table_meta.approx_row_count);
		names_list.push_back(table_name);
		if (have_entries.find(table_name) == have_entries.end()) {
			to_create.push_back(table_meta);
		}
	});

	// Pass C: fill entries_/known_table_names_ under names_mutex_ + entry_mutex_
	// (taken in GetEntry's step-4 nesting order: names before entry). We collect
	// shared_ptr refs rather than invoking callback() here so the locks are
	// released before user code runs — callbacks frequently re-enter
	// LoadSingleEntry paths that need entry_mutex_ (spec 052 singleflight).
	//
	// Issue #178 review: the persistent inserts are epoch-guarded too. If an
	// Invalidate() landed after our metadata load, emplacing here would
	// resurrect pre-invalidation entries that GetEntry's step-1 fast path then
	// serves indefinitely. On a dirty epoch we still build entries for THIS
	// call's callback (anchored, stale-but-consistent listing) but persist
	// nothing — the next bind reloads fresh metadata.
	vector<shared_ptr<MSSQLTableEntry>> snapshot;
	{
		std::lock_guard<std::mutex> nlock(names_mutex_);
		std::lock_guard<std::mutex> lock(entry_mutex_);
		const bool epoch_clean = invalidation_epoch_.load() == epoch_at_start;

		for (auto &table_meta : to_create) {
			auto entry = CreateTableEntry(table_meta);
			if (!entry) {
				continue;
			}
			if (epoch_clean) {
				// Spec 052 T007: emplace-only (winner wins on race vs
				// LoadSingleEntry). C++11-compatible pair access (CLAUDE.md
				// ODR rule: no structured bindings).
				auto insert_result = entries_.emplace(entry->name, std::move(entry));
				snapshot.push_back(insert_result.first->second);
			} else {
				snapshot.push_back(std::move(entry));
			}
		}
		for (const auto &table_name : names_list) {
			if (have_entries.find(table_name) != have_entries.end()) {
				auto it = entries_.find(table_name);
				if (it != entries_.end()) {
					snapshot.push_back(it->second);
				}
				// else: invalidated between passes — omit from this listing;
				// the epoch guard below keeps the flags unpublished.
			}
		}
		if (epoch_clean) {
			for (const auto &table_name : names_list) {
				known_table_names_.insert(table_name);
			}
		}
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

bool MSSQLTableSet::LoadSingleEntry(ClientContext &context, const string &name,
									shared_ptr<MSSQLTableEntry> *out_entry) {
	auto &catalog = schema_.GetMSSQLCatalog();
	auto &cache = catalog.GetMetadataCache();
	auto &pool = catalog.GetConnectionPool();

	// Ensure cache settings are loaded (sets TTL)
	catalog.EnsureCacheLoaded(context);

	// Spec 052 singleflight (FR-007): coordinate concurrent first-loads of the
	// same table so only ONE thread issues the SQL Server round trip. Waiters
	// re-check the cache after the owner finishes; on cache hit they return
	// the owner's entry without a redundant fetch.
	uint64_t epoch_at_start;
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
		// Issue #178 review: snapshot the invalidation epoch for the
		// publication guard below. Captured under entry_mutex_ so any
		// invalidation whose clears already ran is reflected here.
		epoch_at_start = invalidation_epoch_.load();
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
		// Issue #178 review: metadata is COPIED out of the cache under its
		// mutex (see GetTableMetadata contract) — the old raw-pointer return
		// was dereferenced here after the lock was released, racing
		// Refresh/LoadAllTableMetadata/TTL reloads that free the map node.
		MSSQLTableMetadata table_meta;
		bool found = false;
		try {
			found = cache.GetTableMetadata(*connection, schema_.name, name, table_meta);
		} catch (...) {
			pool.Release(std::move(connection));
			throw;
		}
		pool.Release(std::move(connection));

		if (found) {
			table_exists = true;
			fetched_entry = CreateTableEntry(table_meta);
		}
	} catch (...) {
		propagated = std::current_exception();
	}

	// Publish results + release the load slot. attempted_tables_ is set on
	// success and on confirmed-not-exists; on exception it is NOT set so the
	// next caller retries the fetch.
	//
	// Issue #178 review: publication is epoch-guarded. If an Invalidate()
	// landed after our snapshot, emplacing would resurrect a pre-invalidation
	// entry (and a stale not-exists verdict would poison attempted_tables_).
	// On a dirty epoch nothing is persisted; the fetched entry is still handed
	// to THIS caller via out_entry, and the next bind reloads fresh metadata.
	{
		std::unique_lock<std::mutex> lock(entry_mutex_);
		const bool epoch_clean = invalidation_epoch_.load() == epoch_at_start;
		if (epoch_clean && table_exists && fetched_entry) {
			entries_.emplace(fetched_entry->name, fetched_entry);
			attempted_tables_.insert(name);
		} else if (epoch_clean && !propagated && !table_exists) {
			attempted_tables_.insert(name);
		}
		loads_in_progress_.erase(name);
	}
	load_cv_.notify_all();

	if (propagated) {
		std::rethrow_exception(propagated);
	}
	if (out_entry && fetched_entry) {
		*out_entry = std::move(fetched_entry);
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

}  // namespace duckdb
