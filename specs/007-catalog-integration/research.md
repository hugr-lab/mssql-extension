# Research: Catalog Integration & Read-Only SELECT with Pushdown

**Branch**: `007-catalog-integration`
**Date**: 2026-01-18

This document consolidates research findings for implementing catalog integration, filter/projection pushdown, and collation-aware parameter binding.

---

## DuckDB Catalog API Patterns

### Decision
Implement custom catalog using DuckDB's extension API following patterns established by duckdb-postgres.

### Rationale
The postgres_scanner extension provides a battle-tested reference implementation for remote database catalog integration. Following these patterns ensures compatibility with DuckDB's catalog system.

### Architecture Overview

#### Core Catalog Classes

**Catalog Hierarchy:**
```
Catalog (base)
  └── MSSQLCatalog (custom)
        ├── MSSQLSchemaSet (lazy-loaded schemas)
        ├── ConnectionPool (from existing specs)
        └── MetadataCache (new)
```

**Schema Entry Pattern:**
```
SchemaCatalogEntry (base)
  └── MSSQLSchemaEntry
        ├── MSSQLTableSet (lazy-loaded tables)
        └── MSSQLTypeSet (type mappings)
```

**Table Entry Pattern:**
```
TableCatalogEntry (base)
  └── MSSQLTableEntry
        ├── ColumnMetadata (with collation)
        └── GetScanFunction() → TableFunction
```

#### Key Virtual Methods

**Catalog class must implement:**
- `Initialize(bool load_builtin)` - Initial setup
- `LookupSchema(CatalogTransaction, string&)` - Schema lookup
- `ScanSchemas(CatalogTransaction, function<void(SchemaCatalogEntry&)>)` - List schemas
- `GetCatalogType()` - Returns "mssql"
- `GetDefaultSchema()` - Returns "dbo"
- `InMemory()` - Returns false

**SchemaEntry must implement:**
- `CreateTable(...)` - For future write support (currently throws)
- `LookupEntry(...)` - Lazy load table/view entries
- `Scan(CatalogType, function<void(CatalogEntry&)>)` - List objects

**TableEntry must implement:**
- `GetScanFunction(...)` - Returns table function for SELECT
- `GetStatistics(...)` - Cardinality estimates
- `GetStorageInfo(...)` - Storage metadata

### Lazy Loading Pattern

**CatalogSet base class provides:**
```cpp
class MSSQLCatalogSet {
    atomic<bool> is_loaded;
    mutex load_lock;
    unordered_map<string, shared_ptr<CatalogEntry>> entries;

    void TryLoadEntries(MSSQLTransaction &);
    virtual void LoadEntries(MSSQLTransaction &) = 0;  // Subclass implements
};
```

**TableSet implementation queries sys.* catalogs:**
```sql
SELECT
    s.name AS schema_name,
    t.name AS table_name,
    t.type AS object_type,
    c.name AS column_name,
    c.column_id,
    ty.name AS type_name,
    c.max_length,
    c.precision,
    c.scale,
    c.is_nullable,
    c.collation_name,
    DATABASEPROPERTYEX(DB_NAME(), 'Collation') AS db_collation
FROM sys.tables t
JOIN sys.schemas s ON t.schema_id = s.schema_id
JOIN sys.columns c ON t.object_id = c.object_id
JOIN sys.types ty ON c.user_type_id = ty.user_type_id
ORDER BY s.name, t.name, c.column_id;
```

### Table Function Integration

**Scan function configuration:**
```cpp
TableFunction CreateMSSQLScanFunction() {
    TableFunction func;
    func.projection_pushdown = true;   // Enable column pruning
    func.filter_pushdown = true;       // Enable filter pushdown
    func.filter_prune = true;          // Remove filtered columns if not needed
    return func;
}
```

**Bind data structure:**
```cpp
struct MSSQLBindData : public TableFunctionData {
    string catalog_name;
    string schema_name;
    string table_name;
    vector<string> column_names;
    vector<LogicalType> column_types;
    vector<MSSQLColumnInfo> mssql_columns;  // With collation info
    shared_ptr<MSSQLMetadataCache> cache;
};
```

**Filter pushdown in Init:**
```cpp
unique_ptr<LocalTableFunctionState> MSSQLInitLocal(...) {
    auto &bind_data = input.bind_data.Cast<MSSQLBindData>();
    auto &column_ids = input.column_ids;   // Requested columns
    auto &filters = input.filters;          // TableFilterSet

    // Build SQL with pushdown
    string sql = BuildPushedQuery(bind_data, column_ids, filters);
    return make_uniq<MSSQLLocalState>(sql);
}
```

### Storage Extension Registration

```cpp
class MSSQLStorageExtension : public StorageExtension {
public:
    MSSQLStorageExtension() {
        attach = MSSQLAttach;
        create_transaction_manager = MSSQLCreateTransactionManager;
    }
};

static unique_ptr<Catalog> MSSQLAttach(
    StorageExtensionInfo *,
    ClientContext &context,
    AttachedDatabase &db,
    const string &name,
    AttachInfo &info,
    AccessMode access_mode) {

    auto secret = GetSecretFromInfo(context, info);
    return make_uniq<MSSQLCatalog>(db, secret, access_mode);
}
```

### Alternatives Considered

