// Spec 079 PR E2, issue #410: the describe cache of mssql_scan shapes -- only noted
// statements are kept, an entry dies with the invalidation epoch or the TTL,
// the least recently used goes past the capacity, and Forget drops a shape.

#include "query/mssql_describe_cache.hpp"

#include <iostream>
#include <thread>

using namespace duckdb;
using namespace duckdb::mssql;

static int failures = 0;

static void Expect(bool condition, const char *what) {
	if (!condition) {
		std::cerr << "FAIL: " << what << "\n";
		failures++;
	}
}

static CachedShape Shape(const char *name) {
	CachedShape shape;
	shape.types.push_back(LogicalType::INTEGER);
	shape.names.push_back(name);
	shape.datetime2.push_back(false);
	return shape;
}

int main() {
	const auto key = DescribeCache::Key("SELECT [a] FROM [dbo].[t]", "", true);
	Expect(key != DescribeCache::Key("SELECT [a] FROM [dbo].[t]", "", false), "native types are part of the key");
	Expect(key != DescribeCache::Key("SELECT [a] FROM [dbo].[t]", "@p0 int", true), "declarations are part of the key");
	{
		DescribeCache cache;
		CachedShape out;
		Expect(!cache.Lookup(key, 1, 0, out), "not described yet");
		cache.Store(key, 1, Shape("a"));
		Expect(cache.Lookup(key, 1, 0, out) && out.names.size() == 1 && out.names[0] == "a", "described: kept");
		Expect(!cache.Lookup(key, 2, 0, out), "the epoch moved: gone");
		Expect(!cache.Lookup(key, 1, 0, out), "and gone for good, not back at the old epoch");
		cache.Store(key, 2, Shape("a"));
		Expect(cache.Lookup(key, 2, 0, out), "described again at the new epoch");
		cache.Forget(key);
		Expect(!cache.Lookup(key, 2, 0, out), "forgotten after a shape mismatch");
		cache.Store(key, 2, Shape("a"));
		Expect(cache.Lookup(key, 2, 0, out), "described again after Forget");
		cache.Store(key, 3, Shape("a"));
		Expect(!cache.Lookup(key, 2, 0, out), "described after an invalidation the asker has not seen: not served");
		Expect(cache.Lookup(key, 3, 0, out), "... and not wiped either");
		cache.Store(key, 2, Shape("stale"));
		Expect(cache.Lookup(key, 3, 0, out) && out.names[0] == "a",
			   "a slower bind's older shape does not overwrite a newer one");
		cache.Clear();
		Expect(!cache.Lookup(key, 3, 0, out), "cleared");
		const auto long_key = DescribeCache::Key(std::string(DescribeCache::MAX_STATEMENT + 1, 'x'), "", true);
		cache.Store(long_key, 3, Shape("a"));
		Expect(!cache.Lookup(long_key, 3, 0, out), "a statement past MAX_STATEMENT is not remembered");
	}
	{
		DescribeCache cache;
		CachedShape out;
		cache.Store(key, 1, Shape("a"));
		std::this_thread::sleep_for(std::chrono::milliseconds(1100));
		Expect(!cache.Lookup(key, 1, 1, out), "older than the TTL: gone");
	}
	{
		DescribeCache cache;
		CachedShape out;
		for (size_t i = 0; i <= DescribeCache::CAPACITY; i++) {
			const auto k = DescribeCache::Key("SELECT " + std::to_string(i), "", true);
			cache.Store(k, 1, Shape("x"));
			if (i == 0) {
				continue;
			}
			// Keep the first one recent: it must survive the overflow.
			cache.Lookup(DescribeCache::Key("SELECT 0", "", true), 1, 0, out);
		}
		Expect(cache.Lookup(DescribeCache::Key("SELECT 0", "", true), 1, 0, out), "the recently used one survives");
		Expect(!cache.Lookup(DescribeCache::Key("SELECT 1", "", true), 1, 0, out), "the least recently used one went");
		Expect(
			cache.Lookup(DescribeCache::Key("SELECT " + std::to_string(DescribeCache::CAPACITY), "", true), 1, 0, out),
			"the newest one is kept");
	}
	if (failures == 0) {
		std::cout << "test_describe_cache: all passed\n";
		return 0;
	}
	std::cerr << failures << " failure(s)\n";
	return 1;
}
