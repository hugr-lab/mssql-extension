# Research: DuckDB Surface API

**Feature**: 002-duckdb-surface-api
**Date**: 2026-01-15

## Overview

This document consolidates research on DuckDB extension APIs required for implementing the MSSQL extension's public interface.

---

## 1. Secret System

### Decision
Use DuckDB's `KeyValueSecret` with custom `SecretType` registration.

### Rationale
- DuckDB provides a built-in secret management system designed for extensions
- `KeyValueSecret` handles all storage, serialization, and redaction automatically
- Follows established patterns from other database extensions (PostgreSQL, MySQL)
- No custom encryption needed—constitution delegates security to DuckDB's secret manager

### Alternatives Considered
| Alternative | Rejected Because |
|-------------|------------------|
| Custom BaseSecret subclass | Unnecessary complexity; KeyValueSecret handles all requirements |
| External credential store | Violates "Native and Open" principle; adds dependency |
| In-memory only | Would not persist across sessions |

### API Pattern

```cpp
// Register secret type
SecretType mssql_type;
mssql_type.name = "mssql";
mssql_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
mssql_type.default_provider = "config";
loader.RegisterSecretType(mssql_type);

// Register creation function with named parameters
CreateSecretFunction create_func;
create_func.secret_type = "mssql";
create_func.provider = "config";
create_func.function = CreateMSSQLSecretFromConfig;
create_func.named_parameters["host"] = LogicalType::VARCHAR;
create_func.named_parameters["port"] = LogicalType::INTEGER;
create_func.named_parameters["database"] = LogicalType::VARCHAR;
create_func.named_parameters["user"] = LogicalType::VARCHAR;
create_func.named_parameters["password"] = LogicalType::VARCHAR;
loader.RegisterFunction(std::move(create_func));
```

### Key Classes
- `SecretType` (duckdb/main/secret/secret.hpp)
- `CreateSecretFunction` - defines parameters and creation callback
- `KeyValueSecret` - stores key-value pairs with redaction support
- `CreateSecretInput` - receives user-provided values

---

## 2. Storage Extension (Attach/Detach)

### Decision
Implement `StorageExtension` with custom attach function returning a minimal `Catalog`.

### Rationale
- DuckDB's `ATTACH ... TYPE xxx` dispatches to registered storage extensions
- Allows lazy connection pattern (connect only when data accessed)
- Provides standard transaction manager integration
- Catalog can be empty for stub phase; populated later for schema browsing

### Alternatives Considered
| Alternative | Rejected Because |
|-------------|------------------|
| Virtual table per connection | Doesn't integrate with ATTACH/DETACH syntax |
| Custom database manager | Reinvents DuckDB infrastructure; harder to maintain |
| Attached database without catalog | Still requires StorageExtension registration |

### API Pattern

```cpp
// Define attach function
static unique_ptr<Catalog> MSSQLAttach(
    optional_ptr<StorageExtensionInfo> storage_info,
    ClientContext &context,
    AttachedDatabase &db,
    const string &name,
    AttachInfo &info,
    AttachOptions &options
) {
    // Extract SECRET parameter from options
    // Store connection context (lazy - don't connect yet)
    // Return empty catalog for stub
    return make_uniq<Catalog>(db);
}

// Register storage extension
auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
auto storage_ext = make_uniq<StorageExtension>();
storage_ext->attach = MSSQLAttach;
storage_ext->create_transaction_manager = MSSQLCreateTransactionManager;
config.storage_extensions["mssql"] = std::move(storage_ext);
```

### Key Classes
- `StorageExtension` (duckdb/storage/storage_extension.hpp)
- `AttachedDatabase` - represents an attached database instance
- `AttachInfo` - contains path, name, and user options
- `AttachOptions` - contains access mode, db_type, secret reference

### Connection Context Management
- Store connection metadata in `StorageExtensionInfo` subclass
- Track attached contexts by name for function lookups
- Implement detach cleanup via `AttachedDatabase::OnDetach()`

---

## 3. Table Functions

### Decision
Use `TableFunction` with bind/init/execute pattern for both `mssql_scan` and `mssql_execute`.

### Rationale
- Standard DuckDB pattern for functions returning tabular data
- Supports streaming results (important for future phases)
- Integrates with query optimizer and parallel execution
- Well-documented with many extension examples

### Alternatives Considered
| Alternative | Rejected Because |
|-------------|------------------|
| Scalar function returning JSON | Cannot return proper relations; poor UX |
| Custom operator | Overkill for table-valued functions |
| Direct catalog integration | Better suited for schema browsing, not ad-hoc queries |

