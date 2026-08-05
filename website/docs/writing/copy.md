---
title: COPY TO (Bulk Load)
sidebar_position: 1
---

# COPY TO (Bulk Load)

High-performance bulk data transfer using the native TDS BulkLoadBCP protocol. Significantly faster than INSERT for large datasets.

### Target Formats

The COPY TO command supports two target formats:

| Format | Syntax | Example |
|--------|--------|---------|
| **URL** | `mssql://catalog/schema/table` | `mssql://sqlserver/dbo/my_table` |
| **Catalog** | `catalog.schema.table` | `sqlserver.dbo.my_table` |

Both formats are equivalent and can be used interchangeably.

#### Empty Schema Syntax for Temp Tables

Temp tables can use an empty schema notation for clarity:

| Format | Standard Syntax | Empty Schema Syntax |
|--------|-----------------|---------------------|
| **URL** | `mssql://catalog/#temp` | `mssql://catalog//#temp` |
| **Catalog** | `catalog.#temp` | `catalog..#temp` |

Both syntaxes are equivalent for temp tables. The empty schema syntax (`//` or `..`) explicitly shows there's no schema component.

### Basic COPY TO

```sql
-- Copy DuckDB table to SQL Server (URL format)
COPY my_local_table TO 'mssql://sqlserver/dbo/target_table' (FORMAT 'bcp');

-- Copy DuckDB table to SQL Server (catalog format)
COPY my_local_table TO 'sqlserver.dbo.target_table' (FORMAT 'bcp');

-- Copy query results to SQL Server
COPY (SELECT * FROM source WHERE year = 2024) TO 'mssql://sqlserver/dbo/target_table' (FORMAT 'bcp');

-- Generate data and copy to SQL Server
COPY (SELECT i AS id, 'row_' || i AS name FROM range(1000000) t(i))
  TO 'sqlserver.dbo.million_rows' (FORMAT 'bcp');
```

### COPY TO Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `CREATE_TABLE` | BOOLEAN | true | Auto-create target table if it doesn't exist |
| `REPLACE` | BOOLEAN | false | Drop and recreate table (replaces existing data) |
| `FLUSH_ROWS` | BIGINT | 100000 | Rows before flushing to SQL Server (overrides setting) |
| `TABLOCK` | BOOLEAN | false | Use TABLOCK hint for faster bulk load (overrides setting) |

```sql
-- Auto-create table (default: true)
COPY data TO 'mssql://sqlserver/dbo/new_table' (FORMAT 'bcp', CREATE_TABLE true);

-- Replace existing table (drop and recreate)
COPY data TO 'mssql://sqlserver/dbo/existing_table' (FORMAT 'bcp', REPLACE true);

-- Control flush frequency (rows before committing to SQL Server)
COPY data TO 'sqlserver.dbo.table' (FORMAT 'bcp', FLUSH_ROWS 500000);

-- Disable TABLOCK hint (allows concurrent access, slower)
COPY data TO 'sqlserver.dbo.table' (FORMAT 'bcp', TABLOCK false);
```

### Temporary Tables

Temp tables are prefixed with `#` (local) or `##` (global). They require a transaction context to remain accessible.

```sql
-- Local temp table using URL format (session-scoped, requires transaction)
BEGIN;
COPY data TO 'mssql://sqlserver/#temp_table' (FORMAT 'bcp');
SELECT * FROM mssql_scan('sqlserver', 'SELECT * FROM #temp_table');
COMMIT;

-- Local temp table using catalog format
BEGIN;
COPY data TO 'sqlserver.#temp_table' (FORMAT 'bcp');
SELECT * FROM mssql_scan('sqlserver', 'SELECT * FROM #temp_table');
COMMIT;

-- Empty schema syntax (equivalent alternatives)
BEGIN;
COPY data TO 'mssql://sqlserver//#temp_table' (FORMAT 'bcp');  -- URL with empty schema
COPY data TO 'sqlserver..#temp_table' (FORMAT 'bcp');          -- Catalog with empty schema
COMMIT;

-- Global temp table (visible to all sessions)
COPY data TO 'mssql://sqlserver/##global_temp' (FORMAT 'bcp');
```

> **Note**: Temp tables have no schema component. Use `catalog.#table`, `catalog..#table`, `mssql://catalog/#table`, or `mssql://catalog//#table` format.

