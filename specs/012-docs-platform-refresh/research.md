# Research: Documentation, Platform Support & Cache Refresh

**Branch**: `012-docs-platform-refresh` | **Date**: 2026-01-19

## Research Tasks

### 1. DuckDB Scalar Function Registration Pattern

**Decision**: Use Pattern C (complex scalar with bind data) from `mssql_exec`

**Rationale**:
- Requires compile-time validation of catalog name argument
- Need to verify catalog exists and is MSSQL type before execution
- Matches existing patterns in the codebase for catalog-aware functions

**Alternatives Considered**:
- Pattern A (simple scalar like `mssql_version`): Rejected - no argument validation capability
- Pattern B (diagnostic group like `mssql_open/close`): Rejected - single function, doesn't need grouping

**Key Implementation Details**:
```cpp
// Required components:
1. MSSQLRefreshCacheBindData : FunctionData - holds validated catalog name
2. MSSQLRefreshCacheBind() - validates argument, checks catalog exists
3. MSSQLRefreshCacheExecute() - calls InvalidateMetadataCache() + EnsureCacheLoaded()
4. RegisterMSSQLRefreshCacheFunction(ExtensionLoader&) - registration wrapper
```

### 2. Cache Invalidation Mechanism

**Decision**: Use existing `InvalidateMetadataCache()` followed by `EnsureCacheLoaded(context)`

**Rationale**:
- `InvalidateMetadataCache()` already exists in MSSQLCatalog (line 146 in header)
- `EnsureCacheLoaded()` handles the actual refresh with connection acquisition
- This pattern ensures thread-safe cache state transitions

**Alternatives Considered**:
- Direct cache manipulation: Rejected - bypasses existing safety mechanisms
- New refresh method: Rejected - unnecessary when existing methods suffice

**Code Path**:
```
mssql_refresh_cache('ctx')
  → MSSQLCatalog::InvalidateMetadataCache()
    → metadata_cache_->SetState(STALE)
  → MSSQLCatalog::EnsureCacheLoaded(context)
    → Acquires connection from pool
    → Calls metadata_cache_->Refresh()
    → Loads schemas/tables from SQL Server
```

### 3. Error Handling Strategy

**Decision**: Layer-appropriate exceptions with informative messages

**Implementation**:
| Error Condition | Exception Type | Message Pattern |
|-----------------|----------------|-----------------|
| Empty/null argument | `InvalidInputException` | "mssql_refresh_cache: catalog name is required" |
| Non-existent catalog | `BinderException` | "mssql_refresh_cache: catalog 'X' not found..." |
| Non-MSSQL catalog | `BinderException` | "mssql_refresh_cache: catalog 'X' is not an MSSQL catalog" |
| Connection failure | `IOException` | Propagated from TDS layer |
| Pool timeout | `IOException` | Propagated from pool acquire |

### 4. DuckDB Community Extension description.yml Schema

**Decision**: Use schema provided by user with modifications based on DuckDB community examples

**Key Fields**:
- `extension.name`: "mssql"
- `extension.version`: "0.1.0"
- `extension.excluded_platforms`: ["osx_amd64", "windows_arm64"]
- `repo.github`: "hugr-lab/mssql-extension"
- `docs.hello_world`: Working SQL example

**Reference**: [DuckDB Community Extensions Repository](https://github.com/duckdb/community-extensions)

### 5. Platform Support Documentation

**Decision**: Update existing Platform Support table in README with accurate status

**Current vs. Target**:
| Platform | Current Status | Target Status |
|----------|---------------|---------------|
| Linux x86_64 | Fully Tested | CI-validated |
| Linux ARM64 | Fully Tested | Not tested, not built in CD |
| macOS ARM64 | Tested | Primary development |
| Windows x64 | Experimental | Not tested, not built in CD |

**Rationale**: User specified these exact statuses reflecting actual CI/CD coverage.

## Research Summary

All technical unknowns resolved:

1. **Function Pattern**: Pattern C with bind data (validated)
2. **Cache Mechanism**: Existing InvalidateMetadataCache + EnsureCacheLoaded (validated)
3. **Error Strategy**: Layer-appropriate exceptions (defined)
4. **description.yml**: User-provided template (validated)
5. **Platform Docs**: User-specified statuses (defined)

No NEEDS CLARIFICATION items remain.
