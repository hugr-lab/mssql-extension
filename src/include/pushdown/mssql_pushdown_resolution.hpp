#pragma once

#include <string>

#include "duckdb/common/shared_ptr.hpp"

namespace duckdb {

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
void NoteResolvedTable(const MSSQLCatalog &catalog, const shared_ptr<MSSQLTableEntry> &entry);

//! The entry most recently resolved on this thread for `table` in `catalog`,
//! in `schema` when one is given (an empty schema matches any), or null.
shared_ptr<MSSQLTableEntry> FindResolvedTable(const MSSQLCatalog &catalog, const std::string &schema,
											  const std::string &table);

}  // namespace mssql
}  // namespace duckdb
