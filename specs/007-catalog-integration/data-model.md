# Data Model: Catalog Integration & Read-Only SELECT with Pushdown

**Branch**: `007-catalog-integration`
**Date**: 2026-01-18
**Spec**: [spec.md](./spec.md)

---

## Entity Relationship Overview

```
┌─────────────────────┐     ┌─────────────────────┐
│   MSSQLCatalog      │────▶│  MSSQLSchemaEntry   │
│                     │ 1:N │                     │
│ - db (AttachedDB)   │     │ - name              │
│ - connection_pool   │     │ - tables (TableSet) │
│ - metadata_cache    │     │ - views (TableSet)  │
│ - db_collation      │     │ - parent_catalog    │
└─────────────────────┘     └─────────────────────┘
                                      │
                                      │ 1:N
                                      ▼
┌─────────────────────┐     ┌─────────────────────┐
│ MSSQLMetadataCache  │     │  MSSQLTableEntry    │
│                     │     │                     │
│ - schemas           │     │ - name              │
│ - tables            │     │ - object_type (T/V) │
│ - columns           │     │ - columns           │
│ - last_refresh_ts   │     │ - parent_schema     │
│ - ttl_seconds       │     │ - GetScanFunction() │
└─────────────────────┘     └─────────────────────┘
                                      │
                                      │ 1:N
                                      ▼
┌─────────────────────┐     ┌─────────────────────┐
│  MSSQLColumnInfo    │     │MSSQLFilterPushdown  │
│                     │     │                     │
│ - name              │     │ - columns           │
│ - sql_type_name     │     │ - filters           │
│ - duckdb_type       │     │ - parameters        │
│ - precision         │     │ - collation_map     │
│ - scale             │     │                     │
│ - max_length        │     │ + TranslateFilters()│
│ - is_nullable       │     │ + BuildParameters() │
│ - collation_name    │     └─────────────────────┘
│ - is_case_sensitive │              │
└─────────────────────┘              │ produces
                                      ▼
┌─────────────────────┐     ┌─────────────────────┐
│MSSQLPreparedStmt    │◀────│   MSSQLBindData     │
│                     │     │                     │
│ - statement_text    │     │ - catalog_name      │
│ - param_signature   │     │ - schema_name       │
│ - param_values      │     │ - table_name        │
│ - param_types       │     │ - column_ids        │
│                     │     │ - column_info       │
│ + Execute()         │     │ - filter_plan       │
│ + Stream()          │     │ - prepared_stmt     │
└─────────────────────┘     └─────────────────────┘
```

---

## Entity Definitions

### MSSQLCatalog

**Purpose**: DuckDB catalog representing an attached SQL Server database.

**Inherits**: `duckdb::Catalog`

| Field | Type | Description |
|-------|------|-------------|
| `db` | `AttachedDatabase&` | Parent DuckDB attached database |
| `connection_pool` | `shared_ptr<tds::ConnectionPool>` | From existing spec 003 |
| `metadata_cache` | `unique_ptr<MSSQLMetadataCache>` | Cached schemas/tables/columns |
| `db_collation` | `string` | Database default collation |
| `access_mode` | `AccessMode` | READ_ONLY (enforced for this spec) |
| `default_schema` | `string` | "dbo" by default |

**Key Methods**:
- `Initialize(bool load_builtin)` - Queries database default collation
- `LookupSchema(CatalogTransaction&, const string&)` - Returns MSSQLSchemaEntry or nullptr
- `ScanSchemas(CatalogTransaction&, function<void(SchemaCatalogEntry&)>)` - Lists all schemas
- `GetCatalogType()` - Returns "mssql"
- `GetDefaultSchema()` - Returns "dbo"

**Validation Rules**:
- Database connection must be established before operations
- Write operations (PlanInsert, PlanUpdate, PlanDelete) throw "Write operations not supported"

---

### MSSQLSchemaEntry

**Purpose**: DuckDB schema entry representing a SQL Server schema (e.g., dbo, hr).

**Inherits**: `duckdb::SchemaCatalogEntry`

