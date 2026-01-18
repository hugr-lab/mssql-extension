# Contract: Table Function API

**Branch**: `007-catalog-integration`
**Date**: 2026-01-18

This contract defines the table scan function interface for MSSQL extension SELECT operations.

---

## Table Function Registration

### Function Signature

```cpp
TableFunction CreateMSSQLTableScanFunction() {
    TableFunction func("mssql_table_scan", {}, MSSQLTableScan);

    // Enable pushdown optimizations
    func.projection_pushdown = true;
    func.filter_pushdown = true;
    func.filter_prune = true;

    // Callbacks
    func.bind = MSSQLTableScanBind;
    func.init_global = MSSQLTableScanInitGlobal;
    func.init_local = MSSQLTableScanInitLocal;
    func.function = MSSQLTableScan;
    func.cardinality = MSSQLTableScanCardinality;

    return func;
}
```

---

## Bind Data Structure

```cpp
struct MSSQLBindData : public TableFunctionData {
    // Table identification
    string catalog_name;
    string schema_name;
    string table_name;

    // Column metadata
    vector<string> column_names;
    vector<LogicalType> column_types;
    vector<MSSQLColumnInfo> mssql_columns;

    // Catalog reference
    MSSQLCatalog *catalog;
    MSSQLTableEntry *table_entry;

    // Cardinality
    idx_t approx_row_count;

    // Database collation fallback
    string database_collation;
};
```

---

## Global State Structure

```cpp
struct MSSQLGlobalState : public GlobalTableFunctionState {
    // Connection from pool
    tds::PooledConnection connection;

    // Query execution state
    unique_ptr<MSSQLResultStream> result_stream;

    // Statistics
    atomic<idx_t> rows_scanned;

    // Synchronization
    mutex lock;

    idx_t MaxThreads() const override {
        return 1;  // Single-threaded for this spec
    }
};
```

---

## Local State Structure

```cpp
struct MSSQLLocalState : public LocalTableFunctionState {
    // Column projection
    vector<column_t> column_ids;

    // Filter pushdown results
    unique_ptr<MSSQLFilterPushdown> filter_plan;
    unique_ptr<MSSQLPreparedStatement> prepared_stmt;

    // Generated SQL
    string query_sql;

    // Current chunk being filled
    DataChunk current_chunk;

    // Scan completion flag
    bool scan_complete;
};
```

---

## Function Callbacks

### Bind

```cpp
unique_ptr<FunctionData> MSSQLTableScanBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names);
```

**Contract**:
- Input: Table reference from catalog entry via `GetScanFunction()`
- Output: `MSSQLBindData` populated with column metadata
- Side effect: Sets `return_types` and `names` for DuckDB schema
- Error: Throws if table not found or connection fails

### InitGlobal

```cpp
unique_ptr<GlobalTableFunctionState> MSSQLTableScanInitGlobal(
    ClientContext &context,
    TableFunctionInitInput &input);
```

**Contract**:
- Input: Bind data
- Output: Global state with connection acquired from pool
- Side effect: None
- Error: Throws if connection pool exhausted

### InitLocal

```cpp
unique_ptr<LocalTableFunctionState> MSSQLTableScanInitLocal(
    ExecutionContext &context,
    TableFunctionInitInput &input,
    GlobalTableFunctionState *global_state);
```

**Contract**:
- Input: Bind data, column_ids (projection), filters (filter pushdown)
- Output: Local state with generated SQL query
- Side effect: Translates filters to SQL WHERE clause
- Behavior:
  - Only requested columns included in SELECT (projection pushdown)
  - Supported filters translated to WHERE clause (filter pushdown)
  - Unsupported filters recorded for local evaluation

### Scan Function

```cpp
void MSSQLTableScan(
    ClientContext &context,
    TableFunctionInput &data,
    DataChunk &output);
```

**Contract**:
- Input: Global and local state
- Output: DataChunk filled with rows
- Behavior:
  - First call: Executes query via sp_executesql
  - Subsequent calls: Streams results via MSSQLResultStream
  - Final call: Sets output size to 0, returns connection to pool
- Error: Throws IOException on SQL Server errors

### Cardinality

```cpp
unique_ptr<NodeStatistics> MSSQLTableScanCardinality(
    ClientContext &context,
    const FunctionData *bind_data_p);
```

**Contract**:
- Input: Bind data
- Output: Cardinality estimate from sys.partitions
- Used for: Query optimizer planning

---

## SQL Generation

### Projection Pushdown

Generated SQL includes only requested columns:

```sql
-- If column_ids = {0, 2, 5}
SELECT [col0], [col2], [col5] FROM [schema].[table]

-- If all columns requested
SELECT [col0], [col1], [col2], ... FROM [schema].[table]
```

### Filter Pushdown with sp_executesql

```sql
-- Generated sp_executesql call
EXEC sp_executesql
    N'SELECT [id], [name] FROM [dbo].[customers]
      WHERE [name] = CONVERT(varchar(max), @p1) COLLATE SQL_Latin1_General_CP1_CI_AS
        AND [age] > @p2',
    N'@p1 NVARCHAR(MAX), @p2 INT',
    @p1 = N'Smith',
    @p2 = 30;
```

### Identifier Quoting

All identifiers use SQL Server bracket quoting:
- `]` escaped as `]]`
- Example: `[My]Table]` becomes `[My]]Table]`

```cpp
string QuoteIdentifier(const string &name) {
    string result = "[";
    for (char c : name) {
        if (c == ']') {
            result += "]]";
        } else {
            result += c;
        }
    }
    result += "]";
    return result;
}
```

---

## Error Codes

| Error | Description | DuckDB Exception |
|-------|-------------|------------------|
| SQL Server connection failed | Network/auth error | `IOException` |
| SQL Server query error | Syntax/permission error | `IOException` |
| Table not found | Object doesn't exist | `CatalogException` |
| Column not found | Invalid column reference | `CatalogException` |
| Write attempted | INSERT/UPDATE/DELETE | `NotImplementedException` |
