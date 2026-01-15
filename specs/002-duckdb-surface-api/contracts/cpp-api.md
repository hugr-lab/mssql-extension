# C++ API Contract: MSSQL Extension

**Feature**: 002-duckdb-surface-api
**Date**: 2026-01-15

This document defines the internal C++ API for the MSSQL extension components.

---

## 1. Header Files

### mssql_secret.hpp

```cpp
#pragma once

#include "duckdb.hpp"
#include "duckdb/main/secret/secret.hpp"

namespace duckdb {

// Secret field names (constants)
constexpr const char *MSSQL_SECRET_HOST = "host";
constexpr const char *MSSQL_SECRET_PORT = "port";
constexpr const char *MSSQL_SECRET_DATABASE = "database";
constexpr const char *MSSQL_SECRET_USER = "user";
constexpr const char *MSSQL_SECRET_PASSWORD = "password";

// Register MSSQL secret type and creation function
void RegisterMSSQLSecretType(ExtensionLoader &loader);

// Create secret from user-provided parameters
// Throws: InvalidInputException on validation failure
unique_ptr<BaseSecret> CreateMSSQLSecretFromConfig(
    ClientContext &context,
    CreateSecretInput &input
);

// Validate secret fields
// Returns: empty string if valid, error message if invalid
string ValidateMSSQLSecretFields(const CreateSecretInput &input);

}  // namespace duckdb
```

### mssql_storage.hpp

```cpp
#pragma once

#include "duckdb.hpp"
#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

// Connection information extracted from secret
struct MSSQLConnectionInfo {
    string host;
    uint16_t port;
    string database;
    string user;
    string password;
    bool connected = false;

    // Create from secret
    static shared_ptr<MSSQLConnectionInfo> FromSecret(
        ClientContext &context,
        const string &secret_name
    );
};

// Attached context state
struct MSSQLContext {
    string name;
    string secret_name;
    shared_ptr<MSSQLConnectionInfo> connection_info;
    optional_ptr<AttachedDatabase> attached_db;

    MSSQLContext(const string &name, const string &secret_name);
};

// Global context manager (singleton per DatabaseInstance)
class MSSQLContextManager {
public:
    // Get singleton instance
    static MSSQLContextManager &Get(DatabaseInstance &db);

    // Context operations
    void RegisterContext(const string &name, shared_ptr<MSSQLContext> ctx);
    void UnregisterContext(const string &name);
    shared_ptr<MSSQLContext> GetContext(const string &name);
    bool HasContext(const string &name);
    vector<string> ListContexts();

private:
    mutex lock;
    unordered_map<string, shared_ptr<MSSQLContext>> contexts;
};

// Storage extension info (shared state)
struct MSSQLStorageExtensionInfo : public StorageExtensionInfo {
    // Reserved for future connection pooling, etc.
};

// Register storage extension for ATTACH TYPE mssql
void RegisterMSSQLStorageExtension(ExtensionLoader &loader);

// Attach callback
unique_ptr<Catalog> MSSQLAttach(
    optional_ptr<StorageExtensionInfo> storage_info,
    ClientContext &context,
    AttachedDatabase &db,
    const string &name,
    AttachInfo &info,
    AttachOptions &options
);

// Transaction manager factory
unique_ptr<TransactionManager> MSSQLCreateTransactionManager(
    optional_ptr<StorageExtensionInfo> storage_info,
    AttachedDatabase &db,
    Catalog &catalog
);

}  // namespace duckdb
```

### mssql_functions.hpp

```cpp
#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

// ============================================
// mssql_execute
// ============================================

struct MSSQLExecuteBindData : public FunctionData {
    string context_name;
    string sql_statement;

    unique_ptr<FunctionData> Copy() const override;
    bool Equals(const FunctionData &other) const override;
};

// Bind: validates arguments, sets return schema
unique_ptr<FunctionData> MSSQLExecuteBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names
);

// Execute: produces single-row result
void MSSQLExecuteFunction(
    ClientContext &context,
    TableFunctionInput &data,
    DataChunk &output
);

// ============================================
// mssql_scan
// ============================================

struct MSSQLScanBindData : public FunctionData {
    string context_name;
    string query;
    vector<LogicalType> return_types;
    vector<string> column_names;

    unique_ptr<FunctionData> Copy() const override;
    bool Equals(const FunctionData &other) const override;
};

struct MSSQLScanGlobalState : public GlobalTableFunctionState {
    string context_name;
    string query;
    idx_t total_rows;
    atomic<idx_t> rows_returned;

    idx_t MaxThreads() const override;
};

struct MSSQLScanLocalState : public LocalTableFunctionState {
    idx_t current_row = 0;
};

// Bind: validates arguments, determines return schema
unique_ptr<FunctionData> MSSQLScanBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names
);

// Global init: sets up execution state
unique_ptr<GlobalTableFunctionState> MSSQLScanInitGlobal(
    ClientContext &context,
    TableFunctionInitInput &input
);

// Local init: per-thread state
unique_ptr<LocalTableFunctionState> MSSQLScanInitLocal(
    ExecutionContext &context,
    TableFunctionInitInput &input,
    GlobalTableFunctionState *global_state
);

// Execute: produces output rows
void MSSQLScanFunction(
    ClientContext &context,
    TableFunctionInput &data,
    DataChunk &output
);

// ============================================
// Registration
// ============================================

// Register all MSSQL table functions
void RegisterMSSQLFunctions(ExtensionLoader &loader);

}  // namespace duckdb
```