| Field | Type | Description |
|-------|------|-------------|
| `name` | `string` | Schema name (e.g., "dbo") |
| `tables` | `MSSQLTableSet` | Lazy-loaded tables and views |
| `parent_catalog` | `MSSQLCatalog&` | Reference to parent catalog |

**Key Methods**:
- `LookupEntry(CatalogTransaction&, CatalogType, const string&)` - Returns table/view entry
- `Scan(CatalogType, function<void(CatalogEntry&)>)` - Lists tables/views
- `GetCatalogSet(CatalogType)` - Routes to appropriate internal set

**Validation Rules**:
- Schema name must exist in SQL Server sys.schemas
- CreateTable/DropEntry throw "Write operations not supported"

---

### MSSQLTableEntry

**Purpose**: DuckDB table entry representing a SQL Server table or view.

**Inherits**: `duckdb::TableCatalogEntry`

| Field | Type | Description |
|-------|------|-------------|
| `name` | `string` | Table/view name |
| `object_type` | `ObjectType` | TABLE or VIEW |
| `columns` | `vector<MSSQLColumnInfo>` | Column metadata with collation |
| `parent_schema` | `MSSQLSchemaEntry&` | Reference to parent schema |
| `approx_row_count` | `idx_t` | Cardinality estimate from sys.partitions |

**Key Methods**:
- `GetScanFunction(ClientContext&, TableFunctionBindInput&)` - Returns table function for SELECT
- `GetStatistics(ClientContext&, column_t)` - Returns column statistics
- `GetStorageInfo(ClientContext&)` - Returns storage metadata

**Validation Rules**:
- Must have at least one column
- object_type must be TABLE ('U') or VIEW ('V')

---

### MSSQLColumnInfo

**Purpose**: Column metadata including SQL Server-specific information.

| Field | Type | Description |
|-------|------|-------------|
| `name` | `string` | Column name |
| `column_id` | `int` | Ordinal position (1-based) |
| `sql_type_name` | `string` | SQL Server type name (e.g., "varchar", "int") |
| `duckdb_type` | `LogicalType` | Mapped DuckDB type |
| `max_length` | `int16_t` | Max length in bytes (-1 for MAX) |
| `precision` | `uint8_t` | Numeric precision |
| `scale` | `uint8_t` | Numeric scale |
| `is_nullable` | `bool` | Allows NULL values |
| `collation_name` | `string` | Column collation (may be empty for non-text) |
| `is_case_sensitive` | `bool` | Derived from collation (_CS_ or _BIN) |
| `is_unicode` | `bool` | True for NVARCHAR/NCHAR/NTEXT |
| `is_utf8` | `bool` | Derived from collation (_UTF8) |

**Validation Rules**:
- `name` must not be empty
- `column_id` must be positive
- `collation_name` must be valid if present (parseable)

**State Transitions**: N/A (immutable after construction)

---

### MSSQLMetadataCache

**Purpose**: In-memory cache of schema/table/column metadata with TTL support.

| Field | Type | Description |
|-------|------|-------------|
| `schemas` | `unordered_map<string, SchemaMetadata>` | Cached schema data |
| `last_refresh_ts` | `chrono::steady_clock::time_point` | Last full refresh timestamp |
| `ttl_seconds` | `int64_t` | Cache TTL (0 = manual refresh only) |
| `mutex` | `std::mutex` | Thread-safety for cache access |

**Nested Types**:

```cpp
struct SchemaMetadata {
    string name;
    unordered_map<string, TableMetadata> tables;
};

struct TableMetadata {
    string name;
    ObjectType type;
    vector<MSSQLColumnInfo> columns;
    idx_t approx_row_count;
};
```

**Key Methods**:
- `GetSchemas()` - Returns all cached schemas
- `GetTables(schema_name)` - Returns tables in schema
- `GetColumns(schema_name, table_name)` - Returns column info
- `Refresh(Connection&)` - Full cache refresh from SQL Server
- `IsExpired()` - Checks if TTL exceeded (when TTL > 0)
- `Invalidate()` - Marks cache as requiring refresh

