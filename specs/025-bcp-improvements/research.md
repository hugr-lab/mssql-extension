# Research: BCP Improvements

**Feature**: 025-bcp-improvements
**Date**: 2026-01-30

## Research Questions

### 1. Empty Schema URL Parsing

**Question**: How should `mssql://db//#temp` and `db..#temp` be parsed?

**Decision**: Treat empty path segment or empty schema component as valid for temp tables only.

**Rationale**:
- SQL Server temp tables (`#temp`, `##global_temp`) have no schema in `tempdb`
- Current implementation defaults empty schema to `dbo`, which is correct for regular tables
- For temp tables, empty schema should be accepted and the table accessed as `tempdb..#table`
- Both URL format (`mssql://db//#temp`) and catalog format (`db..#temp`) should work

**Implementation**:
- In `ResolveURL()`: When path has 3 parts (catalog//table), check if table starts with `#`
  - If yes: accept empty schema, set `schema_name = ""` (or skip schema in queries)
  - If no: reject with error "Empty schema only valid for temp tables"
- In `ResolveCatalog()`: When schema is empty, apply same logic

**Alternatives Considered**:
- Always default to `dbo` - rejected because `dbo.#temp` is not valid SQL Server syntax
- Require explicit syntax only (`db.#temp`) - rejected for user flexibility

### 2. Connection Leak Prevention

**Question**: Where are connections potentially leaked in COPY error paths?

**Decision**: Add RAII-style connection cleanup in all COPY callback phases.

**Rationale**:
- Current implementation releases connections in `BCPCopyFinalize()` success path
- Error paths in `BCPCopyBind()`, `BCPCopySink()`, and `BCPCopyFinalize()` may not properly release
- Connection should be released back to pool (or kept pinned if in transaction) on ANY error

**Implementation**:
- Add try-catch around critical sections in each callback
- Use `ConnectionProvider::ReleaseConnection()` in error paths (handles transaction awareness)
- Ensure BCP stream is properly terminated (DONE token) before releasing connection
- Add debug logging for connection acquire/release tracking

**Alternatives Considered**:
- RAII wrapper class for connection - considered but existing patterns work well with explicit release

### 3. Type Mismatch Error Messages

**Question**: What information should type mismatch errors include?

**Decision**: Include column name, expected SQL Server type, and actual DuckDB source type.

**Rationale**:
- Current errors may be cryptic or from SQL Server directly
- Users need to know which column has the issue and what types are involved
- This reduces debugging time significantly

**Implementation**:
- In `ValidateTarget()` or schema matching: compare source and target types
- Build error message: "Column 'name' type mismatch: target expects INT, source provides VARCHAR"
- Log column index for debugging

**Alternatives Considered**:
- Only log to debug - rejected, users need this in error message
- Include row number - not practical for BCP streaming, deferred

### 4. INSERT Method Integration

**Question**: How should INSERT method integrate with existing INSERT infrastructure?

**Decision**: Reuse existing `MSSQLInsertExecutor` with COPY-specific configuration.

**Rationale**:
- INSERT infrastructure already handles batching, type conversion, and RETURNING
- No need to duplicate this logic in COPY
- Settings like `mssql_insert_batch_size` already exist and should be respected

**Implementation**:
- Add `METHOD` option to COPY: `(FORMAT 'bcp', METHOD 'insert')`
- In `BCPCopySink()`: if METHOD='insert', accumulate rows and call INSERT executor
- In `BCPCopyFinalize()`: execute final INSERT batch
- For RETURNING support: only available if table has identity column and option enabled

**Alternatives Considered**:
- New INSERT-specific COPY format (`FORMAT 'insert'`) - rejected, METHOD option cleaner
- Always use INSERT for small datasets - rejected, explicit user choice preferred

## Best Practices Applied

### DuckDB COPY Function Patterns

From existing DuckDB extensions and the current BCP implementation:
- Use `FunctionData` for bind-time configuration (options, target resolution)
- Use `GlobalSinkState` for connection and writer state
- Use `LocalSinkState` for per-thread accumulation (if parallel)
- Release resources in `Finalize()` or on error

### Connection Pool Error Handling

From existing extension patterns:
- Always release connections on error paths
- Check transaction state before releasing (keep pinned if in transaction)
- Log connection lifecycle at debug level for troubleshooting

## Dependencies

| Dependency | Version | Purpose |
|------------|---------|---------|
| DuckDB | main branch | COPY function API, catalog integration |
| Existing INSERT | src/dml/insert/ | Reused for INSERT method |
| Connection Pool | src/connection/ | Connection management |

## Risks and Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Empty schema breaks regular tables | Low | High | Guard with temp table check |
| INSERT method slower than expected | Low | Medium | Document as intentional, for debugging |
| Connection leak edge cases missed | Medium | High | Comprehensive error tests |
