#include "pushdown/mssql_pushdown_resolution.hpp"

#include <vector>

#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_table_entry.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {
namespace mssql {

namespace {

struct ResolvedTable {
	const MSSQLCatalog *catalog = nullptr;
	std::string schema;
	std::string table;
	weak_ptr<MSSQLTableEntry> entry;
};

//! The most recent resolutions on this thread, newest last. Bounded: the
//! rewriter asks about a table right after resolving it, so only the last few
//! matter, and a bound keeps a long-lived worker thread from accumulating
//! notes for every table it ever bound.
constexpr size_t MAX_NOTES = 64;

std::vector<ResolvedTable> &Notes() {
	thread_local std::vector<ResolvedTable> notes;
	return notes;
}

}  // namespace

void NoteResolvedTable(const MSSQLCatalog &catalog, const shared_ptr<MSSQLTableEntry> &entry) {
	if (!entry) {
		return;
	}
	auto &notes = Notes();
	const std::string schema = entry->schema.name.GetIdentifierName();
	const std::string table = entry->name.GetIdentifierName();
	for (auto it = notes.begin(); it != notes.end(); ++it) {
		if (it->catalog == &catalog && it->schema == schema && it->table == table) {
			notes.erase(it);
			break;
		}
	}
	if (notes.size() >= MAX_NOTES) {
		notes.erase(notes.begin());
	}
	ResolvedTable note;
	note.catalog = &catalog;
	note.schema = schema;
	note.table = table;
	note.entry = entry;
	notes.push_back(std::move(note));
}

shared_ptr<MSSQLTableEntry> FindResolvedTable(const MSSQLCatalog &catalog, const std::string &schema,
											  const std::string &table) {
	auto &notes = Notes();
	for (auto it = notes.rbegin(); it != notes.rend(); ++it) {
		if (it->catalog != &catalog || it->table != table || (!schema.empty() && it->schema != schema)) {
			continue;
		}
		return it->entry.lock();
	}
	return nullptr;
}

}  // namespace mssql
}  // namespace duckdb
