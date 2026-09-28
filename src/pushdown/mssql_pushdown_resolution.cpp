#include "pushdown/mssql_pushdown_resolution.hpp"

#include <vector>

#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_table_entry.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {
namespace mssql {

namespace {

struct Note {
	const MSSQLCatalog *catalog = nullptr;
	std::string schema;
	std::string table;
	weak_ptr<MSSQLTableEntry> entry;
	weak_ptr<ClientContext> context;
};

//! The most recent resolutions on this thread, newest last. Bounded: the
//! rewriter asks about a table right after resolving it, so only the last few
//! matter, and a bound keeps a long-lived worker thread from accumulating
//! notes for every table it ever bound.
constexpr size_t MAX_NOTES = 64;

std::vector<Note> &Notes() {
	thread_local std::vector<Note> notes;
	return notes;
}

}  // namespace

void NoteResolvedTable(ClientContext &context, const MSSQLCatalog &catalog, const shared_ptr<MSSQLTableEntry> &entry) {
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
	Note note;
	note.catalog = &catalog;
	note.schema = schema;
	note.table = table;
	note.entry = entry;
	note.context = context.shared_from_this();
	notes.push_back(std::move(note));
}

ResolvedTable FindResolvedTable(const MSSQLCatalog &catalog, const std::string &schema, const std::string &table) {
	// The exact spelling first; then any spelling, as DuckDB's own lookup
	// matches names case-insensitively (`db.ORDERS` finds `Orders`).
	auto &notes = Notes();
	for (int exact = 1; exact >= 0; exact--) {
		for (auto it = notes.rbegin(); it != notes.rend(); ++it) {
			if (it->catalog != &catalog) {
				continue;
			}
			const bool match = exact
								   ? it->schema == schema && it->table == table
								   : StringUtil::CIEquals(it->schema, schema) && StringUtil::CIEquals(it->table, table);
			if (!match) {
				continue;
			}
			ResolvedTable found;
			found.entry = it->entry.lock();
			found.context = it->context.lock();
			return found;
		}
	}
	return ResolvedTable();
}

}  // namespace mssql
}  // namespace duckdb
