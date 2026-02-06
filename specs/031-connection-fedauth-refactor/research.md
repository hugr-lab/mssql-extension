# Research Findings: Connection & FEDAUTH Refactoring

**Branch**: `031-connection-fedauth-refactor` | **Date**: 2026-02-06

## Bug 0.1: BCP State Corruption

### Root Cause: Confirmed

**Location**: `src/copy/bcp_writer.cpp:257-269`

When BCP encounters an error, it throws an exception at line 261:
```cpp
if (has_error) {
    if (error_message.empty()) {
        error_message = "Unknown SQL Server error during bulk load";
    }
    throw InvalidInputException("MSSQL: BCP failed: %s", error_message);  // Line 261
}
// ...
conn_.TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);  // Line 269 - NEVER REACHED
```

The state transition at line 269 is only reached on success. On error, the connection remains in `Executing` state.

**Pool Validation Gap**: `ConnectionPool::Release()` at line 130-167 only checks `IsAlive()`, not connection state. A connection in `Executing` state passes `IsAlive()` and gets returned to idle pool.

### Fix Strategy

1. **Option B (Recommended)**: Close connection on BCP error before throwing
2. **Option C (Defense in depth)**: Add state validation in `Release()` - close connections not in `Idle` state

### Implementation

```cpp
// In bcp_writer.cpp, before throw:
if (has_error) {
    // Reset connection state before throwing - connection will be closed by pool
    conn_.Close();  // or: conn_.TransitionState(Executing, Idle) then throw
    throw InvalidInputException("MSSQL: BCP failed: %s", error_message);
}

// In tds_connection_pool.cpp Release():
if (!config_.connection_cache || !conn->IsAlive() || conn->GetState() != ConnectionState::Idle) {
    conn->Close();
    stats_.connections_closed++;
    // ...
}
```

---

## Bug 0.2: Excessive Connection Acquires (CRITICAL UPDATE)

### Root Cause 1: Eager Connection Acquisition Pattern

**Location**: `src/catalog/mssql_catalog.cpp:164-171`

```cpp
optional_ptr<SchemaCatalogEntry> MSSQLCatalog::LookupSchema(...) {
    // ...
    auto connection = connection_pool_->Acquire();  // ALWAYS ACQUIRES - even if cache is valid!
    if (!connection) {
        throw IOException("Failed to acquire connection for schema lookup");
    }

    // Trigger lazy loading of schema list
    metadata_cache_->EnsureSchemasLoaded(*connection);  // May not even use the connection
    connection_pool_->Release(std::move(connection));
```

The problem is that `LookupSchema()` **always acquires a connection BEFORE checking if the cache is valid**. The cache check happens inside `EnsureSchemasLoaded()`, but the connection is already acquired by then.

Same pattern in `ScanSchemas()` at line 193.

**Impact**: Every schema lookup requires a connection acquire/release cycle, even when data is already cached.

### Root Cause 2: Full Cache Invalidation

**Location**: `src/dml/ctas/mssql_ctas_executor.cpp:285`

```cpp
void CTASExecutionState::InvalidateCache() {
    if (catalog) {
        catalog->InvalidateMetadataCache();  // INVALIDATES ALL CACHE, NOT JUST TARGET SCHEMA
    }
}
```

This calls `InvalidateAll()` in metadata cache which sets ALL schemas, tables, and columns to `NOT_LOADED` state - not just the newly created table.

### Connection Acquire Sequence for CTAS

1. `TableExists()` check → Acquire #1 (even if cached)
2. `SchemaExists()` check → Acquire #2 (even if cached)
3. `ExecuteDDL()` (CREATE TABLE) → Acquire #3
4. `ExecuteBCPInsert()` → Acquire #4 (held for BCP)
5. **After CTAS completes**: `InvalidateCache()` clears entire cache
6. Any subsequent query needs to reload:
   - `EnsureSchemasLoaded()` → Acquire #5
   - `EnsureTablesLoaded()` → Acquire #6
   - `EnsureColumnsLoaded()` → Acquire #7

### Why Other Operations Work Better

Other DDL operations use **point invalidation**:

```cpp
// In mssql_schema_entry.cpp:140
mssql_catalog.InvalidateSchemaTableSet(name);  // Only invalidates one schema
```

### Fix Strategy (Two-Part)

**Part 1: Lazy Connection Acquisition**

Refactor `LookupSchema()` to check cache first, only acquire connection if needed:

```cpp
optional_ptr<SchemaCatalogEntry> MSSQLCatalog::LookupSchema(...) {
    // Check if schema exists in cache WITHOUT acquiring connection
    if (metadata_cache_->HasSchema(name) && metadata_cache_->GetState() == MSSQLCacheState::LOADED) {
        return &GetOrCreateSchemaEntry(name);  // Cache hit - no connection needed
    }

    // Cache miss - acquire connection and load
    auto connection = connection_pool_->Acquire();
    // ...
}
```

**Part 2: Point Invalidation for CTAS**

```cpp
void CTASExecutionState::InvalidateCache() {
    if (catalog) {
        // Point invalidation - only invalidate the target schema
        catalog->InvalidateSchemaTableSet(target.schema_name);
    }
}
```

---

## Bug 0.3: INSERT in Transaction Fails

### Root Cause: Confirmed

**Location**: `src/catalog/mssql_catalog.cpp:164-166`

```cpp
optional_ptr<SchemaCatalogEntry> MSSQLCatalog::LookupSchema(...) {
    // ...
    auto connection = connection_pool_->Acquire();  // Direct pool access - IGNORES PINNED CONNECTION
    if (!connection) {
        throw IOException("Failed to acquire connection for schema lookup");
    }
```

The `LookupSchema()` method directly acquires from the connection pool **without checking for a pinned transaction connection**. This causes:
1. Pool exhaustion when transaction already has a pinned connection
2. Connection state mismatches
3. Error: "Failed to acquire connection for schema lookup"

### Correct Pattern

`ConnectionProvider::GetConnection()` in `mssql_connection_provider.cpp:97-125` correctly checks for pinned connections:

```cpp
auto *txn = TryGetMSSQLTransaction(context, catalog);
if (!txn || is_autocommit) {
    // Autocommit: acquire from pool
} else {
    // Transaction: use pinned connection
    auto pinned = txn->GetPinnedConnection();
    if (pinned) {
        return pinned;
    }
}
```

### Fix Strategy

Refactor `LookupSchema()` (and similar methods in `MSSQLTableSet`) to:
1. Accept a `ClientContext&` parameter
2. Use `ConnectionProvider::GetConnection()` instead of direct pool access
3. Or pass an optional connection parameter that can be used if provided

---

## Bug 0.4: Token Expiration

### Current State: Token Cache Works Correctly

**Location**: `src/azure/azure_token.cpp:41-47`

```cpp
std::string TokenCache::GetToken(const std::string &secret_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(secret_name);
    if (it != cache_.end() && it->second.IsValid()) {  // Checks expiration
        return it->second.access_token;
    }
    return "";
}
```

`IsValid()` checks expiration with 5-minute margin (`TOKEN_REFRESH_MARGIN_SECONDS = 300`).

### Root Cause: Token Captured at ATTACH Time

The issue is that pool connections are created with a **pre-encoded token captured in the factory closure** at ATTACH time. This token is stored in `fedauth_token_utf16le` and never refreshed.

**Location**: `src/connection/mssql_pool_manager.cpp` - `GetOrCreatePoolWithAzureAuth()` captures the token in the lambda.

### Why Detach/Attach Doesn't Help

When detaching, the token cache is NOT invalidated for that context's Azure secret. Re-attaching may reuse the cached (expired) token.

### Fix Strategy

1. **On auth failure**: Invalidate cached token and retry once with fresh token
2. **On DETACH**: Call `TokenCache::Invalidate()` for the context's Azure secret name

---

## Bug 0.5: FEDAUTH ATTACH Validation

### Root Cause: Confirmed - Asymmetric Validation

**Location**: `src/mssql_storage.cpp:766-775`

