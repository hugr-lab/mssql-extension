# Data Model: Partitioned Table Metadata Queries

**Phase 1 Output** | **Date**: 2026-05-19

## `sys.partitions` row multiplication

`sys.partitions` is a SQL Server DMV with the schema:

| Column | Type | Notes |
| ------ | ---- | ----- |
| `partition_id` | BIGINT | Unique per partition |
| `object_id` | INT | The table or indexed view |
| `index_id` | INT | 0 = heap; 1 = clustered index; ≥ 2 = non-clustered |
| `partition_number` | INT | 1-based within the (object, index) pair |
| `rows` | BIGINT | Approximate row count, maintained by stats |

For a **non-partitioned heap**: one row with `(object_id, 0, 1, <rows>)`.
For a **non-partitioned clustered table**: one row with `(object_id, 1, 1, <rows>)`.
For a **partitioned clustered table with N partitions**: N rows with `(object_id, 1, k, <rows_in_partition_k>)` for `k = 1..N`.

The current extension query says:

```sql
LEFT JOIN sys.partitions p
    ON o.object_id = p.object_id
    AND p.index_id IN (0, 1)
```

For an N-partition clustered table this multiplies the outer row by N.

## Outer query shapes (before / after)

### `SINGLE_TABLE_METADATA_SQL_TEMPLATE`

**Before**:

```sql
SELECT
    o.type AS object_type,
    ISNULL(p.rows, 0) AS approx_rows,
    c.name AS column_name,
    c.column_id,
    ISNULL(t.name, TYPE_NAME(c.user_type_id)) AS type_name,
    c.max_length, c.precision, c.scale, c.is_nullable,
    ISNULL(c.collation_name, '') AS collation_name
FROM sys.objects o
INNER JOIN sys.columns c ON c.object_id = o.object_id
LEFT JOIN sys.types t ON c.system_type_id = t.user_type_id AND t.system_type_id = t.user_type_id
LEFT JOIN sys.partitions p ON o.object_id = p.object_id AND p.index_id IN (0, 1)
WHERE o.object_id = OBJECT_ID('%s')
ORDER BY c.column_id
```

For a 4-partition table with 2 columns: 2 × 4 = 8 rows. Each column appears 4 times.

**After**:

```sql
SELECT
    o.type AS object_type,
    ISNULL(p.rows, 0) AS approx_rows,
    c.name AS column_name,
    c.column_id,
    ISNULL(t.name, TYPE_NAME(c.user_type_id)) AS type_name,
    c.max_length, c.precision, c.scale, c.is_nullable,
    ISNULL(c.collation_name, '') AS collation_name
FROM sys.objects o
INNER JOIN sys.columns c ON c.object_id = o.object_id
LEFT JOIN sys.types t ON c.system_type_id = t.user_type_id AND t.system_type_id = t.user_type_id
LEFT JOIN (
    SELECT object_id, SUM(rows) AS rows
    FROM sys.partitions
    WHERE index_id IN (0, 1)
    GROUP BY object_id
) p ON p.object_id = o.object_id
WHERE o.object_id = OBJECT_ID('%s')
ORDER BY c.column_id
```

For the same table: 2 rows. Each column appears once. `approx_rows = SUM(rows)`.

### `BULK_METADATA_SCHEMA_SQL_TEMPLATE`

Same transformation applied to the per-schema bulk query. Row count formula: was `Σ(columns × partitions)` per table; now `Σ(columns)` per table.

### `TABLE_DISCOVERY_SQL_TEMPLATE`

**Before**:

```sql
SELECT
    o.name AS object_name,
    o.type AS object_type,
    ISNULL(p.rows, 0) AS approx_rows
FROM sys.objects o
LEFT JOIN sys.partitions p ON o.object_id = p.object_id AND p.index_id IN (0, 1)
WHERE o.type IN ('U', 'V')
  AND o.is_ms_shipped = 0
  AND SCHEMA_NAME(o.schema_id) = '%s'
```

For a 4-partition table this emits 4 rows with the same `object_name`. `std::map::emplace` keeps only the first; `approx_rows` is the row count of whichever partition `sys.partitions` happens to emit first. This is the silent miscount.

**After**:

```sql
SELECT
    o.name AS object_name,
    o.type AS object_type,
    ISNULL(p.rows, 0) AS approx_rows
FROM sys.objects o
LEFT JOIN (
    SELECT object_id, SUM(rows) AS rows
    FROM sys.partitions
    WHERE index_id IN (0, 1)
    GROUP BY object_id
) p ON p.object_id = o.object_id
WHERE o.type IN ('U', 'V')
  AND o.is_ms_shipped = 0
  AND SCHEMA_NAME(o.schema_id) = '%s'
```

One row per table. `approx_rows = SUM(per-partition rows)`.

## Worked example: issue #85's `dbo.log` table

Schema (from the issue):

```sql
CREATE PARTITION FUNCTION [pf_logs_by_month] (DATETIME2(7))
AS RANGE RIGHT FOR VALUES (
    '2026-02-01 00:00:00',
    '2026-03-01 00:00:00',
    '2026-04-01 00:00:00'
);

CREATE PARTITION SCHEME [ps_logs_by_month]
AS PARTITION [pf_logs_by_month] ALL TO ([PRIMARY]);

CREATE TABLE dbo.log (
    log_ts  DATETIME2(7) DEFAULT (GETUTCDATE()) NOT NULL,
    message NVARCHAR(255)
);

INSERT INTO dbo.log (message) VALUES ('bar');

CREATE CLUSTERED INDEX ix_ci_log_log_ts
    ON dbo.log (log_ts DESC)
    WITH (DATA_COMPRESSION = PAGE, OPTIMIZE_FOR_SEQUENTIAL_KEY = ON)
    ON [ps_logs_by_month] (log_ts);
```

3 boundary values → 4 partitions. After the single INSERT (which lands in whichever partition contains `GETUTCDATE()` at insert time), `sys.partitions` reports:

| partition_number | rows |
| ---------------: | ---: |
| 1                | 0    |
| 2                | 1    |
| 3                | 0    |
| 4                | 0    |

**Before fix** — `SINGLE_TABLE_METADATA` returns 8 rows:

```
object_type | approx_rows | column_name | column_id
U           | 0           | log_ts      | 1
U           | 1           | log_ts      | 1
U           | 0           | log_ts      | 1
U           | 0           | log_ts      | 1
U           | 0           | message     | 2
U           | 1           | message     | 2
U           | 0           | message     | 2
U           | 0           | message     | 2
```

`MakeTableInfo` loops these rows, calling `info.columns.AddColumn("log_ts")` four times. The second call throws "Column with name log_ts already exists!" → user sees the error from issue #85.

**After fix** — `SINGLE_TABLE_METADATA` returns 2 rows:

```
object_type | approx_rows | column_name | column_id
U           | 1           | log_ts      | 1
U           | 1           | message     | 2
```

`MakeTableInfo` adds each column once. `approx_rows = SUM(0+1+0+0) = 1`. Query succeeds.

## Aggregation cost analysis

For a schema with T tables and P average partitions per table:

| Stage | Pre-fix | Post-fix |
| ----- | ------- | -------- |
| `sys.partitions` rows scanned | T × P | T × P |
| Rows materialized to client | T × P × avg_columns | T × avg_columns |
| Client-side processing | Multiplied by P (then deduped or crashed) | Linear in columns |

The server-side hash/stream aggregate adds an `O(T × P)` operator with `O(T)` state. For typical ERP / OLTP schemas (T ≤ 10,000, P ≤ 50), this is a few-millisecond overhead at most. Net effect: fewer rows on the wire, slightly more CPU on the server, lower client memory.

## Related cache state

`MSSQLTableMetadata.approx_row_count` (in `src/include/catalog/mssql_metadata_cache.hpp`) currently holds whatever the first emitted partition row had. After the fix, it holds `SUM(rows)`. Consumers of this field:

- `MSSQLTableEntry::GetStorageInfo` → `TableStorageInfo.cardinality` (used by DuckDB's planner for join ordering and filter selectivity).
- `MSSQLStatisticsProvider::PreloadRowCount` (used as the cached row count for `mssql_pool_stats()`).
- `MSSQLPreloadCatalog` summary message (used for the user-visible "Preloaded N schemas, M tables, K columns" return value).

All three consumers benefit from the corrected value — none of them have any assumption that the count must be "less than or equal to" the true count.
