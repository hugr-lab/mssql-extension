#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/unique_ptr.hpp"

namespace duckdb {

class MSSQLMetadataCache;
class MSSQLTableEntry;

//===----------------------------------------------------------------------===//
// MSSQLTransactionMetadata - what one explicit transaction knows about the
// catalog that the shared cache must not (issue #380)
//
// The catalog's metadata cache and table sets are shared by every connection
// of the database instance, so they may only ever hold COMMITTED state. Inside
// an explicit transaction the extension therefore:
//
//   * reads the shared cache, except for the schemas and tables this
//     transaction changed (MarkChanged) -- the shared copy of those describes
//     the committed state, not the transaction's;
//   * never writes it: a miss is loaded on the transaction's pinned connection
//     (which sees the transaction's own uncommitted DDL, where a pool
//     connection would block on its schema lock) into THIS object;
//   * at COMMIT or ROLLBACK, invalidates in the shared cache everything it
//     changed, and drops this object with the transaction.
//
// Loading into the shared cache instead was measured to leak: after ROLLBACK
// a table created in the transaction stayed bound as a phantom, and a second
// connection found the uncommitted table in the cache while the transaction
// was open.
//
// Thread-safe: DuckDB may bind on several threads of one transaction.
//===----------------------------------------------------------------------===//

class MSSQLTransactionMetadata {
public:
	//! `shared_epoch`: the shared cache's invalidation epoch when this
	//! transaction first needed metadata of its own (issue #383).
	MSSQLTransactionMetadata(unique_ptr<MSSQLMetadataCache> cache, uint64_t shared_epoch);
	~MSSQLTransactionMetadata();

	MSSQLTransactionMetadata(const MSSQLTransactionMetadata &) = delete;
	MSSQLTransactionMetadata &operator=(const MSSQLTransactionMetadata &) = delete;

	//! The transaction's own metadata cache: loads made on its pinned
	//! connection land here, never in the catalog's shared cache.
	MSSQLMetadataCache &Cache();

	//! A table entry this transaction loaded itself, or null.
	shared_ptr<MSSQLTableEntry> FindEntry(const string &schema, const string &table);
	//! True when this transaction looked the table up and it does not exist.
	bool IsAbsent(const string &schema, const string &table);
	//! Keep an entry built from this transaction's metadata; if one exists for
	//! the name already, that one is returned and `entry` dropped.
	shared_ptr<MSSQLTableEntry> AddEntry(const string &schema, const string &table, shared_ptr<MSSQLTableEntry> entry);
	void MarkAbsent(const string &schema, const string &table);
	//! A schema whose whole table listing this transaction loaded itself.
	bool IsSchemaListed(const string &schema);
	void MarkSchemaListed(const string &schema);

	//! This transaction changed the table / the schema / (with an empty schema)
	//! possibly anything: stop trusting the shared cache for it, and forget what
	//! this transaction had loaded for it.
	void MarkChanged(const string &schema, const string &table = string());
	//! What MarkChanged("") does for THIS transaction's lookups -- stop trusting
	//! the shared cache for anything, drop what was loaded -- without asking the
	//! shared cache to forget anything at the end. For DDL the extension cannot
	//! see through (mssql_exec under mssql_exec_invalidate_cache = false): the
	//! transaction must not bind, or learn a rowid key, from a shared entry its
	//! own uncommitted DDL may have changed; the shared cache is the user's to
	//! invalidate, as in autocommit (review of #382).
	void MarkChangedLocally();
	bool IsChanged(const string &schema, const string &table);
	bool IsSchemaChanged(const string &schema);
	//! MarkChanged with an empty schema was called: even the schema list may differ.
	bool IsAllChanged();

	//! The tables this transaction loaded itself: what is published into the
	//! shared cache once the transaction has ended (issue #383).
	std::set<std::pair<string, string>> GetLoadedTables();
	uint64_t SharedEpochAtStart() const {
		return shared_epoch_;
	}

	//! What the shared cache must forget when the transaction ends.
	struct Changes {
		bool all = false;
		std::set<string> schemas;
		std::set<std::pair<string, string>> tables;
	};
	Changes GetChanges();

private:
	using Key = std::pair<string, string>;

	std::mutex lock_;
	unique_ptr<MSSQLMetadataCache> cache_;
	std::map<Key, shared_ptr<MSSQLTableEntry>> entries_;
	std::set<Key> absent_;
	std::set<string> listed_schemas_;
	bool all_changed_ = false;
	bool locally_changed_ = false;
	std::set<string> changed_schemas_;
	std::set<Key> changed_tables_;
	const uint64_t shared_epoch_;
};

}  // namespace duckdb
