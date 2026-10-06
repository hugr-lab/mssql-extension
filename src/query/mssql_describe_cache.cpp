#include "query/mssql_describe_cache.hpp"

namespace duckdb {
namespace mssql {

std::string DescribeCache::Key(const std::string &statement, const std::string &declarations, bool native_types) {
	// The parts cannot run into each other: a NUL is in neither.
	std::string key;
	key.reserve(statement.size() + declarations.size() + 4);
	key += native_types ? 'n' : 'p';
	key += '\0';
	key += declarations;
	key += '\0';
	key += statement;
	return key;
}

void DescribeCache::Touch(Entry &entry) {
	recency_.splice(recency_.begin(), recency_, entry.recency);
}

void DescribeCache::Note(const std::string &key) {
	if (key.size() > MAX_STATEMENT) {
		return;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	auto found = entries_.find(key);
	if (found != entries_.end()) {
		Touch(found->second);
		return;
	}
	while (!recency_.empty() && (entries_.size() >= CAPACITY || bytes_ + key.size() > BYTES)) {
		bytes_ -= recency_.back().size();
		entries_.erase(recency_.back());
		recency_.pop_back();
	}
	bytes_ += key.size();
	recency_.push_front(key);
	Entry entry;
	entry.recency = recency_.begin();
	entries_.emplace(key, std::move(entry));
}

bool DescribeCache::Lookup(const std::string &key, uint64_t epoch, int64_t ttl_seconds, CachedShape &out) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto found = entries_.find(key);
	if (found == entries_.end() || !found->second.described) {
		return false;
	}
	auto &entry = found->second;
	const bool expired =
		ttl_seconds > 0 && std::chrono::steady_clock::now() - entry.stored > std::chrono::seconds(ttl_seconds);
	if (entry.epoch > epoch) {
		// Described after an invalidation this bind did not see yet: newer than
		// the asker, not stale -- kept for the next one.
		return false;
	}
	if (entry.epoch != epoch || expired) {
		entry.described = false;
		entry.shape = CachedShape();
		return false;
	}
	Touch(entry);
	out = entry.shape;
	return true;
}

void DescribeCache::Store(const std::string &key, uint64_t epoch, CachedShape shape) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto found = entries_.find(key);
	if (found == entries_.end()) {
		return;
	}
	auto &entry = found->second;
	if (entry.described && entry.epoch > epoch) {
		// A bind that read the epoch before an invalidation another bind has
		// already described past: keep the newer shape (review of #406).
		return;
	}
	entry.described = true;
	entry.epoch = epoch;
	entry.stored = std::chrono::steady_clock::now();
	entry.shape = std::move(shape);
	Touch(entry);
}

void DescribeCache::Clear() {
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &entry : entries_) {
		entry.second.described = false;
		entry.second.shape = CachedShape();
	}
}

void DescribeCache::Forget(const std::string &key) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto found = entries_.find(key);
	if (found != entries_.end()) {
		found->second.described = false;
		found->second.shape = CachedShape();
	}
}

}  // namespace mssql
}  // namespace duckdb