- **Direct table functions only**: Rejected; doesn't provide catalog browsing via SHOW SCHEMAS/TABLES
- **Virtual tables without catalog**: Rejected; requires explicit table function calls instead of natural syntax
- **Full catalog sync on attach**: Rejected; too slow for large schemas, lazy loading preferred

---

## sp_executesql Best Practices

### Decision
Use sp_executesql with NVARCHAR parameters for all pushed predicates.

### Rationale
sp_executesql provides SQL injection protection, execution plan caching, and proper type handling for parameterized queries.

### Basic Syntax

```sql
EXEC sp_executesql
    @stmt = N'SELECT [col1], [col2] FROM [schema].[table] WHERE [col1] = @p1 AND [col2] > @p2',
    @params = N'@p1 NVARCHAR(MAX), @p2 INT',
    @p1 = N'value',
    @p2 = 100;
```

**Key Requirements:**
- `@stmt` must be NVARCHAR (prefixed with N)
- `@params` must be NVARCHAR
- Parameter names follow `@pN` pattern
- Multiple parameters are comma-separated

### Execution Plan Caching

**How it works:**
1. First execution compiles query plan
2. Plan cached based on statement text + parameter signature
3. Subsequent executions reuse cached plan
4. Only parameter values change between executions

**Benefits over EXEC:**
- EXEC creates new plan for each unique SQL string
- sp_executesql reuses plans for parameterized queries
- Significant CPU reduction for repeated queries

**Parameter Sniffing:**
- SQL Server "sniffs" values on first execution
- Cached plan may not be optimal for different value distributions
- Mitigation: OPTION (RECOMPILE) for highly variable data

### Type Coercion Rules

**Critical Issue: NVARCHAR vs VARCHAR**

When comparing NVARCHAR parameter to VARCHAR column:
```sql
-- This causes implicit conversion on the COLUMN, breaking indexes
DECLARE @param NVARCHAR(100) = N'value';
SELECT * FROM Table WHERE varchar_column = @param;
-- Execution plan shows: CONVERT_IMPLICIT(nvarchar, [varchar_column])
-- Result: Index SCAN instead of SEEK
```

**Solution: Match types precisely**
```sql
-- For VARCHAR columns, use VARCHAR parameter type
EXEC sp_executesql
    N'SELECT * FROM Table WHERE varchar_column = @p1',
    N'@p1 VARCHAR(100)',  -- Not NVARCHAR!
    @p1 = 'value';
```

**However, TDS wire protocol consideration:**
- TDS transmits text as UTF-16LE (NVARCHAR on wire)
- We must use CONVERT with COLLATE on the parameter side

### Collation-Safe Binding Pattern

For VARCHAR columns with explicit collation:

```sql
EXEC sp_executesql
    N'SELECT * FROM [dbo].[customers]
      WHERE [name] = CONVERT(varchar(max), @p1) COLLATE SQL_Latin1_General_CP1_CI_AS',
    N'@p1 NVARCHAR(MAX)',
    @p1 = N'Smith';
```

**Why this works:**
1. Parameter sent as NVARCHAR (UTF-16 on wire)
2. CONVERT transforms to VARCHAR
3. COLLATE specifies exact column collation
4. Column side untouched → index seekable

**Pattern for different column types:**

| Column Type | Parameter Declaration | WHERE Clause Pattern |
|-------------|----------------------|---------------------|
| VARCHAR(n) with collation | @p1 NVARCHAR(MAX) | `[col] = CONVERT(varchar(max), @p1) COLLATE <col_collation>` |
| NVARCHAR(n) | @p1 NVARCHAR(MAX) | `[col] = @p1` (no conversion needed) |
| INT | @p1 INT | `[col] = @p1` |
| DATETIME | @p1 DATETIME | `[col] = @p1` |

### Parameter Limits

- NVARCHAR(MAX) supports up to 1 billion characters
- Practical limit: server memory
- No hard limit on parameter count
- IN lists: prefer multiple OR with parameters over single IN (up to limit)

### Alternatives Considered

- **Literal formatting**: Rejected; SQL injection risk, no plan caching
- **sp_prepare/sp_execute**: Rejected; requires handle management, complexity
- **EXEC(@sql)**: Rejected; no plan reuse, injection vulnerable

---

## SQL Server Collation Handling

## 1. Collation Detection from Collation Names

### Decision
Parse collation names to extract case sensitivity, accent sensitivity, and other properties using well-defined naming patterns.

### Rationale
SQL Server collation names follow predictable patterns that encode their properties directly in the name, allowing efficient detection without additional database queries.

### Collation Naming Pattern

Collation names follow the format:
```
<CollationDesignator>_<LocaleVersion>_<ComparisonStyle>[_SC][_UTF8]
```

**Key Components:**
- **CollationDesignator**: Base collation rules (e.g., Latin1_General, Japanese_Bushu_Kakusu_100)
- **LocaleVersion**: Unicode version support
  - `_90_`: Unicode 4.1
  - `_100_`: Unicode 5.0
  - `_140_`: Unicode 9.0
- **ComparisonStyle**: Sensitivity flags
  - `CS` = Case-Sensitive
  - `CI` = Case-Insensitive
  - `AS` = Accent-Sensitive
  - `AI` = Accent-Insensitive
  - `KS` = Kana-Sensitive
  - `KI` = Kana-Insensitive
  - `WS` = Width-Sensitive
  - `WI` = Width-Insensitive
