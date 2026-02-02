# Implementation Plan: CTAS BCP Integration

**Branch**: `027-ctas-bcp-integration` | **Date**: 2026-02-02 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/027-ctas-bcp-integration/spec.md`

## Summary

Add BCP protocol support to CTAS operations for improved performance (2-10x faster than batched INSERT). The implementation reuses existing BCPWriter infrastructure from COPY TO and adds a new `mssql_ctas_use_bcp` setting (default: true). Additionally, change `mssql_copy_tablock` default from true to false for safer multi-user behavior.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB (main branch), OpenSSL (vcpkg), existing TDS protocol layer
**Storage**: SQL Server 2019+ (remote target), in-memory (batch buffering)
**Testing**: DuckDB SQLLogicTest framework, C++ unit tests
**Target Platform**: Linux (GCC), macOS (Clang), Windows (MSVC, MinGW)
**Project Type**: DuckDB extension (single project)
**Performance Goals**: 2x throughput vs INSERT mode for 100K+ row CTAS operations
**Constraints**: Bounded memory via mssql_copy_flush_rows, single connection per CTAS
**Scale/Scope**: Extension-level feature, ~300-400 lines of new/modified code

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | ✅ PASS | Uses native TDS BulkLoadBCP protocol, no external dependencies |
| II. Streaming First | ✅ PASS | BCPWriter streams rows without full buffering |
| III. Correctness over Convenience | ✅ PASS | Fails explicitly on unsupported types, clear error messages |
| IV. Explicit State Machines | ✅ PASS | Connection states documented, BCP phase added to CTASPhase |
| V. DuckDB-Native UX | ✅ PASS | Standard CTAS syntax unchanged, setting-based opt-out |
| VI. Incremental Delivery | ✅ PASS | BCP mode additive, INSERT mode preserved as fallback |

**Post-Design Re-check**: All principles satisfied. No violations.

## Project Structure

### Documentation (this feature)

```text
specs/027-ctas-bcp-integration/
├── plan.md              # This file
├── research.md          # Phase 0 output - integration analysis
├── data-model.md        # Phase 1 output - entity modifications
├── quickstart.md        # Phase 1 output - usage guide
└── tasks.md             # Phase 2 output (created by /speckit.tasks)
```

### Source Code (repository root)

```text
src/
├── connection/
│   └── mssql_settings.cpp       # Add mssql_ctas_use_bcp setting
├── copy/
│   ├── bcp_config.cpp           # Change mssql_copy_tablock default
│   └── bcp_writer.hpp/cpp       # Reused by CTAS (no changes needed)
├── dml/ctas/
│   ├── mssql_ctas_config.hpp    # Add use_bcp, bcp_flush_rows, bcp_tablock
│   ├── mssql_ctas_executor.hpp  # Add BCPWriter member, BCP methods
│   └── mssql_ctas_executor.cpp  # Implement BCP execution path
└── include/
    └── connection/
        └── mssql_settings.hpp   # Add LoadCTASUseBCP declaration

test/sql/ctas/
├── ctas_basic.test              # Update for BCP mode
├── ctas_bcp.test                # NEW: BCP-specific tests
└── ctas_insert_mode.test        # NEW: Legacy INSERT mode tests

docs/
├── architecture.md              # Add CTAS BCP integration section
└── testing.md                   # Add CTAS testing guidance

README.md                        # Document new settings
CLAUDE.md                        # Document mssql_ctas_use_bcp setting
```

**Structure Decision**: Single project structure (DuckDB extension). Changes primarily in `src/dml/ctas/` with settings in `src/connection/` and tests in `test/sql/ctas/`.

## Complexity Tracking

No constitution violations. No complexity justification needed.

## Implementation Phases

### Phase 1: Settings and Configuration

1. Add `mssql_ctas_use_bcp` setting (BOOLEAN, default: true)
2. Change `mssql_copy_tablock` default from true to false
3. Extend CTASConfig with use_bcp, bcp_flush_rows, bcp_tablock fields
4. Load BCP settings in CTAS config loader

### Phase 2: BCP Integration in CTAS

1. Add BCPWriter and BCPColumnMetadata members to CTASExecutionState
2. Create `InitializeBCP()` method (converts CTASColumnDef → BCPColumnMetadata)
3. Create `ExecuteBCPInsert()` method (INSERT BULK command)
4. Create `AddChunkBCP()` method (encode rows via BCPRowEncoder)
5. Create `FlushBCP()` method (send accumulated data, handle batching)
6. Modify `ExecuteDDL()` to branch based on use_bcp setting
7. Modify `AddChunk()` to delegate to BCP or INSERT path
8. Modify `FlushInserts()` to call FlushBCP when in BCP mode

### Phase 3: Testing

1. Update existing CTAS tests to work with BCP mode (default)
2. Add ctas_bcp.test with BCP-specific scenarios
3. Add ctas_insert_mode.test to verify legacy behavior with `SET mssql_ctas_use_bcp = false`
4. Test TABLOCK default change in COPY tests

### Phase 4: Documentation

1. Update README.md with new settings and defaults
2. Update docs/architecture.md with CTAS BCP integration diagram
3. Update docs/testing.md with CTAS test guidance
4. Update CLAUDE.md with new setting

## Risk Mitigation

| Risk | Mitigation |
|------|------------|
| Type mapping mismatch | Use TargetResolver functions (same as COPY TO) |
| BCP errors hard to debug | Include BCP context in error messages, preserve INSERT fallback |
| TABLOCK default change breaks workflows | Document change clearly, easy opt-in for performance |
| Memory usage with large CTAS | Inherit mssql_copy_flush_rows bounded streaming |

## Generated Artifacts

- [research.md](./research.md) - Integration analysis and decisions
- [data-model.md](./data-model.md) - Entity modifications
- [quickstart.md](./quickstart.md) - Usage guide
