// ============================================================================
// Spec 052 Option D — per-ClientContext bind-lifetime anchors for catalog
// entries. Replaces the per-catalog graveyard with a per-query holder of
// shared_ptr that releases at QueryEnd (= bind+execute done).
//
// Problem: DuckDB's catalog API returns optional_ptr<CatalogEntry> (non-owning
// raw pointer). The window between LookupEntry returning the raw pointer and
// our extension code (GetScanFunction / PlanInsert / etc.) running creates a
// UAF if another thread Invalidate()s the entry in that window.
//
// Solution: at LookupEntry time (our override), stash the shared_ptr<entry>
// into a per-ClientContext anchors list. DuckDB calls QueryEnd on every
// ClientContextState at end of each query — that's where we drop the list.
// While the query runs, the anchor keeps the entry alive even if entries_
// dropped its ref via a concurrent Invalidate.
//
// Per-context (not per-catalog) so each connection's anchors are isolated:
// two parallel binds on different connections don't share anchors.
// Per-catalog isolation via the registry key ("mssql_bind_anchors:<context>")
// so two ATTACH'es with different DSNs don't share anchors either.
// ============================================================================

#pragma once

#include <mutex>
#include <vector>
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/main/client_context_state.hpp"

namespace duckdb {

class MSSQLTableEntry;
class MSSQLSchemaEntry;
class ClientContext;
class MSSQLCatalog;

class MSSQLBindAnchors : public ClientContextState {
public:
	MSSQLBindAnchors() = default;
	~MSSQLBindAnchors() override = default;

	// DuckDB calls this at the end of every query on this ClientContext.
	// Dropping the anchors here releases our reference; if entries_ already
	// dropped its ref (concurrent Invalidate during the query), the
	// underlying entry is freed here.
	void QueryEnd(ClientContext &context) override;

	// Stash a shared_ptr to keep the entry alive until QueryEnd. Called from
	// MSSQLSchemaEntry::LookupEntry and MSSQLCatalog::LookupSchema.
	void AnchorTable(shared_ptr<MSSQLTableEntry> entry);
	void AnchorSchema(shared_ptr<MSSQLSchemaEntry> entry);

	// For diagnostics / tests only.
	size_t TableAnchorCount() const;
	size_t SchemaAnchorCount() const;

	// Retrieve (or create) the anchors holder for the given context+catalog
	// pair. Key includes catalog context_name so two ATTACH'es on the same
	// connection don't share anchors.
	static MSSQLBindAnchors &For(ClientContext &context, const MSSQLCatalog &catalog);

private:
	mutable std::mutex mutex_;
	std::vector<shared_ptr<MSSQLTableEntry>> table_anchors_;
	std::vector<shared_ptr<MSSQLSchemaEntry>> schema_anchors_;
};

}  // namespace duckdb