- **_SC**: Supplementary Characters support (Unicode surrogate pairs)
- **_UTF8**: UTF-8 encoding (SQL Server 2019+)

**Common Examples:**
- `SQL_Latin1_General_CP1_CI_AS`: SQL collation, case-insensitive, accent-sensitive
- `Latin1_General_100_CS_AS_SC`: Windows collation, case-sensitive, accent-sensitive, supplementary characters
- `Japanese_Bushu_Kakusu_100_CS_AS_KS_WS_SC_UTF8`: All sensitivity options enabled with UTF-8

### Detection Algorithm

```cpp
// Pseudocode for detecting case sensitivity
bool is_case_sensitive(const std::string& collation_name) {
    // Binary collations (_BIN, _BIN2) are always case-sensitive
    if (collation_name.find("_BIN") != std::string::npos) {
        return true;
    }
    // Check for explicit CS marker
    return collation_name.find("_CS_") != std::string::npos ||
           collation_name.find("_CS") == (collation_name.length() - 3);
}

bool is_accent_sensitive(const std::string& collation_name) {
    return collation_name.find("_AS_") != std::string::npos ||
           collation_name.find("_AS") == (collation_name.length() - 3);
}

bool is_utf8_enabled(const std::string& collation_name) {
    return collation_name.find("_UTF8") != std::string::npos;
}
```

### Programmatic Property Detection

SQL Server provides two system functions for detailed collation inspection:

#### 1. sys.fn_helpcollations()

Returns all available collations with descriptions:

```sql
SELECT name, description
FROM sys.fn_helpcollations()
WHERE name = 'Latin1_General_100_CI_AS';
```

#### 2. COLLATIONPROPERTY()

Returns specific properties of a collation:

```sql
SELECT
    COLLATIONPROPERTY('Latin1_General_CI_AS', 'LCID') AS LocaleID,
    COLLATIONPROPERTY('Latin1_General_CI_AS', 'CodePage') AS CodePage,
    COLLATIONPROPERTY('Latin1_General_CI_AS', 'Version') AS Version;
```

**Available Properties:**
- `LCID`: Locale Identifier (Windows LCID)
- `CodePage`: Code page number for VARCHAR storage
- `Version`: Collation version (0-3)

### Default Collation

The default server-level collation for "English (United States)" installations is `SQL_Latin1_General_CP1_CI_AS` (case-insensitive, accent-sensitive).

### Alternatives Considered
- Runtime database queries for every collation check: Rejected; adds query overhead
- Hardcoded collation mappings: Rejected; not maintainable, breaks with new SQL Server versions

---

## 2. Windows vs SQL Collations

### Decision
Prefer Windows collations (e.g., `Latin1_General_CI_AS`) over SQL collations (e.g., `SQL_Latin1_General_CP1_CI_AS`) for better Unicode handling and performance.

### Key Differences

| Aspect | SQL Collations | Windows Collations |
|--------|---------------|-------------------|
| **Unicode Handling** | Different algorithms for Unicode vs non-Unicode | Same algorithm for both |
| **Index Usage** | Cannot use index for Unicode/non-Unicode comparison | Can use index for mixed comparisons |
| **Character Expansion** | No character expansion | Supports character expansion (æ → ae) |
| **Naming Pattern** | Starts with `SQL_` | No `SQL_` prefix |
| **Performance** | Index scan for mixed types | Index seek for mixed types |
| **Recommendation** | Legacy, avoid for new development | Preferred for SQL Server 2008+ |

**Example:**
- SQL Collation: `SQL_Latin1_General_CP1_CI_AS`
- Windows Collation: `Latin1_General_CI_AS`

### Performance Impact

Windows collations enable index seeks when comparing VARCHAR and NVARCHAR columns, while SQL collations force index scans. For mixed Unicode/non-Unicode workloads, Windows collations are significantly faster.

### Rationale
- SQL collations are legacy from SQL Server 2000 and earlier
- Windows collations introduced in SQL Server 2008 provide better Unicode compliance
- Query optimizer makes better choices with Windows collations

---

## 3. Sargable COLLATE Syntax for Query Pushdown

### Decision
Apply `COLLATE` to the parameter/literal side of comparisons, never to the indexed column side.

### Rationale
Wrapping an indexed column in a COLLATE expression makes it non-sargable, preventing index seeks and forcing table/index scans.

### Sargable vs Non-Sargable Patterns

**❌ Non-Sargable (Breaks Index Usage):**
```sql
-- Function applied to indexed column
SELECT * FROM Customers
WHERE CustomerName COLLATE Latin1_General_CI_AS = @param;

-- Even with matching collation
SELECT * FROM Customers
WHERE CustomerName COLLATE SQL_Latin1_General_CP1_CS_AS = 'Smith';
```

**✅ Sargable (Preserves Index Usage):**
```sql
-- COLLATE applied to parameter/literal
SELECT * FROM Customers
WHERE CustomerName = @param COLLATE Latin1_General_CI_AS;

-- Or use DATABASE_DEFAULT
SELECT * FROM Customers
WHERE CustomerName = @param COLLATE DATABASE_DEFAULT;

-- No COLLATE if collations already match
SELECT * FROM Customers
WHERE CustomerName = 'Smith';  -- Best when column and param collations match
```