### COPY TO Settings

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `mssql_copy_flush_rows` | BIGINT | 102400 | Rows per batch — the boundary the **server** sees, not a client buffer. 102400 is SQL Server's own threshold for writing a batch straight into a compressed columnstore rowgroup; below it every row goes to the delta store, so a smaller value defeats `table_kind = 'columnstore'` entirely |
| `mssql_copy_tablock` | VARCHAR | `auto` | `auto` \| `true` \| `false`. `auto` decides from the target's shape: **on** for a heap, **off** for anything clustered — see below |
| `mssql_copy_parallel_writers` | BIGINT | 0 | Concurrent bulk-load connections one COPY or CTAS may open. `0` derives it from DuckDB's thread count, capped at 8; `1` disables parallel loading |

`mssql_copy_flush_rows` was 100000 and `mssql_copy_tablock` was a `BOOLEAN`
defaulting to `false`; both changed in spec 057, and the reasons are measurements
rather than taste:

* at 100000 rows a columnstore load produced **one OPEN rowgroup and 53 MB**; at
  102400, **nine COMPRESSED rowgroups and 7 MB** (1M rows). The extra 2400 rows
  cost nothing on a heap, where batch size measured nearly flat;
* TABLOCK helps a heap, where concurrent loaders take mutually compatible BU
  locks, and **serialises** everything clustered. On 2M rows into a clustered
  columnstore: hint on 8.92 s with the server pinned at one core, hint off
  5.23 s across three, and **identical compression either way**. What decides
  whether rows land compressed is `mssql_copy_flush_rows`, not the lock.

### COPY TO Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `CREATE_TABLE` | BOOLEAN | `true` | Create the target if it does not exist |
| `REPLACE` | BOOLEAN | `false` | Drop and recreate the target, discarding its definition |
| `TRUNCATE` | BOOLEAN | `false` | Empty an existing target, **keeping** its definition |
| `TABLOCK` | BOOLEAN | from setting | Table-level lock for the load |
| `FLUSH_ROWS` | BIGINT | from setting | Rows buffered before each flush |
| `STRING_LENGTH` | BIGINT | from setting | Length for unannotated `VARCHAR` columns this COPY creates (0 = MAX) |
| `TABLE_KIND` | VARCHAR | from setting | `HEAP` or `COLUMNSTORE` for a table this COPY creates |

```sql
-- Reload a table without losing its indexes, permissions or partitioning
COPY data TO 'sqlserver.dbo.facts' (FORMAT 'bcp', TRUNCATE true);

-- Create a sized columnstore target in one statement
COPY data TO 'sqlserver.dbo.facts' (FORMAT 'bcp', TABLE_KIND 'columnstore', STRING_LENGTH 200);
```

`TRUNCATE` and `REPLACE` both destroy rows the statement does not name, and they
are not interchangeable: `REPLACE` also discards the table's definition, while
`TRUNCATE` keeps it. `TRUNCATE` needs `ALTER` on the table and is refused by SQL
Server when a foreign key references it. It runs on the same connection as the
load, so an aborted COPY cannot leave the table both empty and unloaded.

#### Temp tables need a transaction

A `#temp` table belongs to the **connection** that created it, and SQL Server
drops it when that connection is released. Outside a transaction each statement
takes whatever connection the pool hands out, so a COPY into a `#temp` and a
later read of it are not guaranteed to see the same table — and usually will
not. A DuckDB transaction pins one connection for its whole extent:

```sql
BEGIN TRANSACTION;
COPY (SELECT * FROM staging) TO 'sqlserver..#batch' (FORMAT 'bcp');
CREATE TABLE result AS SELECT * FROM mssql_scan('sqlserver', 'SELECT * FROM #batch');
COMMIT;
```

`##global` temp tables need the same transaction, and are **not** a way around
it. A global temp table lives only as long as the session that created it, and
the pool's reset ends that session — so a `##` table created by one pooled
statement is already gone by the next one, on the same connection. Use a
transaction for those too.

#### Reading and writing the same catalog in one transaction

A DuckDB transaction pins **one** SQL Server connection, and that connection
cannot stream a result set and receive a bulk load at the same time. So a COPY
whose source reads from the same attached catalog it writes to fails inside an
explicit transaction:

```sql
BEGIN TRANSACTION;
COPY (SELECT id FROM sqlserver.dbo.Src) TO 'sqlserver.dbo.Dst' (FORMAT 'bcp', CREATE_TABLE false);
-- IO Error: Failed to execute SQL batch: Cannot execute: connection not in
-- Idle state (current: Executing)
```

