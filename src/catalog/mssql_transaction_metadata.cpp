#include "catalog/mssql_transaction_metadata.hpp"

#include "catalog/mssql_metadata_cache.hpp"
#include "catalog/mssql_table_entry.hpp"

namespace duckdb {

MSSQLTransactionMetadata::MSSQLTransactionMetadata(unique_ptr<MSSQLMetadataCache> cache) : cache_(std::move(cache)) {}

MSSQLTransactionMetadata::~MSSQLTransactionMetadata() = default;

MSSQLMetadataCache &MSSQLTransactionMetadata::Cache() {
	return *cache_;
}

shared_ptr<MSSQLTableEntry> MSSQLTransactionMetadata::FindEntry(const string &schema, const string &table) {
	std::lock_guard<std::mutex> guard(lock_);
	auto it = entries_.find(Key(schema, table));
	return it == entries_.end() ? nullptr : it->second;
}

bool MSSQLTransactionMetadata::IsAbsent(const string &schema, const string &table) {
	std::lock_guard<std::mutex> guard(lock_);
	return absent_.count(Key(schema, table)) > 0;
}

shared_ptr<MSSQLTableEntry> MSSQLTransactionMetadata::AddEntry(const string &schema, const string &table,
															   shared_ptr<MSSQLTableEntry> entry) {
	std::lock_guard<std::mutex> guard(lock_);
	Key key(schema, table);
	absent_.erase(key);
	// Emplace-only: two threads of one transaction loading the same table keep
	// the first entry, so both bind to one object.
	auto result = entries_.emplace(key, std::move(entry));
	return result.first->second;
}

void MSSQLTransactionMetadata::MarkAbsent(const string &schema, const string &table) {
	std::lock_guard<std::mutex> guard(lock_);
	absent_.insert(Key(schema, table));
}

bool MSSQLTransactionMetadata::IsSchemaListed(const string &schema) {
	std::lock_guard<std::mutex> guard(lock_);
	return listed_schemas_.count(schema) > 0;
}

void MSSQLTransactionMetadata::MarkSchemaListed(const string &schema) {
	std::lock_guard<std::mutex> guard(lock_);
	listed_schemas_.insert(schema);
}

void MSSQLTransactionMetadata::MarkChanged(const string &schema, const string &table, bool dropped) {
	std::lock_guard<std::mutex> guard(lock_);
	if (schema.empty()) {
		all_changed_ = true;
		entries_.clear();
		absent_.clear();
		listed_schemas_.clear();
		cache_->Invalidate();
		return;
	}
	if (table.empty()) {
		changed_schemas_.insert(schema);
		for (auto it = entries_.begin(); it != entries_.end();) {
			it = it->first.first == schema ? entries_.erase(it) : std::next(it);
		}
		for (auto it = absent_.begin(); it != absent_.end();) {
			it = it->first == schema ? absent_.erase(it) : std::next(it);
		}
		listed_schemas_.erase(schema);
		cache_->InvalidateSchema(schema);
		return;
	}
	Key key(schema, table);
	changed_tables_.insert(key);
	if (dropped) {
		dropped_tables_.insert(key);
	} else {
		dropped_tables_.erase(key);
	}
	entries_.erase(key);
	absent_.erase(key);
	// The listing may have been taken before the table was created or dropped.
	listed_schemas_.erase(schema);
	cache_->InvalidateTable(schema, table);
	cache_->InvalidateSchemaTableList(schema);
}

void MSSQLTransactionMetadata::MarkChangedLocally() {
	std::lock_guard<std::mutex> guard(lock_);
	locally_changed_ = true;
	entries_.clear();
	absent_.clear();
	listed_schemas_.clear();
	cache_->Invalidate();
}

bool MSSQLTransactionMetadata::IsChanged(const string &schema, const string &table) {
	std::lock_guard<std::mutex> guard(lock_);
	return all_changed_ || locally_changed_ || changed_schemas_.count(schema) > 0 ||
		   changed_tables_.count(Key(schema, table)) > 0;
}

bool MSSQLTransactionMetadata::IsSchemaChanged(const string &schema) {
	std::lock_guard<std::mutex> guard(lock_);
	if (all_changed_ || locally_changed_ || changed_schemas_.count(schema) > 0) {
		return true;
	}
	// A changed table makes the schema's shared listing untrustworthy too: it may
	// lack a table the transaction created, or list one it dropped.
	for (const auto &key : changed_tables_) {
		if (key.first == schema) {
			return true;
		}
	}
	return false;
}

std::set<std::pair<string, string>> MSSQLTransactionMetadata::GetLoadedTables() {
	std::lock_guard<std::mutex> guard(lock_);
	std::set<Key> loaded;
	for (const auto &pair : entries_) {
		loaded.insert(pair.first);
	}
	return loaded;
}

bool MSSQLTransactionMetadata::IsAllChanged() {
	std::lock_guard<std::mutex> guard(lock_);
	return all_changed_ || locally_changed_;
}

MSSQLTransactionMetadata::Changes MSSQLTransactionMetadata::GetChanges() {
	std::lock_guard<std::mutex> guard(lock_);
	Changes changes;
	changes.all = all_changed_;
	changes.schemas = changed_schemas_;
	changes.tables = changed_tables_;
	changes.dropped = dropped_tables_;
	return changes;
}

}  // namespace duckdb