### API Pattern

```cpp
// Bind function - determines output schema
static unique_ptr<FunctionData> MSSQLScanBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names
) {
    // Validate arguments (context_name, query)
    // For stub: return fixed schema
    return_types.push_back(LogicalType::INTEGER);
    return_types.push_back(LogicalType::VARCHAR);
    names.push_back("id");
    names.push_back("name");
    return bind_data;
}

// Main function - produces output rows
static void MSSQLScanFunction(
    ClientContext &context,
    TableFunctionInput &data,
    DataChunk &output
) {
    // For stub: return 3 hardcoded rows
    output.SetCardinality(3);
    // Fill columns...
}

// Registration
TableFunction mssql_scan("mssql_scan",
    {LogicalType::VARCHAR, LogicalType::VARCHAR},
    MSSQLScanFunction,
    MSSQLScanBind,
    MSSQLScanInitGlobal,
    MSSQLScanInitLocal
);
loader.RegisterFunction(mssql_scan);
```

### mssql_execute Return Schema
| Column | Type | Description |
|--------|------|-------------|
| success | BOOLEAN | Whether execution succeeded |
| affected_rows | BIGINT | Number of rows affected |
| message | VARCHAR | Status or error message |

### mssql_scan Stub Return Schema
| Column | Type | Description |
|--------|------|-------------|
| id | INTEGER | Sample row ID |
| name | VARCHAR | Sample name value |

---

## 4. Context Lookup Pattern

### Decision
Maintain a global map of attached MSSQL contexts accessible to table functions.

### Rationale
- Table functions receive context_name as string argument
- Need to resolve to connection metadata for query execution
- Must validate context exists before executing queries

### Implementation Approach

```cpp
// In mssql_storage.hpp
class MSSQLContextManager {
public:
    static MSSQLContextManager& Get(DatabaseInstance &db);

    void RegisterContext(const string &name, shared_ptr<MSSQLConnectionInfo> info);
    void UnregisterContext(const string &name);
    optional_ptr<MSSQLConnectionInfo> GetContext(const string &name);
    bool HasContext(const string &name);
};

// In mssql_functions.cpp
void MSSQLScanFunction(...) {
    auto &manager = MSSQLContextManager::Get(context.db);
    auto conn_info = manager.GetContext(context_name);
    if (!conn_info) {
        throw InvalidInputException("Unknown MSSQL context: " + context_name);
    }
    // Execute query using conn_info...
}
```

---

## 5. Error Handling Strategy

### Decision
Use DuckDB exception types with clear, actionable messages.

### Exception Types
| Scenario | Exception Type |
|----------|---------------|
| Missing required secret field | `InvalidInputException` |
| Invalid port number | `InvalidInputException` |
| Unknown context name | `InvalidInputException` |
| Secret not found | `BinderException` |
| Internal error | `InternalException` |

### Message Format
```
"MSSQL Error: {what happened}. {suggestion to fix}"

Examples:
- "MSSQL Error: Missing required field 'host'. Provide host parameter when creating secret."
- "MSSQL Error: Port must be between 1 and 65535. Got: -1"
- "MSSQL Error: Unknown context 'mydb'. Attach a database first with: ATTACH '' AS mydb (TYPE mssql, SECRET ...)"
```

---

## 6. File Organization

### Decision
Separate concerns into dedicated source files with shared headers.

### Files
| File | Responsibility |
|------|---------------|
| `mssql_secret.hpp/cpp` | Secret type and creation function |
| `mssql_storage.hpp/cpp` | StorageExtension, attach/detach, context manager |
| `mssql_functions.hpp/cpp` | mssql_scan, mssql_execute table functions |
| `mssql_extension.cpp` | Entry point, registration coordination |

### Rationale
- Clear separation of concerns
- Easier to maintain and test independently
- Follows patterns from other DuckDB extensions
- Headers in `src/include/`, implementation in `src/`

---

## Summary

All NEEDS CLARIFICATION items from Technical Context have been resolved:

| Item | Resolution |
|------|------------|
| Secret storage mechanism | KeyValueSecret via DuckDB's secret manager |
| Attach handler pattern | StorageExtension with custom attach function |
| Table function pattern | Bind/Init/Execute with FunctionData state |
| Context management | Global manager with name-based lookup |
| Error handling | DuckDB exception types with actionable messages |

The implementation can proceed to Phase 1 design artifacts.