### Query Pushdown Strategy

When generating SQL for remote execution:

1. **Match Existing Collations**: If column and parameter collations match, omit COLLATE entirely
2. **Apply to Constants**: If mismatch, apply COLLATE to the constant/parameter side:
   ```sql
   WHERE column_name = 'value' COLLATE Latin1_General_CI_AS
   ```
3. **Use DATABASE_DEFAULT**: When database collation is acceptable:
   ```sql
   WHERE column_name = 'value' COLLATE DATABASE_DEFAULT
   ```

### Convert Expressions

The `CONVERT` function with `COLLATE` can also be used but follows the same rule:

```sql
-- ❌ Non-sargable
WHERE CONVERT(VARCHAR, column_name) COLLATE Latin1_General_CI_AS = @param

-- ✅ Sargable
WHERE column_name = CONVERT(VARCHAR, @param COLLATE Latin1_General_CI_AS)
```

### Execution Plan Impact

When COLLATE is applied to a column, the execution plan shows:
- `CONVERT_IMPLICIT` warning
- Index Scan instead of Index Seek
- Estimated rows may be inaccurate
- Significantly higher CPU and I/O costs

---

## 4. Collation Mismatch and Index Usage

### Decision
Detect collation mismatches early and apply mitigation strategies to preserve index usage.

### Common Mismatch Scenarios

#### 1. TempDB Collation Mismatch

**Problem**: User database collation differs from TempDB (which inherits server collation).

```sql
-- Error example
Cannot resolve the collation conflict between
'SQL_Latin1_General_CP437_CI_AS' and 'Latin1_General_CI_AS'
in the equal to operation
```

**Solution**: Use `COLLATE DATABASE_DEFAULT` in temp table definitions:

```sql
CREATE TABLE #temp (
    id INT,
    name VARCHAR(100) COLLATE DATABASE_DEFAULT
);
```

**Severity**: High - causes runtime errors in production when temp tables join with user tables.

#### 2. Cross-Database Queries

**Problem**: Different databases on same server may have different collations.

**Solution**: Explicit COLLATE on one side of join:

```sql
SELECT *
FROM DB1.dbo.Customers c
JOIN DB2.dbo.Orders o
  ON c.CustomerID = o.CustomerID COLLATE DATABASE_DEFAULT
```

#### 3. Linked Server Queries

**Problem**: Remote server may have different collation.

**Impact**:
- If "use remote collation" is TRUE: SQL Server assumes compatibility and pushes predicates to remote server
- If "use remote collation" is FALSE: SQL Server pulls all data locally and filters (huge performance hit)

**Solution**: Apply COLLATE to local side when collations differ:

```sql
SELECT *
FROM LinkedServer.RemoteDB.dbo.Customers
WHERE CustomerName COLLATE Latin1_General_CI_AS = @local_param
```

**Configuration Check**:
```sql
EXEC sp_serveroption @server='LinkedServerName',
     @optname='use remote collation',
     @optvalue='true';
```

**Warning**: Setting "use remote collation" to TRUE with incompatible collations can cause data inconsistencies.

#### 4. VARCHAR vs NVARCHAR Mixing

**Problem**: Comparing VARCHAR to NVARCHAR with different collations causes implicit conversion.

**Impact**:
- Index on VARCHAR column cannot be used when compared to NVARCHAR parameter
- CONVERT_IMPLICIT appears in execution plan
- Performance degrades to table scan

**Solution**: Match data types or use explicit CONVERT on parameter side:

```sql
-- ✅ Explicit conversion on parameter side
WHERE varchar_column = CONVERT(VARCHAR(100), @nvarchar_param)

-- ✅ Better: use matching parameter type
WHERE varchar_column = @varchar_param
```

### Performance Measurements

Real-world impact of collation mismatches:
- **10x slower execution**: Documented cases of queries executing 10 times slower due to collation conversions
- **Index scan vs seek**: Table scans vs index seeks can be 100-1000x slower on large tables
- **CPU usage**: Implicit conversions increase CPU consumption significantly

### Detection in Query Plans

Look for these indicators in execution plans:
- `CONVERT_IMPLICIT` warnings
- Index Scan operators where Index Seek expected
- "Type conversion in expression may affect CardinalityEstimate" warnings

---

## 5. Database Default Collation Fallback

### Decision
Implement collation hierarchy: Column → Table → Database → Server.

### Collation Hierarchy

SQL Server resolves collations in this order:

```
1. Column-level COLLATE clause (highest priority)
   ↓ (if not specified)
2. Column definition collation
   ↓ (if not specified)
3. Database default collation
   ↓ (if not specified during database creation)
4. Server instance collation (lowest priority)
```

### Fallback Behavior

#### New Database
```sql
-- No collation specified → inherits server collation
CREATE DATABASE MyDB;

-- Explicit collation
CREATE DATABASE MyDB
COLLATE Latin1_General_100_CI_AS;
```

#### New Table/Column
```sql
-- Column inherits database collation
CREATE TABLE Users (
    Name VARCHAR(100)  -- Uses database collation
);

-- Column with explicit collation
CREATE TABLE Users (
    Name VARCHAR(100) COLLATE Latin1_General_CS_AS
);
```

