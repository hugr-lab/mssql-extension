# Implementation Plan: Documentation, Platform Support & Cache Refresh

**Branch**: `012-docs-platform-refresh` | **Date**: 2026-01-19 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/012-docs-platform-refresh/spec.md`

## Summary

This feature completes three deliverables:
1. **P1**: Implement `mssql_refresh_cache('catalog_name')` scalar function to enable manual metadata cache refresh without detach/reattach
2. **P2**: Update README with experimental status notice, contribution welcome message, and accurate platform support table
3. **P3**: Create `description.yml` for DuckDB community extension submission

The implementation follows existing patterns for scalar function registration (Pattern C from mssql_exec) and leverages existing `MSSQLCatalog::InvalidateMetadataCache()` and `MSSQLMetadataCache::Refresh()` infrastructure.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB main branch (extension API, catalog API), existing TDS layer (specs 001-011)
**Storage**: In-memory (metadata cache)
**Testing**: DuckDB test framework (make test), unit tests in tests/cpp/, integration tests in tests/integration/
**Target Platform**: macOS ARM64 (primary), Linux amd64 (CI-validated)
**Project Type**: Single DuckDB extension
**Performance Goals**: Cache refresh completes within SQL Server query response time (network-bound)
**Constraints**: Must not break existing catalog integration, must follow DuckDB scalar function patterns
**Scale/Scope**: Single scalar function implementation + documentation updates

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | PASS | No new external dependencies; uses existing TDS layer |
| II. Streaming First | PASS | Metadata refresh is not result streaming (metadata only) |
| III. Correctness over Convenience | PASS | Function fails explicitly on invalid input or connection errors |
| IV. Explicit State Machines | PASS | Leverages existing cache state machine (EMPTY→LOADING→READY→STALE) |
| V. DuckDB-Native UX | PASS | Function registered as standard DuckDB scalar function |
| VI. Incremental Delivery | PASS | Completes existing catalog integration feature set |

**Gate Status**: PASSED - No violations

## Project Structure

### Documentation (this feature)

```text
specs/012-docs-platform-refresh/
├── spec.md              # Feature specification
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output (minimal - no new data entities)
├── quickstart.md        # Phase 1 output
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (repository root)

```text
src/
├── catalog/
│   ├── mssql_refresh_function.cpp   # [MODIFY] Implement scalar function
│   └── mssql_metadata_cache.cpp     # [REFERENCE] Existing cache implementation
├── include/
│   └── catalog/
│       └── mssql_refresh_function.hpp  # [CREATE] Header for registration
├── mssql_extension.cpp              # [MODIFY] Add registration call

tests/
├── cpp/
│   └── test_refresh_cache.cpp       # [CREATE] Unit tests for function
└── integration/
    └── test_refresh_integration.cpp # [CREATE] Integration tests (optional)

# Root level
README.md                            # [MODIFY] Add status, platforms, function docs
description.yml                      # [CREATE] Community extension metadata
```

**Structure Decision**: Single project structure maintained. Changes localized to catalog module and documentation.

## Complexity Tracking

> No violations to justify - all work follows established patterns.

## Design Details

### Component 1: mssql_refresh_cache() Scalar Function

**Pattern**: Complex scalar function with bind data (Pattern C from mssql_exec)

**Implementation Approach**:
1. Create `MSSQLRefreshCacheBindData` structure to hold validated catalog name
2. Implement bind function for compile-time validation:
   - Validate argument is constant string
   - Validate catalog exists and is MSSQL type
   - Throw `BinderException` for invalid/missing catalog
3. Implement execute function:
   - Get catalog from bind data
   - Call `InvalidateMetadataCache()` then `EnsureCacheLoaded()`
   - Return `true` on success
   - Propagate connection errors as exceptions

**Function Signature**: `mssql_refresh_cache(VARCHAR) -> BOOLEAN`

**Error Handling**:
- Empty/null argument: `InvalidInputException` at bind time
- Non-existent catalog: `BinderException` with helpful message
- Connection failure: `IOException` propagated from TDS layer
- Timeout: Existing `mssql_acquire_timeout` setting applies

### Component 2: README Updates

**Changes Required**:
1. Add experimental status notice after Features section (or in introduction)
2. Add contribution welcome message
3. Update Platform Support table:
   - macOS ARM64: Primary development
   - Linux amd64: CI-validated
   - Linux arm64: Not tested, not built in CD
   - Windows amd64: Not tested, not built in CD
4. Add `mssql_refresh_cache()` to Function Reference section

### Component 3: description.yml

**Location**: Repository root
**Schema**: DuckDB community extension format
**Content**:
- Extension metadata (name, version, description, license, maintainers)
- Excluded platforms: `osx_amd64`, `windows_arm64`
- Hello world example with ATTACH and SELECT
- Extended description with feature summary

## File Change Summary

| File | Action | Description |
|------|--------|-------------|
| `src/catalog/mssql_refresh_function.cpp` | Modify | Implement scalar function |
| `src/include/catalog/mssql_refresh_function.hpp` | Create | Header with registration function |
| `src/mssql_extension.cpp` | Modify | Add RegisterMSSQLRefreshCacheFunction call |
| `tests/cpp/test_refresh_cache.cpp` | Create | Unit tests |
| `README.md` | Modify | Status, platforms, function docs |
| `description.yml` | Create | Community extension metadata |

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Cache state corruption on partial refresh | Low | High | Use existing atomic cache operations |
| Breaking existing catalog tests | Low | Medium | Run full test suite before merge |
| description.yml schema mismatch | Medium | Low | Validate against community examples |

## Dependencies

- Existing `MSSQLCatalog` class with `InvalidateMetadataCache()` method
- Existing `MSSQLMetadataCache` with state machine (EMPTY, LOADING, READY, STALE)
- Existing scalar function registration patterns (mssql_exec, mssql_open/close/ping)
- DuckDB extension loader API