It fails cleanly: no rows are written, and the catalog is fully usable after
`ROLLBACK`. Either read into a local table first —

```sql
CREATE TABLE staging AS SELECT id FROM sqlserver.dbo.Src;   -- outside, or before the load
BEGIN TRANSACTION;
COPY staging TO 'sqlserver.dbo.Dst' (FORMAT 'bcp', CREATE_TABLE false);
COMMIT;
```

— or drop the explicit transaction, which lets the read and the load take
separate pooled connections. Two different catalogs (two ATTACHes) are also
fine, each having its own pool: only same-catalog read-and-write collides.

**CTAS is not subject to this**, because it does not use the transaction's
connection at all — see below.

#### CTAS is not part of the transaction

A `CREATE TABLE ... AS SELECT` into SQL Server runs entirely outside any
explicit DuckDB transaction: its `CREATE TABLE` is DDL that autocommits, and its
rows are loaded on connections of their own, taken from the pool. So a
`ROLLBACK` undoes neither:

```sql
BEGIN;
CREATE TABLE sqlserver.dbo.Dst AS SELECT * FROM sqlserver.dbo.Src;
ROLLBACK;
-- Dst exists, with every row in it.
```

This is deliberate. Putting the load on the transaction's pinned connection is
the only way to make `ROLLBACK` discard the rows, and it costs more than it
buys: one connection cannot stream a result set and receive a bulk load at the
same time, so the statement above — reading from the same catalog it writes to —
could not run at all, and no CTAS inside a transaction could use more than one
writer. The table itself survived a rollback either way, since the `CREATE` had
already autocommitted; the choice was only ever between an empty table and a
full one.

Undoing a table the statement itself created means dropping it, which is
complete and needs no shared transaction. That is what
`mssql_ctas_drop_on_failure` does when the load fails partway. Drop it by hand
otherwise:

```sql
SELECT mssql_exec('sqlserver', 'DROP TABLE dbo.Dst');
SELECT mssql_invalidate_cache('sqlserver');   -- the catalog cache does not see it go
```

### Performance Characteristics

- **Protocol**: Uses TDS BulkLoadBCP (packet type 0x07) for maximum throughput
- **Streaming**: Bounded memory usage regardless of dataset size
- **Throughput**: ~300K rows/s for simple rows, ~10K rows/s for wide rows (500+ chars × 10 columns)
- **TABLOCK**: Enables table-level locking and minimal logging for faster inserts
- **UTF-8 targets**: a `varchar` column under a UTF-8 collation receives UTF-8 bytes as they are, rather than being transcoded to UTF-16 for the server to transcode back. Measured at half the wire bytes for ASCII-ish data (35 → 19 bytes for a 16-character value) and −51% client CPU on long strings. An `nvarchar` target is unaffected. This is the whole of the string path on Microsoft Fabric, where every string column is a UTF-8 `varchar`

### COPY TO Behavior

- **Auto-create**: Tables are created automatically with inferred schema (can be disabled)
- **Type mapping**: DuckDB types mapped to SQL Server equivalents (VARCHAR→NVARCHAR, etc.)
- **No RETURNING**: Use INSERT for cases requiring returned values
- **Transaction support**: Works within transactions; temp tables require transaction context

### Column Mapping (Existing Tables)

When copying to an existing table with `CREATE_TABLE false`, columns are matched **by name** (case-insensitive), not by position:

```sql
-- Target table has columns: id INT, name VARCHAR(50), value FLOAT

-- Source can have different column order
CREATE TABLE source AS SELECT 1.5::DOUBLE AS value, 1 AS id;

-- Copies successfully: id→id, value→value, name→NULL
COPY source TO 'mssql://db/dbo/target' (FORMAT 'bcp', CREATE_TABLE false);
```

**Column Mapping Rules:**

| Scenario | Behavior |
|----------|----------|
| Same columns, same order | Direct mapping (backward compatible) |
| Same columns, different order | Mapped by name |
| Source has fewer columns | Missing target columns receive NULL |
| Source has extra columns | Extra columns are ignored |
| No matching columns | Error: "No matching columns" |
| Case mismatch (id vs ID) | Matched case-insensitively |

> **Note**: Target columns that don't have matching source columns must allow NULL values.