**State Transitions**:

```
[Empty] ──Refresh()──▶ [Loaded] ──TTL expires──▶ [Stale] ──Refresh()──▶ [Loaded]
   │                       │                                  │
   └───────────────────────┴──────────Invalidate()────────────┘
                                              │
                                              ▼
                                          [Invalid]
```

**Validation Rules**:
- TTL must be >= 0 (0 disables auto-refresh)
- Refresh must complete within connection timeout

---

### MSSQLFilterPushdown

**Purpose**: Translates DuckDB filter expressions to SQL Server WHERE clauses with parameters.

| Field | Type | Description |
|-------|------|-------------|
| `columns` | `const vector<MSSQLColumnInfo>&` | Column metadata for type/collation lookup |
| `filters` | `optional<TableFilterSet>` | DuckDB filters from bind |
| `parameters` | `vector<MSSQLParameter>` | Generated parameter list |
| `where_clause` | `string` | Generated WHERE clause |
| `unsupported_filters` | `vector<idx_t>` | Column indices of non-pushed filters |

**Nested Types**:

```cpp
struct MSSQLParameter {
    string name;           // @p1, @p2, etc.
    string sql_type;       // "NVARCHAR(MAX)", "INT", etc.
    Value value;           // DuckDB Value
    string collation;      // For VARCHAR params
};
```

**Key Methods**:
- `TranslateFilters()` - Converts TableFilterSet to WHERE clause
- `BuildParameters()` - Generates parameter declarations and values
- `GetWhereClause()` - Returns generated WHERE string
- `GetUnsupportedFilters()` - Returns indices of filters kept local

**Supported Filter Types**:

| DuckDB Filter Type | SQL Server Translation |
|-------------------|----------------------|
| `CONSTANT_COMPARISON (=)` | `[col] = @pN` or `[col] = CONVERT(...) COLLATE ...` |
| `CONSTANT_COMPARISON (<>)` | `[col] <> @pN` |
| `CONSTANT_COMPARISON (<, <=, >, >=)` | `[col] <op> @pN` |
| `IS_NULL` | `[col] IS NULL` |
| `IS_NOT_NULL` | `[col] IS NOT NULL` |
| `CONJUNCTION_AND` | `(filter1) AND (filter2)` |
| `CONJUNCTION_OR` | `(filter1) OR (filter2)` |
| `IN` | `[col] IN (@p1, @p2, ...)` (up to limit) |

**Unsupported (kept local)**:
- Functions other than LOWER/UPPER/LENGTH/SUBSTR
- ILIKE on case-sensitive columns
- Complex expressions
- Filters with undetermined collation impact

**Validation Rules**:
- IN list size must not exceed configured limit (default 100)
- Collation must be determinable for text column filters

---

### MSSQLPreparedStatement

**Purpose**: Represents a prepared query for sp_executesql execution.

| Field | Type | Description |
|-------|------|-------------|
| `statement_text` | `string` | SQL statement with @pN placeholders |
| `param_signature` | `string` | Parameter type declaration string |
| `param_values` | `vector<MSSQLParameter>` | Bound parameter values |

**Key Methods**:
- `BuildExecuteSQL()` - Generates full sp_executesql call
- `GetStatementText()` - Returns parameterized SQL
- `GetParameterSignature()` - Returns @params string

**Generated SQL Pattern**:
```sql
EXEC sp_executesql
    N'SELECT [col1], [col2] FROM [schema].[table] WHERE [col1] = CONVERT(varchar(max), @p1) COLLATE SQL_Latin1_General_CP1_CI_AS',
    N'@p1 NVARCHAR(MAX)',
    @p1 = N'value';
```

**Validation Rules**:
- Statement text must be valid SQL
- Parameter count must match placeholders

---

### MSSQLBindData

**Purpose**: Bind-time data for table scan function.

**Inherits**: `duckdb::TableFunctionData`

