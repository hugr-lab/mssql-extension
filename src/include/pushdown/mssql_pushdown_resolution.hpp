#pragma once

#include <string>

#include "duckdb/common/shared_ptr.hpp"

namespace duckdb {

class ClientContext;
class MSSQLCatalog;
class MSSQLTableEntry;

namespace mssql {

//! Spec 079 § 0.1: the remote-pushdown rewriter's dry run
//! (Catalog::SupportsPushdown) takes no ClientContext, so it cannot look a
//! table up itself -- and a lookup by name without a context would read the
//! shared cache and miss the transaction's own layer (#380: a table the
//! transaction created or altered). But the rewriter resolves every base table
//! through Catalog::GetEntry(context, ...) first, on the same thread, just
//! before asking SupportsPushdown about it. So every entry MSSQLTableSet::GetEntry
//! hands out is noted here, thread-locally, and the dry run reads the one the
//! rewriter itself resolved.
//!
//! Weak references: an entry lives while the statement binding it anchors it
//! (MSSQLBindAnchors); a note that outlives that is simply not found.
//!
//! The context that resolved it is noted with it: the dry run has none of its
//! own, and the statement's defaults (default_order, default_null_order) and
//! the extension's settings are that context's.
void NoteResolvedTable(ClientContext &context, const MSSQLCatalog &catalog, const shared_ptr<MSSQLTableEntry> &entry);

struct ResolvedTable {
	shared_ptr<MSSQLTableEntry> entry;
	shared_ptr<ClientContext> context;
	explicit operator bool() const {
		return entry && context;
	}
};

//! The entry most recently resolved on this thread for `schema`.`table` in
//! `catalog` (the exact spelling first, then any case), with the context that
//! resolved it; empty when there is none or either is gone.
ResolvedTable FindResolvedTable(const MSSQLCatalog &catalog, const std::string &schema, const std::string &table);

}  // namespace mssql
}  // namespace duckdb
