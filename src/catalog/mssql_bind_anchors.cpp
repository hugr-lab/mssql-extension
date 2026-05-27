#include "catalog/mssql_bind_anchors.hpp"

#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_schema_entry.hpp"
#include "catalog/mssql_table_entry.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

void MSSQLBindAnchors::QueryEnd(ClientContext &context) {
	// Drop both anchor vectors. If entries_ already dropped its shared_ptr
	// via a concurrent Invalidate during this query, the underlying entry
	// is freed RIGHT HERE (this is the destructor moment user asked for —
	// "bind ends → ref goes away → record dies"). If entries_ still holds
	// its shared_ptr, our drop just decrements refcount; entry stays alive
	// in the live cache for future binds.
	//
	// Held mutex briefly because two threads could in principle race on
	// QueryEnd vs Anchor* if the same ClientContext fan-outs (it shouldn't
	// per DuckDB single-context-per-thread, but cheap to be safe).
	std::lock_guard<std::mutex> lock(mutex_);
	table_anchors_.clear();
	schema_anchors_.clear();
}

void MSSQLBindAnchors::AnchorTable(shared_ptr<MSSQLTableEntry> entry) {
	if (!entry) {
		return;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	table_anchors_.push_back(std::move(entry));
}

void MSSQLBindAnchors::AnchorSchema(shared_ptr<MSSQLSchemaEntry> entry) {
	if (!entry) {
		return;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	schema_anchors_.push_back(std::move(entry));
}

size_t MSSQLBindAnchors::TableAnchorCount() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return table_anchors_.size();
}

size_t MSSQLBindAnchors::SchemaAnchorCount() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return schema_anchors_.size();
}

MSSQLBindAnchors &MSSQLBindAnchors::For(ClientContext &context, const MSSQLCatalog &catalog) {
	// Per-catalog key ensures two ATTACH'es on the same connection get
	// separate anchor lists (lifetimes are independent).
	static const string KEY_PREFIX = "mssql_bind_anchors:";
	const string key = KEY_PREFIX + catalog.GetContextName();
	auto state = context.registered_state->GetOrCreate<MSSQLBindAnchors>(key);
	return *state;
}

}  // namespace duckdb
