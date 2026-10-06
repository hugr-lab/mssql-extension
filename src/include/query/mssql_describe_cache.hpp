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

//! Spec 079 PR E2: the shapes `mssql_scan` / `mssql_scan_params` were described
//! with, so a statement already described binds without the describe's round
//! trip (and without a connection). Per catalog. First the statements remote
//! pushdown sends; since issue #410 every describing scan (`Note`), because the
//! describe is most of what a short query costs: a TOP 1 took 3.5 ms where the
//! execution alone takes 1.1 (2.4 ms of compile on the server per describe).
//! A user's T-SQL can read what the catalog does not see change (a view, a
//! table altered by `mssql_exec` without mssql_exec_invalidate_cache): the
//! init-time check catches it like any other changed shape.
//!
//! An entry lives as the catalog's metadata does: until the metadata cache's
//! invalidation epoch moves (DDL through the catalog, mssql_invalidate_cache,
//! mssql_refresh_cache; mssql_preload_catalog clears it), or past
//! mssql_catalog_cache_ttl when that is set. The statement text carries no
//! constants (they are @pN parameters), so one entry serves every execution of
//! a shape. Inside an explicit transaction it is read and written only while
//! the transaction has changed no schema (MSSQLTransactionMetadata::HasChanged,
//! #383's rule). A shape changed outside the catalog (any client's DDL) is
//! caught by the init-time check, which drops every entry (`Clear`): that
//! statement fails once, saying to run it again, and the next one describes
//! again.
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

	//! A scan described `key`: its shape may be cached (Store follows).
	void Note(const std::string &key);
	//! The cached shape of `key`, if it is a noted statement described under
	//! `epoch` and not older than `ttl_seconds` (0: no limit).
	bool Lookup(const std::string &key, uint64_t epoch, int64_t ttl_seconds, CachedShape &out);
	//! Keeps `shape` for a noted statement; ignored for any other.
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
