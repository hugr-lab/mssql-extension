#pragma once

#include <chrono>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "duckdb/common/types.hpp"

namespace duckdb {
namespace mssql {

//! What sp_describe_first_result_set told a bind: the result's names and
//! types, and which columns are datetime2.
struct CachedShape {
	std::vector<LogicalType> types;
	std::vector<std::string> names;
	std::vector<bool> datetime2;
};

//! Spec 079 PR E2: the shapes of the statements remote pushdown sends, so a
//! statement of a shape already described binds without the describe's round
//! trip (and without a connection). Per catalog. Only the statements the
//! rewriter wrote are cached (`NotePushed`; a user's `mssql_scan` of the very
//! same text shares the entry): a user's own `mssql_scan` keeps asking, as it
//! always has -- DDL through `mssql_exec` does not invalidate the catalog by
//! default, and a stale shape would turn a query that worked into the
//! init-time shape error.
//!
//! An entry lives as the catalog's metadata does: until the metadata cache's
//! invalidation epoch moves (DDL through the catalog, mssql_invalidate_cache,
//! mssql_refresh_cache; mssql_preload_catalog clears it), or past
//! mssql_catalog_cache_ttl when that is set. The statement text carries no
//! constants (they are @pN parameters), so one entry serves every execution of
//! a shape. A shape changed behind the catalog's
//! back is caught by the init-time check, which drops every entry (`Clear`):
//! that statement fails once, the next one describes again.
class DescribeCache {
public:
	//! The number of statements remembered; the least recently used goes.
	static constexpr size_t CAPACITY = 1024;
	//! A statement longer than this is not remembered (a literal IN list makes
	//! every value set a shape of its own), and the remembered texts together
	//! stay under BYTES.
	static constexpr size_t MAX_STATEMENT = 16 * 1024;
	static constexpr size_t BYTES = 8 * 1024 * 1024;

	static std::string Key(const std::string &statement, const std::string &declarations, bool native_types);

	//! The rewriter is about to send `key`: its shape may be cached.
	void NotePushed(const std::string &key);
	//! The cached shape of `key`, if it is a pushed statement described under
	//! `epoch` and not older than `ttl_seconds` (0: no limit).
	bool Lookup(const std::string &key, uint64_t epoch, int64_t ttl_seconds, CachedShape &out);
	//! Keeps `shape` for a pushed statement; ignored for any other.
	void Store(const std::string &key, uint64_t epoch, CachedShape shape);
	void Forget(const std::string &key);
	//! Drops every shape (a mismatch at execution, mssql_preload_catalog).
	void Clear();

private:
	struct Entry {
		bool described = false;
		uint64_t epoch = 0;
		std::chrono::steady_clock::time_point stored;
		CachedShape shape;
		std::list<std::string>::iterator recency;
	};
	void Touch(Entry &entry);

	std::mutex mutex_;
	std::unordered_map<std::string, Entry> entries_;
	//! Most recently used first.
	std::list<std::string> recency_;
	size_t bytes_ = 0;
};

}  // namespace mssql
}  // namespace duckdb