### Querying Collation Hierarchy

**Server Collation:**
```sql
SELECT SERVERPROPERTY('Collation') AS ServerCollation;
```

**Database Collation:**
```sql
SELECT DATABASEPROPERTYEX('MyDB', 'Collation') AS DBCollation;
-- Or from sys.databases
SELECT name, collation_name FROM sys.databases;
```

**Column Collation:**
```sql
SELECT
    t.name AS TableName,
    c.name AS ColumnName,
    c.collation_name
FROM sys.columns c
JOIN sys.tables t ON c.object_id = t.object_id
WHERE c.collation_name IS NOT NULL
  AND t.is_ms_shipped = 0;
```

**For Specific Table:**
```sql
SELECT name, collation_name
FROM sys.columns
WHERE object_id = OBJECT_ID('dbo.Customers')
  AND collation_name IS NOT NULL;
```

### DATABASE_DEFAULT Keyword

The `COLLATE DATABASE_DEFAULT` clause explicitly requests the current database's default collation:

```sql
-- Temp table inherits current database collation instead of TempDB
CREATE TABLE #temp (
    name VARCHAR(100) COLLATE DATABASE_DEFAULT
);

-- Query joins using database collation
SELECT * FROM RemoteTable
WHERE RemoteColumn COLLATE DATABASE_DEFAULT = LocalColumn;
```

**Use Cases:**
- Temp tables in databases with non-standard collations
- Cross-database queries where one database should dominate
- Standardizing collation in stored procedures used across databases

### Fallback Character Handling

When converting between collations with incompatible code pages:

**Non-UTF-8 Collations:**
- Incompatible characters replaced with "fallback character" (often `?`)
- Example: Converting from Unicode to CP1252 loses characters outside CP1252 range
- **Data Loss Risk**: Silent data corruption

**UTF-8 Collations (SQL Server 2019+):**
- Full Unicode range available for VARCHAR
- No fallback characters needed
- No data loss when converting from NVARCHAR to VARCHAR with UTF-8 collation

**Example of Fallback:**
```sql
-- Assuming CP1252 (Western European)
SELECT 'Hello世界' COLLATE Latin1_General_CI_AS;
-- Result: 'Hello??' (Chinese characters lost)

SELECT 'Hello世界' COLLATE Latin1_General_100_CI_AS_SC_UTF8;
-- Result: 'Hello世界' (preserved with UTF-8)
```

### Query Pushdown Strategy

When generating remote SQL, use this fallback strategy:

1. **Use column collation if known** from catalog metadata
2. **Use database default collation** if column collation is NULL (non-character columns)
3. **Use COLLATE DATABASE_DEFAULT** when database context is reliable
4. **Avoid cross-collation comparisons** in pushed predicates when possible

---

## 6. VARCHAR vs NVARCHAR Collation Considerations

### Decision
Understand VARCHAR/NVARCHAR differences for correct collation handling and query pushdown.

### Core Differences

| Aspect | VARCHAR | NVARCHAR |
|--------|---------|----------|
| **Encoding** | Code page-dependent (1 byte/char typically) | UTF-16LE (2 bytes/char) |
| **Code Page** | Determined by collation | Always Unicode |
| **Storage** | Defined in bytes | Defined in character pairs |
| **Collation Dependency** | Highly collation-dependent | Uses Unicode collation |
| **Max Length** | VARCHAR(8000) = 8000 bytes | NVARCHAR(4000) = 8000 bytes |
| **UTF-8 Support** | Yes (SQL Server 2019+ with _UTF8 collation) | N/A (already Unicode) |
| **International Characters** | Limited to code page | Full Unicode support |

### Storage Considerations

**VARCHAR:**
- `VARCHAR(50)` = 50 bytes of storage
- With single-byte collation (e.g., CP1252): stores up to 50 ASCII characters
- With UTF-8 collation: stores up to 50 bytes, which may be fewer than 50 characters
  - Example: `VARCHAR(20)` with UTF-8 cannot store "räksmörgås" (10 chars, 13 bytes in UTF-8)

**NVARCHAR:**
- `NVARCHAR(50)` = 100 bytes of storage (50 character pairs)
- Always stores up to 50 Unicode characters regardless of content
- `NVARCHAR(10)` requires 22 bytes (20 for data + 2 for overhead)

### UTF-8 VARCHAR (SQL Server 2019+)

**Game Changer**: VARCHAR with UTF-8 collation behaves like NVARCHAR but with variable storage:

```sql
-- Traditional approach
CREATE TABLE Products (
    Name NVARCHAR(100)  -- 200 bytes + 2 overhead
);

-- UTF-8 approach (SQL Server 2019+)
CREATE TABLE Products (
    Name VARCHAR(100) COLLATE Latin1_General_100_CI_AS_SC_UTF8
    -- 100 bytes max, but uses 1 byte for ASCII, 2-4 bytes for other Unicode
);
```

**Storage Savings:**
- ASCII-heavy data: ~50% reduction vs NVARCHAR
- Mixed international data: Variable (depends on character mix)

**Detection:**
```sql
-- Check if column uses UTF-8
SELECT
    c.name AS ColumnName,
    c.collation_name,
    CASE
        WHEN c.collation_name LIKE '%_UTF8' THEN 'UTF-8'
        ELSE 'Non-UTF-8'
    END AS EncodingType
FROM sys.columns c
WHERE c.object_id = OBJECT_ID('Products');
```