---

## 2. Function Signatures

### Secret Functions

```cpp
// Register secret type with DuckDB
void RegisterMSSQLSecretType(ExtensionLoader &loader);

// Signature: create_secret_function_t
unique_ptr<BaseSecret> CreateMSSQLSecretFromConfig(
    ClientContext &context,
    CreateSecretInput &input
);

// Validation helper
// Returns: Empty string if valid, error message otherwise
string ValidateMSSQLSecretFields(const CreateSecretInput &input);
```

### Storage Extension Functions

```cpp
// Signature: attach_function_t
unique_ptr<Catalog> MSSQLAttach(
    optional_ptr<StorageExtensionInfo> storage_info,
    ClientContext &context,
    AttachedDatabase &db,
    const string &name,
    AttachInfo &info,
    AttachOptions &options
);

// Signature: create_transaction_manager_t
unique_ptr<TransactionManager> MSSQLCreateTransactionManager(
    optional_ptr<StorageExtensionInfo> storage_info,
    AttachedDatabase &db,
    Catalog &catalog
);
```

### Table Function Callbacks

```cpp
// mssql_execute
// Signature: table_function_bind_t
unique_ptr<FunctionData> MSSQLExecuteBind(...);
// Signature: table_function_t
void MSSQLExecuteFunction(...);

// mssql_scan
// Signature: table_function_bind_t
unique_ptr<FunctionData> MSSQLScanBind(...);
// Signature: table_function_init_global_t
unique_ptr<GlobalTableFunctionState> MSSQLScanInitGlobal(...);
// Signature: table_function_init_local_t
unique_ptr<LocalTableFunctionState> MSSQLScanInitLocal(...);
// Signature: table_function_t
void MSSQLScanFunction(...);
```

---

## 3. Error Handling

### Exception Types

| Scenario | Exception | Example |
|----------|-----------|---------|
| Invalid user input | `InvalidInputException` | Missing required field |
| Binding failure | `BinderException` | Secret not found |
| Internal error | `InternalException` | Unexpected null pointer |
| Catalog error | `CatalogException` | Duplicate context name |

### Error Message Format

```cpp
// Standard format
throw InvalidInputException(
    "MSSQL Error: %s. %s",
    what_happened,
    suggestion_to_fix
);

// Examples
throw InvalidInputException(
    "MSSQL Error: Missing required field 'host'. "
    "Provide host parameter when creating secret."
);

throw InvalidInputException(
    "MSSQL Error: Unknown context '%s'. "
    "Attach a database first with: ATTACH '' AS %s (TYPE mssql, SECRET ...)",
    context_name, context_name
);
```

---

## 4. Memory Management

### Ownership Rules

| Type | Ownership | Lifetime |
|------|-----------|----------|
| MSSQLConnectionInfo | shared_ptr | Context lifetime |
| MSSQLContext | shared_ptr | Attach to detach |
| FunctionData subclasses | unique_ptr | Query execution |
| GlobalTableFunctionState | unique_ptr | Query execution |
| LocalTableFunctionState | unique_ptr | Query execution (per thread) |

### Creation Patterns

```cpp
// Use make_uniq for unique ownership
auto bind_data = make_uniq<MSSQLScanBindData>();

// Use make_shared_ptr for shared ownership
auto conn_info = make_shared_ptr<MSSQLConnectionInfo>();

// Use optional_ptr for non-owning references
optional_ptr<AttachedDatabase> db;
```

---

## 5. Thread Safety

### Context Manager

All `MSSQLContextManager` operations are protected by internal mutex:

```cpp
void MSSQLContextManager::RegisterContext(
    const string &name,
    shared_ptr<MSSQLContext> ctx
) {
    lock_guard<mutex> guard(lock);
    contexts[name] = std::move(ctx);
}
```

### Table Function State

- `GlobalTableFunctionState`: Shared across threads, use atomics for counters
- `LocalTableFunctionState`: Thread-local, no synchronization needed
- `FunctionData`: Immutable after bind, no synchronization needed

---

## 6. Extension Entry Point

```cpp
// mssql_extension.cpp

#include "mssql_extension.hpp"
#include "mssql_secret.hpp"
#include "mssql_storage.hpp"
#include "mssql_functions.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
    // 1. Register secrets
    RegisterMSSQLSecretType(loader);

    // 2. Register storage extension (ATTACH TYPE mssql)
    RegisterMSSQLStorageExtension(loader);

    // 3. Register table functions
    RegisterMSSQLFunctions(loader);

    // 4. Register utility functions (mssql_version)
    auto mssql_version_func = ScalarFunction(
        "mssql_version", {},
        LogicalType::VARCHAR,
        MssqlVersionFunction
    );
    loader.RegisterFunction(mssql_version_func);
}

}  // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(mssql, loader) {
    duckdb::LoadInternal(loader);
}
}
```