```cpp
if (ctx->connection_info->use_azure_auth) {
    // SKIPS ValidateAzureConnection() completely!
    MSSQL_STORAGE_DEBUG_LOG(1, "Azure auth: skipping validation, will validate on first pool connection");
    auto fedauth_data = mssql::azure::BuildFedAuthExtension(...);
    fedauth_token_utf16le = std::move(fedauth_data.token_utf16le);
} else {
    // SQL auth path ALWAYS validates
    ValidateConnection(*ctx->connection_info, pool_config.connection_timeout);
}
```

### Comparison

| Aspect | SQL Auth | FEDAUTH |
|--------|----------|---------|
| ATTACH validation | `ValidateConnection()` called | **SKIPPED** |
| TCP test | Yes | Deferred to first pool connection |
| Auth test | Yes | Deferred |
| SELECT 1 query | Yes (if TLS) | Never |
| When errors caught | At ATTACH time | At first query time |

### Fix Strategy

Call `ValidateAzureConnection()` during ATTACH:

```cpp
if (ctx->connection_info->use_azure_auth) {
    ValidateAzureConnection(context, *ctx->connection_info, pool_config.connection_timeout);
    // Then proceed with pool creation
}
```

---

## Bug 0.6: Printf Debugging

### Location: Confirmed

`src/tds/tds_connection_pool.cpp:60`:
```cpp
fprintf(stderr, "[MSSQL POOL] Acquire called on pool '%s'\n", context_name_.c_str());
```

### Fix Strategy

Use existing debug pattern:

```cpp
static int GetPoolDebugLevel() {
    static int level = -1;
    if (level == -1) {
        const char *env = std::getenv("MSSQL_DEBUG");
        level = env ? std::atoi(env) : 0;
    }
    return level;
}

#define MSSQL_POOL_DEBUG_LOG(lvl, fmt, ...) \
    do { \
        if (GetPoolDebugLevel() >= lvl) \
            fprintf(stderr, "[MSSQL POOL] " fmt "\n", ##__VA_ARGS__); \
    } while (0)

// Usage:
MSSQL_POOL_DEBUG_LOG(1, "Acquire called on pool '%s'", context_name_.c_str());
```

---

## Bug 0.7: Fabric Data Warehouse BCP/INSERT BULK Not Supported

### Root Cause: Platform Limitation (Confirmed via Microsoft Documentation)

**Finding**: Microsoft Fabric Data Warehouse does **NOT support** the TDS `INSERT BULK` protocol (BulkLoadBCP). This is a platform limitation, not a bug in our code.

### Evidence

1. **GitHub Issue dotnet/SqlClient#2995**: `SqlBulkCopy.WriteToServer()` fails with error:
   ```
   "INSERT is not a supported statement type" (Error 22424)
   ```
   See: https://github.com/dotnet/SqlClient/issues/2995

2. **Microsoft Fabric Documentation**: BULK LOAD command is explicitly NOT supported:
   > "At this time, the following list of commands is NOT currently supported. Don't try to use these commands."
   > - BULK LOAD

   See: https://learn.microsoft.com/en-us/fabric/data-warehouse/tsql-surface-area

3. **TABLOCK Has No Effect**: Even if INSERT BULK worked, TABLOCK is accepted but has no effect:
   > "Some options in BULK INSERT are enabled but have no effect in Fabric Data Warehouse, these include KEEPIDENTITY, FIRE_TRIGGERS, CHECK_CONSTRAINTS, TABLOCK, ORDER, ROWS_PER_BATCH..."

   See: https://blog.fabric.microsoft.com/en/blog/bulk-insert-statement-in-fabric-datawarehouse/

### What Works vs. What Doesn't on Fabric

| Method | Fabric Support | Notes |
|--------|---------------|-------|
| TDS `INSERT BULK` (BulkLoadBCP) | ❌ NOT SUPPORTED | Used by SqlBulkCopy, bcp.exe, our BCP implementation |
| T-SQL `BULK INSERT` | ✅ Supported | Reads from Azure storage files only (not TDS protocol) |
| T-SQL `COPY INTO` | ✅ Supported (Recommended) | High-throughput from Azure storage |
| Regular `INSERT` | ✅ Supported | Row-by-row, slower |
| `CREATE TABLE AS SELECT` | ✅ Supported | Uses regular INSERT internally |

### Impact on mssql-extension