### Implicit Conversion Issues

**VARCHAR to NVARCHAR Comparison:**

```sql
-- Schema
CREATE TABLE Orders (
    OrderID VARCHAR(10) COLLATE Latin1_General_CI_AS
);

-- Query with NVARCHAR parameter
DECLARE @param NVARCHAR(10) = N'12345';
SELECT * FROM Orders WHERE OrderID = @param;
```

**Result:**
- Execution plan shows `CONVERT_IMPLICIT(nvarchar(10), [OrderID])`
- Index on `OrderID` cannot be used (index scan instead of seek)
- Significant performance degradation

**Fix 1: Match Parameter Type**
```sql
DECLARE @param VARCHAR(10) = '12345';  -- VARCHAR, not NVARCHAR
SELECT * FROM Orders WHERE OrderID = @param;
```

**Fix 2: Explicit Conversion on Parameter**
```sql
DECLARE @param NVARCHAR(10) = N'12345';
SELECT * FROM Orders
WHERE OrderID = CONVERT(VARCHAR(10), @param);
```

### N Prefix for Literals

**Impact on Index Usage:**

```sql
-- ❌ Bad: NVARCHAR literal compared to VARCHAR column
SELECT * FROM Orders WHERE OrderID = N'12345';
-- Causes implicit conversion, breaks index usage

-- ✅ Good: VARCHAR literal matches column type
SELECT * FROM Orders WHERE OrderID = '12345';
-- Index can be used efficiently
```

**Rule**: Only use `N` prefix when:
1. Target column is NVARCHAR
2. Literal contains non-ASCII Unicode characters
3. You need Unicode semantics

### Collation Interaction

**VARCHAR Collation Changes Behavior:**

```sql
-- CP1252 collation (Western European)
CREATE TABLE T1 (
    Name VARCHAR(100) COLLATE Latin1_General_CI_AS
);

-- UTF-8 collation (full Unicode)
CREATE TABLE T2 (
    Name VARCHAR(100) COLLATE Latin1_General_100_CI_AS_SC_UTF8
);

INSERT INTO T1 VALUES ('日本語');  -- May lose data (? ? ?)
INSERT INTO T2 VALUES ('日本語');  -- Preserved (UTF-8)
```

**NVARCHAR Collation Independent:**

```sql
CREATE TABLE T3 (
    Name NVARCHAR(100) COLLATE Latin1_General_CI_AS
);

INSERT INTO T3 VALUES (N'日本語');  -- Always preserved (UTF-16)
```

### Query Pushdown Implications

When generating SQL for remote execution:

1. **Detect VARCHAR vs NVARCHAR from catalog:**
   ```sql
   SELECT
       c.name,
       t.name AS type_name,
       c.collation_name
   FROM sys.columns c
   JOIN sys.types t ON c.user_type_id = t.user_type_id
   WHERE c.object_id = OBJECT_ID('TableName');
   ```

2. **Match parameter types in generated SQL:**
   ```sql
   -- If column is VARCHAR
   WHERE column_name = 'literal'  -- No N prefix

   -- If column is NVARCHAR
   WHERE column_name = N'literal'  -- Use N prefix
   ```

3. **Handle UTF-8 VARCHAR specially:**
   ```sql
   -- Detect UTF-8
   IF collation_name LIKE '%_UTF8':
       -- Can safely push Unicode predicates
       WHERE varchar_utf8_column = 'Unicode文字列'
   ```

4. **Avoid cross-type comparisons in pushed queries:**
   ```sql
   -- ❌ Don't push this (causes conversion on remote)
   WHERE varchar_column = nvarchar_column

   -- ✅ Handle locally or add explicit CONVERT
   WHERE varchar_column = CONVERT(VARCHAR(100), nvarchar_column)
   ```

### Best Practices for Query Pushdown

1. **Preserve source type**: If DuckDB parameter is a string, infer target SQL Server type from catalog
2. **Use N prefix only when needed**: Only for NVARCHAR columns to avoid unnecessary conversions
3. **Detect UTF-8 VARCHAR**: These can handle Unicode despite being VARCHAR
4. **Don't push mixed-type predicates**: VARCHAR vs NVARCHAR comparisons better handled locally
5. **Monitor for CONVERT_IMPLICIT**: If generated queries show this warning, adjust type matching

---

## 7. Binary Collations (BIN vs BIN2)

### Overview

Binary collations provide byte-level comparison without linguistic rules, offering performance benefits but with important semantic differences.

### BIN vs BIN2

| Aspect | BIN | BIN2 |
|--------|-----|------|
| **Algorithm** | Hybrid: First WCHAR, then byte-by-byte | Pure Unicode code point comparison |
| **Unicode Compliance** | Non-standard | Unicode code point order |
| **NVARCHAR Performance** | ~10x faster than Windows collation | ~10x faster than Windows collation |
| **VARCHAR Performance** | Minimal improvement over standard | Minimal improvement over standard |
| **Recommendation** | Legacy, avoid | Preferred for binary sorting |

### Important Semantics

**Binary Collations Are NOT Simple Case-Sensitive Equivalents:**

