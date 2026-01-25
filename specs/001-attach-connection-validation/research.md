# Research: Attach Connection Validation

## Overview

This document captures research findings for implementing connection validation during ATTACH and adding `TrustServerCertificate` as an alias for `use_encrypt`.

## Current Implementation Analysis

### 1. ATTACH Flow (Current)

**File**: `src/mssql_storage.cpp` (lines 381-435)

```cpp
unique_ptr<Catalog> MSSQLAttach(...) {
    // 1. Extract SECRET parameter from options
    // 2. Parse connection info (from secret or connection string)
    // 3. Register context with MSSQLContextManager
    // 4. Create connection pool (factory function only - no actual connection)
    // 5. Create and initialize MSSQLCatalog
    // 6. Return catalog
}
```

**Problem**: Step 4 only creates a factory function for connections. No actual connection is attempted until a catalog query needs data.

### 2. Connection Pool Creation

**File**: `src/connection/mssql_pool_manager.cpp` (lines 10-49)

The pool manager creates a connection factory:
```cpp
auto factory = [...]() -> std::shared_ptr<tds::TdsConnection> {
    auto conn = std::make_shared<tds::TdsConnection>();
    if (!conn->Connect(host, port)) {
        return nullptr;  // Silent failure
    }
    if (!conn->Authenticate(username, password, database, use_encrypt)) {
        return nullptr;  // Silent failure
    }
    return conn;
};
```

The factory is stored but never called during ATTACH. Connections are created lazily on first use.

### 3. Error Messages (Current)

When credentials fail later during catalog queries:
- "IO Error: Failed to acquire connection for metadata refresh"
- No indication of what went wrong (auth? network? TLS?)

### 4. Connection String Parsing

**File**: `src/mssql_storage.cpp` (lines 261-304)

Currently parses:
- `server` (host,port)
- `database`
- `user` / `user id` / `uid`
- `password`
- `encrypt` (yes/true/1)

Does NOT parse: `TrustServerCertificate`

## Design Decisions

### Decision 1: Validation Approach

**Decision**: Test connection immediately in MSSQLAttach before registering context or creating catalog.

**Rationale**:
- Single point of validation (fail early)
- No partial state (context, pool, catalog) on failure
- Clear error message at the point of failure

**Alternatives Rejected**:
1. **Lazy validation on first query**: Current behavior - confusing errors, hard to debug
2. **Background validation thread**: Complexity, race conditions with queries

### Decision 2: TrustServerCertificate Semantics

**Decision**: `TrustServerCertificate` is an exact alias for `use_encrypt`, not ADO.NET semantics.

**ADO.NET Semantics** (for reference):
- `Encrypt=true` + `TrustServerCertificate=true` = Encrypted, don't validate cert
- `Encrypt=true` + `TrustServerCertificate=false` = Encrypted, validate cert
- `Encrypt=false` = No encryption

**Our Semantics** (simpler):
- `use_encrypt=true` or `TrustServerCertificate=true` = Enable TLS
- `use_encrypt=false` or `TrustServerCertificate=false` = No TLS
- Both specified with same value = OK
- Both specified with different values = Error

**Rationale**:
- Current TDS implementation doesn't support certificate validation separately
- Simple 1:1 alias is easier to understand and implement
- Users expecting ADO.NET semantics will get TLS enabled (safer default)

**Alternatives Rejected**:
1. **Full ADO.NET semantics**: Requires certificate validation infrastructure we don't have
2. **Ignore TrustServerCertificate**: Breaks user expectations from connection string copy/paste

### Decision 3: Error Message Categories

**Decision**: Categorize errors into specific types for actionable messages.

| Error Type | Detection | User Message |
|------------|-----------|--------------|
| DNS/Hostname | `Connect()` fails, errno=ENOENT | "Cannot resolve hostname 'X'" |
| Connection Refused | `Connect()` fails, errno=ECONNREFUSED | "Connection refused to X:Y - check if SQL Server is running" |
| Timeout | `Connect()` times out | "Connection timed out to X:Y after Z seconds" |
| Auth Failed | `Authenticate()` fails with error token | "Authentication failed for user 'X' - check credentials" |
| Database Access | `Authenticate()` fails with DB error | "Cannot access database 'X' - check database name and permissions" |
| TLS Failed | TLS handshake fails | "TLS handshake failed - check TLS configuration" |

**Rationale**: Users need to know exactly what to fix without debugging.

### Decision 4: Resource Cleanup on Failure

**Decision**: On validation failure, no resources are registered or created.

**Order of operations**:
1. Parse connection info (can throw - no cleanup needed)
2. Create test connection (if fails - connection cleaned up automatically)
3. Close test connection (explicit cleanup)
4. Register context (only if step 2 succeeded)
5. Create pool and catalog (only if step 2 succeeded)

**Rationale**: RAII ensures no leaks. Test connection is temporary and not from pool.

### Decision 5: Timeout Handling

**Decision**: Use existing `mssql_connection_timeout` setting for validation timeout.

**Default**: 30 seconds

**Rationale**: Consistent with other connection operations. Users can adjust if needed.

## Implementation Strategy

### Phase 1: Connection Validation

1. **Add validation function** in `mssql_storage.cpp`:
   ```cpp
   void ValidateConnection(const MSSQLConnectionInfo& info);
   ```

2. **Call before context registration** in `MSSQLAttach`:
   ```cpp
   ValidateConnection(*ctx->connection_info);  // Throws on failure
   manager.RegisterContext(name, ctx);  // Only reached if valid
   ```

3. **Error translation**: Map TDS errors to user-friendly messages.

### Phase 2: TrustServerCertificate Alias

1. **Add parsing** in connection string parser:
   ```cpp
   if (params.find("trustservercertificate") != params.end()) {
       auto val = StringUtil::Lower(params["trustservercertificate"]);
       trust_server_cert = (val == "yes" || val == "true" || val == "1");
   }
   ```

2. **Conflict detection**:
   ```cpp
   if (encrypt_specified && trust_cert_specified && encrypt != trust_cert) {
       throw InvalidInputException("Conflicting values for Encrypt and TrustServerCertificate");
   }
   ```

3. **Apply alias**: `use_encrypt = encrypt || trust_server_cert`

### Phase 3: Testing

1. **Invalid credentials test**: Verify immediate error on bad password
2. **Unreachable server test**: Verify timeout behavior
3. **TrustServerCertificate test**: Verify alias works and conflicts detected
4. **Valid connection test**: Verify no regression in normal flow

## References

- `src/mssql_storage.cpp`: ATTACH handler, connection string parsing
- `src/connection/mssql_pool_manager.cpp`: Pool creation
- `src/tds/tds_connection.cpp`: TDS authentication flow
- `src/connection/mssql_diagnostic.cpp`: Existing diagnostic functions (reference for error handling)
