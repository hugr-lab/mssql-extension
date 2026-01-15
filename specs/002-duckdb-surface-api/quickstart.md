# Quickstart: DuckDB Surface API Implementation

**Feature**: 002-duckdb-surface-api
**Date**: 2026-01-15

This guide provides step-by-step instructions for implementing the MSSQL extension's public interface.

---

## Prerequisites

- DuckDB source (main branch) cloned as submodule
- C++17 compiler (GCC 11+, Clang 14+, MSVC 2019+)
- CMake 3.21+
- Existing extension skeleton from `001-project-bootstrap`

---

## Implementation Order

### Phase 1: Secret Type (FR-001 to FR-006)

**Goal**: Enable `CREATE SECRET ... TYPE mssql`

1. Create `src/include/mssql_secret.hpp`
2. Create `src/mssql_secret.cpp`
3. Add to CMakeLists.txt
4. Create `test/sql/mssql_secret.test`

**Verification**:
```sql
CREATE SECRET test_secret (
    TYPE mssql,
    host 'localhost',
    port 1433,
    database 'test',
    user 'sa',
    password 'test'
);

SELECT * FROM duckdb_secrets() WHERE type = 'mssql';
-- Should show secret with password redacted
```

### Phase 2: Storage Extension (FR-007 to FR-012a)

**Goal**: Enable `ATTACH ... TYPE mssql` and `DETACH`

1. Create `src/include/mssql_storage.hpp`
2. Create `src/mssql_storage.cpp`
3. Add to CMakeLists.txt
4. Create `test/sql/mssql_attach.test`

**Verification**:
```sql
CREATE SECRET my_secret (TYPE mssql, host 'localhost', port 1433,
                         database 'test', user 'sa', password 'test');
ATTACH '' AS mydb (TYPE mssql, SECRET my_secret);
-- Should succeed without network connection

DETACH mydb;
-- Should succeed
```

### Phase 3: Table Functions (FR-013 to FR-018)

**Goal**: Enable `mssql_execute` and `mssql_scan`

1. Create `src/include/mssql_functions.hpp`
2. Create `src/mssql_functions.cpp`
3. Add to CMakeLists.txt
4. Create `test/sql/mssql_execute.test`
5. Create `test/sql/mssql_scan.test`

**Verification**:
```sql
-- Setup
CREATE SECRET my_secret (TYPE mssql, host 'localhost', port 1433,
                         database 'test', user 'sa', password 'test');
ATTACH '' AS mydb (TYPE mssql, SECRET my_secret);

-- Test mssql_execute
SELECT * FROM mssql_execute('mydb', 'SELECT 1');
-- Should return: success=true, affected_rows=1, message='Query executed...'

-- Test mssql_scan
SELECT * FROM mssql_scan('mydb', 'SELECT id, name FROM users');
-- Should return 3 sample rows

DETACH mydb;
```

---

## File Structure After Implementation

```
src/
├── include/
│   ├── mssql_extension.hpp    # Existing
│   ├── mssql_secret.hpp       # NEW: Secret type definitions
│   ├── mssql_storage.hpp      # NEW: Storage extension, context manager
│   └── mssql_functions.hpp    # NEW: Table function definitions
├── mssql_extension.cpp        # MODIFIED: Add registrations
├── mssql_secret.cpp           # NEW: Secret implementation
├── mssql_storage.cpp          # NEW: Attach/detach implementation
└── mssql_functions.cpp        # NEW: mssql_scan, mssql_execute

test/sql/
├── mssql_version.test         # Existing
├── mssql_secret.test          # NEW
├── mssql_attach.test          # NEW
├── mssql_execute.test         # NEW
└── mssql_scan.test            # NEW
```

---

## CMakeLists.txt Updates

```cmake
set(EXTENSION_SOURCES
    src/mssql_extension.cpp
    src/mssql_secret.cpp        # ADD
    src/mssql_storage.cpp       # ADD
    src/mssql_functions.cpp     # ADD
)
```

---

## Key Implementation Patterns

### 1. Secret Registration

```cpp
// In RegisterMSSQLSecretType():
SecretType mssql_type;
mssql_type.name = "mssql";
mssql_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
mssql_type.default_provider = "config";
loader.RegisterSecretType(mssql_type);

CreateSecretFunction create_func;
create_func.secret_type = "mssql";
create_func.provider = "config";
create_func.function = CreateMSSQLSecretFromConfig;
create_func.named_parameters["host"] = LogicalType::VARCHAR;
// ... other parameters
loader.RegisterFunction(std::move(create_func));
```

### 2. Storage Extension Registration

```cpp
// In RegisterMSSQLStorageExtension():
auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
auto storage_ext = make_uniq<StorageExtension>();
storage_ext->attach = MSSQLAttach;
storage_ext->create_transaction_manager = MSSQLCreateTransactionManager;
config.storage_extensions["mssql"] = std::move(storage_ext);
```

### 3. Table Function Registration

```cpp
// In RegisterMSSQLFunctions():
TableFunction mssql_scan(
    "mssql_scan",
    {LogicalType::VARCHAR, LogicalType::VARCHAR},
    MSSQLScanFunction,
    MSSQLScanBind,
    MSSQLScanInitGlobal,
    MSSQLScanInitLocal
);
loader.RegisterFunction(mssql_scan);
```

---

## Testing Commands

```bash
# Build extension
make -j$(nproc)

# Run all tests
make test

# Run specific test
./build/release/test/unittest "test/sql/mssql_secret.test"

# Run SQL tests interactively
./build/release/duckdb
```

---

## Common Issues

### Issue: Secret type not recognized

**Symptom**: `Unknown secret type: mssql`

**Solution**: Ensure `RegisterMSSQLSecretType()` is called in `LoadInternal()` before any secret operations.

### Issue: Attach type not recognized

**Symptom**: `Unknown storage type: mssql`

**Solution**: Ensure `RegisterMSSQLStorageExtension()` registers in `config.storage_extensions["mssql"]`.

### Issue: Context not found in table function

**Symptom**: `Unknown context 'mydb'`

**Solution**: Verify `MSSQLContextManager::RegisterContext()` is called during attach and the manager is accessible via `MSSQLContextManager::Get(context.db)`.

---

## Acceptance Checklist

- [ ] `CREATE SECRET ... (TYPE mssql ...` works with all 5 required fields
- [ ] Missing/invalid fields produce clear error messages
- [ ] Secrets appear in `duckdb_secrets()` with password redacted
- [ ] `ATTACH '' AS name (TYPE mssql, SECRET secret_name)` creates context
- [ ] Attach does not open network connection (lazy)
- [ ] `DETACH name` removes context cleanly
- [ ] `mssql_execute` returns (success, affected_rows, message)
- [ ] `mssql_scan` returns 3 sample rows
- [ ] Invalid context name produces clear error in both functions
- [ ] All SQL tests pass
