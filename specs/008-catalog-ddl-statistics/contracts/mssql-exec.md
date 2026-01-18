# Contract: mssql_exec Function

**Feature**: 008-catalog-ddl-statistics
**Module**: `mssql_functions`

## Purpose

Provides a scalar function to execute arbitrary T-SQL statements on SQL Server, returning the affected row count.

## Interface

```cpp
namespace duckdb {

//! Register mssql_exec function
void RegisterMSSQLExecFunction(DatabaseInstance &db);

//! mssql_exec function implementation
//! @param secret_name Name of the DuckDB secret containing connection info
//! @param sql T-SQL statement to execute
//! @return Number of affected rows (or 0 for statements without row counts)
struct MSSQLExecFunction {
    static ScalarFunction GetFunction();

    //! Function name
    static constexpr const char *NAME = "mssql_exec";

    //! Function signature
    //! mssql_exec(secret_name VARCHAR, sql VARCHAR) -> BIGINT
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result);
};

} // namespace duckdb
```

## Function Signature

```sql
mssql_exec(secret_name VARCHAR, sql VARCHAR) -> BIGINT
```

### Parameters

| Parameter | Type | Description |
| --------- | ---- | ----------- |
| secret_name | VARCHAR | Name of DuckDB secret with MSSQL connection info |
| sql | VARCHAR | T-SQL statement to execute |

### Return Value

- **BIGINT**: Number of rows affected by the statement
- Returns `0` for statements that don't affect rows (e.g., CREATE, DROP)
- Returns `-1` if row count is unavailable

## Behavior

### Normal Execution

1. Look up secret by name from DuckDB's secret manager
2. Get or create connection from pool
3. Execute T-SQL statement on SQL Server
4. Return affected row count
5. Release connection back to pool

### Read-Only Mode Check

```cpp
// Inside mssql_exec implementation
auto &catalog = GetMSSQLCatalogFromSecret(secret_name);
if (catalog.IsReadOnly()) {
    throw InvalidInputException(
        "Cannot execute mssql_exec: catalog '%s' is attached in read-only mode",
        catalog.GetName()
    );
}
```

### Error Handling

SQL Server errors are surfaced as DuckDB exceptions:

```cpp
try {
    connection.Execute(sql);
} catch (const TdsException &e) {
    throw InvalidInputException(
        "MSSQL execution error: SQL Server error %d, state %d, class %d: %s",
        e.error_number(), e.state(), e.severity(), e.message()
    );
}
```

## Usage Examples

### Create Table

```sql
SELECT mssql_exec('my_mssql_secret', 'CREATE TABLE dbo.test (id INT, name NVARCHAR(50))');
-- Returns: 0
```

### Insert Data

```sql
SELECT mssql_exec('my_mssql_secret', 'INSERT INTO dbo.test VALUES (1, ''Alice''), (2, ''Bob'')');
-- Returns: 2
```

### Update Data

```sql
SELECT mssql_exec('my_mssql_secret', 'UPDATE dbo.test SET name = ''Charlie'' WHERE id = 1');
-- Returns: 1
```

### Execute Stored Procedure

```sql
SELECT mssql_exec('my_mssql_secret', 'EXEC dbo.my_procedure @param1 = 42');
-- Returns: row count from procedure (or 0)
```

### Administrative Commands

```sql
SELECT mssql_exec('my_mssql_secret', 'TRUNCATE TABLE dbo.test');
-- Returns: 0 (TRUNCATE doesn't report row count)
```

## Error Examples

### Read-Only Mode

```sql
-- Attach in read-only mode
ATTACH 'mssql://server/db' AS mssql (TYPE mssql, SECRET my_secret, READ_ONLY);

-- Attempt to execute
SELECT mssql_exec('my_secret', 'CREATE TABLE test (id INT)');
-- Error: Cannot execute mssql_exec: catalog 'mssql' is attached in read-only mode
```

### SQL Server Error

```sql
SELECT mssql_exec('my_secret', 'DROP TABLE nonexistent_table');
-- Error: MSSQL execution error: SQL Server error 3701, state 1, class 11:
--        Cannot drop the table 'nonexistent_table', because it does not exist or you do not have permission.
```

### Invalid Secret

```sql
SELECT mssql_exec('nonexistent_secret', 'SELECT 1');
-- Error: Secret 'nonexistent_secret' not found
```

## Security Considerations

- `mssql_exec` executes arbitrary T-SQL with the permissions of the connected user
- No query filtering or validation is performed
- Users should use READ_ONLY mode when DDL/DML should be blocked
- Secrets should be managed carefully (not exposed in logs)

## Relation to DDL Hooks

`mssql_exec` is the underlying execution mechanism used by DDL hooks:

```
User: CREATE TABLE mssql.dbo.test (id INT);
           │
           ▼
MSSQLSchemaEntry::CreateTable()
           │
           ▼
MSSQLDDLTranslator::TranslateCreateTable()
           │
           ▼
Generated T-SQL: "CREATE TABLE [dbo].[test] ([id] INT NOT NULL);"
           │
           ▼
MSSQLCatalog::ExecuteDDL() (uses same mechanism as mssql_exec)
           │
           ▼
SQL Server executes T-SQL
```