```sql
-- BIN2 collation
SELECT * FROM Users
WHERE Name = 'Smith' COLLATE Latin1_General_BIN2;

-- This is NOT the same as
SELECT * FROM Users
WHERE Name = 'Smith' COLLATE Latin1_General_CS_AS;
```

**Why They Differ:**
- BIN2 compares raw byte values (e.g., 'A' = 0x41, 'a' = 0x61)
- CS_AS applies linguistic case rules (e.g., 'ß' vs 'SS' in German)
- BIN2 has no concept of equivalent character forms

**Example:**
```sql
-- Case-sensitive Windows collation
'café' = 'café'  -- TRUE (accent insensitive with AI)
'café' = 'cafe'  -- TRUE

-- BIN2 collation
'café' = 'café'  -- TRUE
'café' = 'cafe'  -- FALSE (different byte sequences)
```

### Performance Characteristics

**NVARCHAR with BIN2:**
- ~10x faster than linguistic collation for sorts and comparisons
- Suitable for high-throughput scenarios where linguistic accuracy not needed

**VARCHAR with BIN2:**
- Marginal improvement over `SQL_Latin1_General_CP1_CI_AS`
- Not worth the semantic trade-offs for most use cases

### Use Cases

**Good For:**
- Hash keys (consistent byte-level comparison)
- Binary data stored as VARCHAR/NVARCHAR
- Performance-critical joins on NVARCHAR where linguistic rules not needed

**Not Good For:**
- User-facing text (names, addresses, descriptions)
- Any scenario requiring linguistic equivalence
- Case-insensitive searches (BIN2 is always case-sensitive)

### Query Pushdown Implications

When encountering binary collations:

1. **Detect BIN/BIN2:**
   ```cpp
   bool is_binary_collation =
       collation_name.find("_BIN") != std::string::npos;
   ```

2. **Preserve in pushed queries:**
   ```sql
   WHERE binary_column = 'value' COLLATE Latin1_General_BIN2
   ```

3. **Warning**: Don't assume BIN2 = case-sensitive for user filters:
   - User expects linguistic case-sensitivity
   - BIN2 provides byte-level sensitivity
   - These are not equivalent

---

## 8. Implicit Conversion Detection

### Problem Statement

Implicit conversions occur when SQL Server must convert data types or collations at runtime, often causing severe performance degradation.

### Common Causes

1. **Data Type Mismatch:**
   ```sql
   -- INT column compared to VARCHAR literal
   WHERE IntColumn = '123'  -- CONVERT_IMPLICIT(int, '123')
   ```

2. **Collation Mismatch:**
   ```sql
   -- Different collations on each side
   WHERE Col1 COLLATE Latin1_General_CI_AS =
         Col2 COLLATE SQL_Latin1_General_CP1_CI_AS
   ```

3. **VARCHAR vs NVARCHAR:**
   ```sql
   -- VARCHAR column vs NVARCHAR parameter
   WHERE VarCharColumn = @NVarCharParam
   ```

### Detection in Execution Plans

**Execution Plan Warnings:**
- "Type conversion in expression (CONVERT_IMPLICIT...) may affect 'CardinalityEstimate'"
- `CONVERT_IMPLICIT` function wrapped around column reference
- Index Scan instead of Index Seek

**Query to Find Implicit Conversions:**
```sql
-- Capture from Query Store (SQL Server 2016+)
SELECT
    qsp.query_plan,
    qsqt.query_sql_text
FROM sys.query_store_plan qsp
JOIN sys.query_store_query qsq ON qsp.query_id = qsq.query_id
JOIN sys.query_store_query_text qsqt ON qsq.query_text_id = qsqt.query_text_id
WHERE CAST(qsp.query_plan AS NVARCHAR(MAX)) LIKE '%CONVERT_IMPLICIT%';
```

### Performance Impact

**Measured Effects:**
- Index seeks → Index scans (100-1000x slower on large tables)
- Increased CPU consumption during conversion
- Inaccurate cardinality estimates
- Missing index recommendations not logged

**Example:**
```sql
-- Non-sargable due to implicit conversion
SELECT * FROM LargeTable WHERE IntColumn = '12345';
-- Execution time: 5000ms (table scan)

-- Sargable with correct type
SELECT * FROM LargeTable WHERE IntColumn = 12345;
-- Execution time: 5ms (index seek)
```

### Mitigation Strategies

**1. Fix at Source (Best):**
```sql
-- Match types exactly
DECLARE @param INT = 12345;  -- Not VARCHAR
SELECT * FROM Table WHERE IntColumn = @param;
```

**2. Explicit Conversion on Non-Indexed Side:**
```sql
-- Convert parameter, not column
SELECT * FROM Table
WHERE IntColumn = CONVERT(INT, @varchar_param);
```

**3. Persisted Computed Column (Advanced):**
```sql
-- Add computed column with index
ALTER TABLE Table
ADD ConvertedColumn AS CONVERT(VARCHAR(20), IntColumn) PERSISTED;

CREATE INDEX IX_Converted ON Table(ConvertedColumn);

-- Query uses computed column
SELECT * FROM Table WHERE ConvertedColumn = @varchar_param;
```

### Query Pushdown Strategy

When generating SQL for remote execution:

1. **Type Matching**: Match DuckDB types to SQL Server types precisely:
   ```
   DuckDB VARCHAR → SQL Server VARCHAR (not NVARCHAR)
   DuckDB INTEGER → SQL Server INT (not VARCHAR)
   ```

2. **Collation Awareness**: Include COLLATE only when necessary, on parameter side:
   ```sql
   WHERE column = @param COLLATE Latin1_General_CI_AS
   ```

3. **Avoid Mixed-Type Predicates**: Don't push predicates that cause conversion:
   ```sql
   -- ❌ Don't push
   WHERE varchar_col = nvarchar_col

   -- ✅ Handle locally in DuckDB
   ```

4. **Testing**: Validate generated SQL plans don't contain `CONVERT_IMPLICIT` on indexed columns

---

## References

### Microsoft Official Documentation
- [Collation and Unicode Support - SQL Server | Microsoft Learn](https://learn.microsoft.com/en-us/sql/relational-databases/collations/collation-and-unicode-support?view=sql-server-ver17)
- [COLLATE (Transact-SQL) - SQL Server | Microsoft Learn](https://learn.microsoft.com/en-us/sql/t-sql/statements/collations?view=sql-server-ver17)
- [View Collation Information - SQL Server | Microsoft Learn](https://learn.microsoft.com/en-us/sql/relational-databases/collations/view-collation-information?view=sql-server-ver17)
- [sys.fn_helpcollations (Transact-SQL) - SQL Server | Microsoft Learn](https://learn.microsoft.com/en-us/sql/relational-databases/system-functions/sys-fn-helpcollations-transact-sql?view=sql-server-ver17)
- [Set or Change the Database Collation - SQL Server | Microsoft Learn](https://learn.microsoft.com/en-us/sql/relational-databases/collations/set-or-change-the-database-collation?view=sql-server-ver17)

### Technical Articles
- [How column COLLATION can affect SQL Server query performance - MSSQLTips](https://www.mssqltips.com/sqlservertip/3215/how-column-collation-can-affect-sql-server-query-performance/)
- [Understanding the COLLATE DATABASE_DEFAULT clause in SQL Server - MSSQLTips](https://www.mssqltips.com/sqlservertip/4395/understanding-the-collate-databasedefault-clause-in-sql-server/)
- [The Curious Case of Collations and Performance in SQL Server - Eitan Blumin](https://eitanblumin.com/2018/10/28/the-curious-case-of-collations-and-performance-in-sql-server/)
- [Learn How Implicit Conversions in SQL Server affect Query Performance - MSSQLTips](https://www.mssqltips.com/sqlservertip/7732/implicit-conversions-in-sql-affect-query-performance/)
- [Implicit Conversion - Brent Ozar Unlimited](https://www.brentozar.com/blitz/implicit-conversion/)

### Specialized Topics
- [A quick look at SQL Server UTF-8 collations - SQLSunday](https://sqlsunday.com/2023/11/08/sql-server-utf8-collations/)
- [Introducing UTF-8 support for SQL Server | Microsoft Community Hub](https://techcommunity.microsoft.com/blog/sqlserver/introducing-utf-8-support-for-sql-server/734928)
- [The difference between BIN2 and Case-Sensitive collations in SQL - SQLServerCentral](https://www.sqlservercentral.com/articles/the-difference-between-bin2-and-case-sensitive-collations-in-sql)
- [SQL Server's Binary Collations | Microsoft Learn](https://learn.microsoft.com/en-us/archive/blogs/qingsongyao/sql-servers-binary-collations)
- [How It Works: Linked Servers and Collation Compatibility | Microsoft Community Hub](https://techcommunity.microsoft.com/t5/sql-server-support-blog/how-it-works-linked-servers-and-collation-compatibility/ba-p/315460)

### Comparison and Best Practices
- [Revised: Difference between collation SQL_Latin1_General_CP1_CI_AS and Latin1_General_CI_AS - SQLServerCentral](https://www.sqlservercentral.com/blogs/revised-difference-between-collation-sql_latin1_general_cp1_ci_as-and-latin1_general_ci_as)
- [VARCHAR Versus NVARCHAR in SQL: Which Character Data Type Should You Select - SQLPey](https://sqlpey.com/sql-server/varchar-vs-nvarchar-selection-guide/)
- [Transact-SQL: VARCHAR vs. NVARCHAR and the 'N' Prefix Explained - SQLPey](https://sqlpey.com/sql/varchar-vs-nvarchar-n-prefix/)
- [Create SQL Server temporary tables with the correct collation - MSSQLTips](https://www.mssqltips.com/sqlservertip/2440/create-sql-server-temporary-tables-with-the-correct-collation/)

### Query Optimization
- [SQL Collation and related performance impact, viewing collation in query plans | Microsoft Learn](https://learn.microsoft.com/en-us/archive/blogs/sql_pfe_blog/sql-collation-and-related-performance-impact-viewing-collation-in-query-plans)
- [How (not) to kill your SQL Server performance with implicit conversion - Data Mozart](https://data-mozart.com/how-not-to-kill-your-sql-server-performance-with-implicit-conversion/)
- [When SQL Server Performance Goes Bad: Implicit Conversions | Redgate](https://www.red-gate.com/hub/product-learning/redgate-monitor/when-sql-server-performance-goes-bad-implicit-conversions)
