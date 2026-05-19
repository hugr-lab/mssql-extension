# Contract: Metadata Cache SQL Templates

**Phase 1 Output** | **Date**: 2026-05-19

## Scope

Three `static const char *` template strings in `src/catalog/mssql_metadata_cache.cpp`:

| Template | Lines (current) | Caller |
| -------- | ---------------:| ------ |
| `TABLE_DISCOVERY_SQL_TEMPLATE`         | 44-53  | `MSSQLMetadataCache::EnsureTablesLoaded`, `LoadTables` |
| `SINGLE_TABLE_METADATA_SQL_TEMPLATE`   | 57-75  | `MSSQLMetadataCache::GetTableMetadata` |
| `BULK_METADATA_SCHEMA_SQL_TEMPLATE`    | 79-105 | `MSSQLMetadataCache::LoadAllTableMetadata`, `BulkLoadAll` |

## Row-shape contract

### `TABLE_DISCOVERY_SQL_TEMPLATE`

**Result columns** (order matters — callers index by position):

| Idx | Name | Type | Nullability |
| --: | ---- | ---- | ----------- |
| 0 | `object_name` | sysname / NVARCHAR(128) | NOT NULL |
| 1 | `object_type` | CHAR(2) | NOT NULL |
| 2 | `approx_rows` | BIGINT | NOT NULL (via `ISNULL(..., 0)`) |

**Row cardinality**: Exactly **one row per object** in the target schema where `o.type IN ('U', 'V')` and `o.is_ms_shipped = 0`. Independent of the number of partitions on any object.

**`approx_rows` semantics**: `COALESCE(SUM(p.rows), 0)` over all rows of `sys.partitions` matching the object's `object_id` with `index_id IN (0, 1)`.

### `SINGLE_TABLE_METADATA_SQL_TEMPLATE`

**Result columns** (order matters):

| Idx | Name | Type | Nullability |
| --: | ---- | ---- | ----------- |
| 0 | `object_type` | CHAR(2) | NOT NULL |
| 1 | `approx_rows` | BIGINT | NOT NULL |
| 2 | `column_name` | sysname / NVARCHAR(128) | NOT NULL |
| 3 | `column_id` | INT | NOT NULL |
| 4 | `type_name` | sysname / NVARCHAR(128) | NOT NULL (via `ISNULL`/`TYPE_NAME` fallback) |
| 5 | `max_length` | SMALLINT | NOT NULL |
| 6 | `precision` | TINYINT | NOT NULL |
| 7 | `scale` | TINYINT | NOT NULL |
| 8 | `is_nullable` | BIT | NOT NULL |
| 9 | `collation_name` | sysname / NVARCHAR(128) | NOT NULL (via `ISNULL(..., '')`) |

**Row cardinality**: Exactly **one row per `(object_id, column_id)` pair** for the object identified by `OBJECT_ID('<schema>.<table>')`. Independent of the number of partitions.

**`approx_rows` semantics**: As above. Identical value on every emitted row (callers only read it on the first row).

### `BULK_METADATA_SCHEMA_SQL_TEMPLATE`

**Result columns** (order matters):

| Idx | Name | Type | Nullability |
| --: | ---- | ---- | ----------- |
| 0 | `schema_name` | sysname / NVARCHAR(128) | NOT NULL |
| 1 | `object_name` | sysname / NVARCHAR(128) | NOT NULL |
| 2 | `object_type` | CHAR(2) | NOT NULL |
| 3 | `approx_rows` | BIGINT | NOT NULL |
| 4 | `column_name` | sysname / NVARCHAR(128) | NOT NULL |
| 5 | `column_id` | INT | NOT NULL |
| 6-11 | (as in single-table query) | | |

**Row cardinality**: Exactly **one row per `(schema, object_id, column_id)` triple** in the target schema. Independent of the number of partitions on any object.

## Required SQL shape

Each template MUST contain a `LEFT JOIN` against `sys.partitions` whose right-hand side is a derived table that:

1. Selects `object_id` and `SUM(rows) AS rows`.
2. Filters `WHERE index_id IN (0, 1)`.
3. Groups `BY object_id`.

The canonical form (used in all three templates):

```sql
LEFT JOIN (
    SELECT object_id, SUM(rows) AS rows
    FROM sys.partitions
    WHERE index_id IN (0, 1)
    GROUP BY object_id
) p ON p.object_id = o.object_id
```

Variations that satisfy the contract (any equivalent rewrite):

- Aliasing the derived table differently (e.g., `parts` instead of `p`), provided downstream references match.
- Placing the derived table earlier in the JOIN chain (semantics are unchanged for an outer reference via `o.object_id`).
- Using `COALESCE(SUM(rows), 0)` inside the derived table instead of `ISNULL(..., 0)` in the outer projection.

Variations that violate the contract:

- Direct `LEFT JOIN sys.partitions p ON ...` without aggregation — re-introduces the bug.
- `LEFT JOIN sys.partitions p ON ... AND p.partition_number = 1` — would only count partition 1's rows, silently undercounting partitioned tables.
- Dropping the `index_id IN (0, 1)` filter — would count rows once per non-clustered index per partition, inflating the row count by `(1 + N_nonclustered_indexes)`.
- Including `index_id` in the `GROUP BY` — would re-multiply rows when both heap (0) and a clustered index (1) exist for the same object (impossible by SQL Server's data model, but defensive: don't add it).

## Caller-side guarantees

Given a result that satisfies the row-shape contract:

- `MakeTableInfo` (`src/catalog/mssql_table_entry.cpp`) MUST NOT throw "Column with name X already exists!" for any well-formed SQL Server table.
- `MSSQLTableMetadata.approx_row_count` MUST equal the sum of `rows` across all heap/clustered partitions of the table.
- `TableStorageInfo.cardinality` (returned from `MSSQLTableEntry::GetStorageInfo`) MUST equal that sum.

## Unit-test guard (recommended)

A C++ unit test in `test/cpp/test_metadata_cache_queries.cpp` SHOULD assert, for each of the three templates, that the literal text contains:

- The substring `GROUP BY object_id`.
- The substring `SUM(rows)`.
- The substring `index_id IN (0, 1)`.

This is a structural guard, not a semantic test — it catches a future refactor that drops the aggregation but does not validate that the aggregation produces correct values. The integration test in `test/sql/catalog/partitioned_table.test` provides the semantic validation.
