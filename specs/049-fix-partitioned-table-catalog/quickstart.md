# Quickstart: Fix Partitioned Table Catalog

**Phase 1 Output** | **Date**: 2026-05-19

## The fix (5 minutes)

In `src/catalog/mssql_metadata_cache.cpp`, find each occurrence of:

```sql
LEFT JOIN sys.partitions p ON o.object_id = p.object_id AND p.index_id IN (0, 1)
```

and replace with:

```sql
LEFT JOIN (
    SELECT object_id, SUM(rows) AS rows
    FROM sys.partitions
    WHERE index_id IN (0, 1)
    GROUP BY object_id
) p ON p.object_id = o.object_id
```

There are three occurrences (one per SQL template at lines ~50, ~72, ~97).

## Build & test

```bash
# Build
GEN=ninja make

# Unit tests (no SQL Server needed)
./build/release/test/unittest

# Rebuild SQL Server container with the new partitioned-table fixture
make docker-down && make docker-up

# Run all integration tests (including new partitioned_table.test)
make integration-test
```

## Reproduce the bug (without the fix)

```sql
-- In SQL Server (SSMS or sqlcmd):
CREATE PARTITION FUNCTION [pf_qs_demo] (INT)
AS RANGE RIGHT FOR VALUES (10, 20, 30);
GO

CREATE PARTITION SCHEME [ps_qs_demo]
AS PARTITION [pf_qs_demo] ALL TO ([PRIMARY]);
GO

CREATE TABLE dbo.qs_demo (id INT NOT NULL, payload NVARCHAR(50));
INSERT INTO dbo.qs_demo VALUES (5, 'a'), (15, 'b'), (25, 'c'), (35, 'd');
CREATE CLUSTERED INDEX ix_qs_demo ON dbo.qs_demo (id)
    ON [ps_qs_demo] (id);
GO
```

```sql
-- In DuckDB:
LOAD mssql;
ATTACH 'Server=localhost,1433;Database=TestDB;User Id=sa;Password=TestPassword1'
    AS db (TYPE mssql);

SELECT * FROM db.dbo.qs_demo;
-- Before fix:  Catalog Error: Column with name id already exists!
-- After fix:   id | payload
--              ---+--------
--              5  | a
--              15 | b
--              25 | c
--              35 | d
```

## Verify the row count fix

```sql
-- In DuckDB after the fix:
SELECT estimated_size
FROM duckdb_tables()
WHERE database_name = 'db' AND table_name = 'qs_demo';
-- Should equal 4 (sum across all 4 partitions),
-- not 1 (the row count of whichever partition was first emitted).
```

## Cleanup (after testing)

```sql
-- In SQL Server:
DROP TABLE dbo.qs_demo;
DROP PARTITION SCHEME ps_qs_demo;
DROP PARTITION FUNCTION pf_qs_demo;
```

## Files modified

1. `src/catalog/mssql_metadata_cache.cpp` — three SQL template strings.
2. `docker/init/init-partitioned-tables.sql` — new fixture file with `dbo.PartitionedLog` + seed rows.
3. `docker/docker-compose.yml` — mount/load the new init file (if not already auto-loaded by container init).
4. `test/sql/catalog/partitioned_table.test` — new SQLLogicTest regression test.
5. *(optional)* `test/cpp/test_metadata_cache_queries.cpp` — unit-test guard for the aggregation marker.
6. `CLAUDE.md` — append "Recent Changes" entry referencing spec 049.

## Files NOT modified

- Any header file — no API surface change.
- `src/include/catalog/mssql_metadata_cache.hpp` — only the SQL strings change; cache structures, lazy loading, TTL logic all unchanged.
- `src/catalog/mssql_table_entry.cpp` — `MakeTableInfo` is unchanged; it stops throwing because the duplicate rows stop arriving.
- Any other `src/` file — bug is isolated to three SQL string literals.
