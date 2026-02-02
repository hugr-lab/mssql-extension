# Research: CTAS BCP Integration

**Feature**: 027-ctas-bcp-integration
**Date**: 2026-02-02

## Summary

This research documents the findings from analyzing the existing CTAS and BCP implementations to understand how to integrate BCP protocol support into CTAS operations.

---

## Decision 1: BCP Integration Approach

**Decision**: Reuse existing BCPWriter infrastructure from COPY TO within CTASExecutionState

**Rationale**:
- BCPWriter already handles all TDS BulkLoadBCP protocol requirements (COLMETADATA, ROW tokens, DONE)
- Type mapping via TargetResolver is compatible with CTAS column definitions
- Memory-bounded streaming via flush_rows threshold already implemented
- Avoids duplicating complex TDS packet construction logic

**Alternatives Considered**:
1. **Create separate BCP encoder for CTAS** - Rejected: Would duplicate BCPWriter functionality
2. **Modify INSERT executor to use BCP** - Rejected: Too invasive, INSERT needs SQL_BATCH for RETURNING support
3. **Call COPY TO internally from CTAS** - Rejected: Would require materializing source data, losing streaming

---

## Decision 2: Connection Management Strategy

**Decision**: Use same connection reference pattern as current CTAS (CTASExecutionState holds connection)

**Rationale**:
- BCP requires a single connection for the entire bulk load operation
- CTASExecutionState already holds connection reference for DDL + INSERT phases
- No change needed to connection lifecycle management
- Transaction pinning handled by existing ConnectionProvider infrastructure

**Alternatives Considered**:
1. **Explicit state machine transitions** - Deferred: Would be cleaner but requires larger refactor
2. **Separate BCP connection** - Rejected: Would complicate transaction semantics

---

## Decision 3: Type Mapping Source

**Decision**: Use TargetResolver's type mapping functions (GetTDSTypeToken, GetTDSMaxLength, GetSQLServerTypeDeclaration)

**Rationale**:
- TargetResolver already maps DuckDB types to TDS wire format correctly
- Same type mapping used by COPY TO, ensuring consistency
- BCPColumnMetadata structure captures all needed wire format details
- CTAS already has column definitions from DDL phase, easy to convert

**Alternatives Considered**:
1. **Create new type mapper** - Rejected: Would duplicate existing logic
2. **Use INSERT type mapping** - Rejected: INSERT uses SQL strings, not TDS tokens

---

## Decision 4: Configuration Inheritance

**Decision**: CTAS with BCP inherits existing COPY settings (mssql_copy_flush_rows, mssql_copy_tablock)

**Rationale**:
- Users familiar with COPY TO settings can apply same tuning to CTAS
- Avoids proliferation of duplicate settings
- TABLOCK behavior should be consistent between COPY TO and CTAS with BCP
- Simplifies documentation and user experience

**Alternatives Considered**:
1. **Create separate mssql_ctas_* settings for BCP** - Rejected: Would confuse users with duplicate settings
2. **Hardcode BCP settings in CTAS** - Rejected: Removes user control over performance tuning

---

## Decision 5: Error Handling Strategy

**Decision**: Keep existing CTAS cleanup semantics (mssql_ctas_drop_on_failure) for BCP mode

**Rationale**:
- Users expect consistent error handling regardless of data transfer method
- BCP failures should trigger same DROP TABLE cleanup as INSERT failures
- Error messages should indicate BCP-specific context when relevant
- Maintains backward compatibility with existing error handling expectations

**Alternatives Considered**:
1. **Different cleanup for BCP** - Rejected: Inconsistent user experience
2. **No cleanup for BCP (rely on transaction rollback)** - Rejected: DDL auto-commits, so table remains

---

## Technical Findings

### Current CTAS Architecture

```
CTASExecutionState
├── Initialize() - Generate DDL SQL
├── ExecuteDDL() - CREATE TABLE + init MSSQLInsertExecutor
├── AddChunk() - Forward to insert_executor->AddRow()
├── FlushInserts() - Finalize insert batches
└── AttemptCleanup() - DROP TABLE on failure
```

### Current BCP Architecture

```
BCPCopyFunction
├── Bind() - Parse target, load config
├── InitGlobal() - Acquire connection, execute INSERT BULK, init BCPWriter
├── Sink() - Encode rows to BCPWriter
├── FlushToServer() - Send accumulated data, reset state
└── Finalize() - Send DONE token, read response
```

### Integration Points

| Component | Current CTAS | With BCP Integration |
|-----------|--------------|---------------------|
| Data transfer | MSSQLInsertExecutor | BCPWriter |
| Batching | SQL statement batches | Binary packet accumulation |
| Flush control | Internal to INSERT | mssql_copy_flush_rows |
| Locking | Row locks | TABLOCK via mssql_copy_tablock |
| Type encoding | SQL literals | TDS binary tokens |

### Key Files to Modify

| File | Change |
|------|--------|
| `src/include/dml/ctas/mssql_ctas_config.hpp` | Add `use_bcp` flag |
| `src/include/dml/ctas/mssql_ctas_executor.hpp` | Add BCPWriter member, BCP methods |
| `src/dml/ctas/mssql_ctas_executor.cpp` | Implement BCP data path |
| `src/connection/mssql_settings.cpp` | Add `mssql_ctas_use_bcp` setting |
| `src/copy/bcp_config.cpp` | Change `mssql_copy_tablock` default to false |

### Shared Infrastructure to Reuse

1. **BCPWriter** (`src/copy/bcp_writer.hpp/cpp`) - Packet construction
2. **TargetResolver type mapping** (`src/copy/target_resolver.cpp`) - TDS types
3. **BCPConfig** (`src/copy/bcp_config.hpp`) - Settings loading
4. **BCPRowEncoder** (`src/tds/encoding/bcp_row_encoder.cpp`) - Row encoding

---

## Performance Expectations

Based on existing COPY TO benchmarks:
- BCP achieves ~2-10x throughput vs batched INSERT
- TABLOCK provides additional 15-30% improvement
- Memory usage bounded by flush_rows threshold
- Single-connection operation (no parallel sinks needed for CTAS)

---

## Risk Assessment

| Risk | Mitigation |
|------|------------|
| Type mapping mismatch between CTAS and BCP | Use same TargetResolver functions |
| Transaction semantics difference | BCP auto-commits like current DDL phase |
| Error message clarity | Include BCP context in error messages |
| Settings confusion | Document inheritance from COPY settings |