1. **COPY TO MSSQL (BCP mode)**: Will fail on Fabric with "INSERT is not a supported statement type"
2. **CTAS with `mssql_ctas_use_bcp=true`**: Will fail on Fabric
3. **INSERT batching**: Works fine (uses regular INSERT statements)

### Current Code Behavior

The code already has a comment about Fabric issues:

**Location**: `src/connection/mssql_pool_manager.cpp:83`
```cpp
// Note: Warm-up query disabled - Fabric seems to have timing issues with pool connections
```

**Location**: `src/azure/azure_fedauth.cpp:107-117`
```cpp
bool IsFabricEndpoint(const std::string &host) {
    if (ContainsIgnoreCase(host, ".datawarehouse.fabric.microsoft.com")) {
        return true;
    }
    // ...
}
```

### Fix Strategy

**Option A (Graceful Degradation - Recommended):**

1. Detect Fabric endpoint using `IsFabricEndpoint()`
2. Automatically disable BCP for Fabric connections
3. Fall back to batched INSERT statements
4. Log warning: "BCP not supported on Fabric, using INSERT fallback"

**Option B (Documentation Only):**

1. Document limitation in AZURE.md
2. Throw clear error when attempting BCP on Fabric:
   ```
   "Microsoft Fabric Data Warehouse does not support INSERT BULK protocol.
   Use mssql_ctas_use_bcp=false for CTAS operations."
   ```

**Option C (Per-Connection Setting):**

1. Add `is_fabric_endpoint` flag to connection info
2. Auto-set based on hostname detection during ATTACH
3. BCP code checks flag and fails fast with clear error

### Implications for Bug 0.1

Bug 0.1 (BCP State Corruption) is **triggered by Fabric BCP failures**. When BCP fails on Fabric:
1. Server returns error "INSERT is not a supported statement type"
2. BCP exception thrown at line 261
3. State transition skipped (line 269)
4. Connection left in Executing state → Pool corruption

The Bug 0.1 fix (close connection on error) will prevent state corruption, but the root cause (BCP unsupported on Fabric) should also be addressed.

---

## Summary of Fixes Required

| Bug | Root Cause | Fix Location | Complexity |
|-----|------------|--------------|------------|
| 0.1 | BCP error skips state transition | `bcp_writer.cpp` + `tds_connection_pool.cpp` | Low |
| 0.2a | Eager connection acquisition (always acquires even if cached) | `mssql_catalog.cpp:164`, `mssql_table_set.cpp` | Medium |
| 0.2b | Full cache invalidation after CTAS | `mssql_ctas_executor.cpp:285` | Low |
| 0.3 | Direct pool access ignores pinned connection | `mssql_catalog.cpp`, `mssql_table_set.cpp` | Medium |
| 0.4 | Token captured at ATTACH, never refreshed | `mssql_pool_manager.cpp`, detach handler | Medium |
| 0.5 | FEDAUTH skips validation | `mssql_storage.cpp:766-775` | Low |
| 0.6 | Unconditional fprintf | `tds_connection_pool.cpp:60` | Low |
| 0.7 | Fabric doesn't support INSERT BULK | `copy/`, `dml/ctas/` - detection + fallback | Medium |

## Decisions Made

| Decision | Rationale | Alternatives Considered |
|----------|-----------|------------------------|
| Use point invalidation for CTAS | Matches pattern of other DDL ops | Full invalidation (current, too aggressive) |
| Close connection on BCP error | Clean slate, prevents state corruption | Reset to Idle (may mask issues) |
| Validate FEDAUTH on ATTACH | Fail-fast consistency with SQL auth | Deferred validation (current, confusing errors) |
| Token refresh on auth failure | Handles expiration gracefully | Background refresh (more complex) |
| Graceful degradation for Fabric BCP | Better UX than hard error | Documentation only (confusing), Hard error (poor UX) |

## Fabric Endpoint Detection

The extension already has `IsFabricEndpoint()` function in `src/azure/azure_fedauth.cpp:107-117` that detects:
- `*.datawarehouse.fabric.microsoft.com` (Fabric Data Warehouse)
- `*.pbidedicated.windows.net` (Power BI Dedicated / Fabric backend)

This can be reused to implement Fabric-specific behavior for BCP operations.
