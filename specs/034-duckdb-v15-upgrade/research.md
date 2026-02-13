# Research: DuckDB v1.5 Variegata Upgrade

## Decision: Upgrade is Safe

**Rationale**: The extension's existing compatibility layer (`mssql_compat.hpp` + CMake auto-detection) handles the only critical API change (`GetData` → `GetDataInternal`). All other v1.5 changes are backward-compatible or have default implementations that don't require extension code changes.

**Alternatives considered**:
- Wait for v1.5 stable release — rejected because early testing catches issues sooner
- Pin to v1.4.x — rejected because staying current prevents accumulating technical debt

## API Change Analysis

### GetData → GetDataInternal (CRITICAL, MITIGATED)

In v1.5, `PhysicalOperator::GetData` became a non-virtual public wrapper. The virtual method is now `GetDataInternal` (protected). The extension already uses the `MSSQL_GETDATA_METHOD` compat macro, and the CMake auto-detection (`string(FIND ... "GetDataInternal")`) correctly identifies the v1.5 API.

### New Virtual Methods (NO IMPACT)

- `SchemaCatalogEntry::CreateCoordinateSystem` — has default `NotImplementedException`
- `Catalog::SupportsCreateTable` — optional validation hook
- Neither requires an override for basic functionality.

### Changed Signatures (NO IMPACT)

- `GetColumnSegmentInfo(const QueryContext &context)` — extension doesn't override this method.

### New TableFunction Features (BACKWARD COMPATIBLE)

New optional callbacks: `table_statistics_extended_t`, `table_function_rows_scanned_t`, async support. These are additive and don't break existing `TableFunction` usage.

## Version Information

- Current: DuckDB v1.4.4 (commit `6ddac802ff`)
- Target: DuckDB v1.5-variegata (commit `f480e78169`)
- Delta: ~6,275 commits