| Field | Type | Description |
|-------|------|-------------|
| `catalog_name` | `string` | Attached catalog name |
| `schema_name` | `string` | Target schema |
| `table_name` | `string` | Target table/view |
| `column_ids` | `vector<column_t>` | Requested column indices |
| `column_info` | `vector<MSSQLColumnInfo>` | Full column metadata |
| `filter_plan` | `unique_ptr<MSSQLFilterPushdown>` | Translated filters |
| `prepared_stmt` | `unique_ptr<MSSQLPreparedStatement>` | Prepared query |
| `parent_catalog` | `MSSQLCatalog*` | Owning catalog reference |

---

## Type Mappings

### SQL Server to DuckDB Type Mapping

| SQL Server Type | DuckDB Type | Notes |
|-----------------|-------------|-------|
| `bit` | `BOOLEAN` | |
| `tinyint` | `UTINYINT` | |
| `smallint` | `SMALLINT` | |
| `int` | `INTEGER` | |
| `bigint` | `BIGINT` | |
| `real` | `FLOAT` | |
| `float` | `DOUBLE` | |
| `decimal(p,s)` / `numeric(p,s)` | `DECIMAL(p,s)` | |
| `money` | `DECIMAL(19,4)` | |
| `smallmoney` | `DECIMAL(10,4)` | |
| `char(n)` | `VARCHAR` | Fixed-length trimmed |
| `varchar(n)` | `VARCHAR` | |
| `varchar(max)` | `VARCHAR` | |
| `nchar(n)` | `VARCHAR` | Unicode |
| `nvarchar(n)` | `VARCHAR` | Unicode |
| `nvarchar(max)` | `VARCHAR` | Unicode |
| `text` | `VARCHAR` | Legacy |
| `ntext` | `VARCHAR` | Legacy Unicode |
| `date` | `DATE` | |
| `time` | `TIME` | |
| `datetime` | `TIMESTAMP` | |
| `datetime2` | `TIMESTAMP` | |
| `smalldatetime` | `TIMESTAMP` | |
| `datetimeoffset` | `TIMESTAMPTZ` | |
| `binary(n)` | `BLOB` | |
| `varbinary(n)` | `BLOB` | |
| `varbinary(max)` | `BLOB` | |
| `image` | `BLOB` | Legacy |
| `uniqueidentifier` | `UUID` | |

---

## SQL Server Metadata Queries

### Schema Discovery

```sql
SELECT s.name AS schema_name
FROM sys.schemas s
WHERE s.schema_id NOT IN (3, 4)  -- Exclude INFORMATION_SCHEMA, sys
  AND EXISTS (
    SELECT 1 FROM sys.tables t WHERE t.schema_id = s.schema_id
    UNION ALL
    SELECT 1 FROM sys.views v WHERE v.schema_id = s.schema_id
  )
ORDER BY s.name;
```

### Table/View Discovery

```sql
SELECT
    s.name AS schema_name,
    o.name AS object_name,
    o.type AS object_type,  -- 'U' = table, 'V' = view
    p.rows AS approx_rows
FROM sys.objects o
JOIN sys.schemas s ON o.schema_id = s.schema_id
LEFT JOIN sys.partitions p ON o.object_id = p.object_id AND p.index_id IN (0, 1)
WHERE o.type IN ('U', 'V')
  AND o.is_ms_shipped = 0
  AND s.name = @schema_name
ORDER BY o.name;
```

### Column Metadata

```sql
SELECT
    c.name AS column_name,
    c.column_id,
    t.name AS type_name,
    c.max_length,
    c.precision,
    c.scale,
    c.is_nullable,
    c.collation_name,
    DATABASEPROPERTYEX(DB_NAME(), 'Collation') AS db_collation
FROM sys.columns c
JOIN sys.types t ON c.user_type_id = t.user_type_id
WHERE c.object_id = OBJECT_ID(@full_object_name)
ORDER BY c.column_id;
```

### Database Default Collation

```sql
SELECT DATABASEPROPERTYEX(DB_NAME(), 'Collation') AS db_collation;
```
