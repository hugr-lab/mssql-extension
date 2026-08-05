---
title: CREATE TABLE AS SELECT
sidebar_position: 2
---

# CREATE TABLE AS SELECT (CTAS)

Create SQL Server tables directly from DuckDB query results.

### Basic CTAS

```sql
-- Create table from DuckDB query
CREATE TABLE sqlserver.dbo.summary AS
SELECT region, COUNT(*) AS order_count, SUM(amount) AS total
FROM sqlserver.dbo.orders
GROUP BY region;

-- Create from local DuckDB table
CREATE TABLE sqlserver.dbo.imported_data AS
SELECT * FROM read_csv('data.csv');

-- Create from generate_series
CREATE TABLE sqlserver.dbo.sequence AS
SELECT i AS id, 'item_' || i::VARCHAR AS name
FROM generate_series(1, 1000) t(i);
```

### CREATE OR REPLACE

Replace an existing table with new data:

```sql
-- Overwrites existing table (non-atomic: DROP then CREATE)
CREATE OR REPLACE TABLE sqlserver.dbo.daily_report AS
SELECT * FROM sqlserver.dbo.transactions WHERE date = CURRENT_DATE;
```

### Type Mapping

DuckDB types are mapped to SQL Server types as follows. The same mapping is used
by CTAS and by `COPY TO` when it creates the target.

| DuckDB Type | SQL Server Type |
|-------------|-----------------|
| `BOOLEAN` | `BIT` |
| `TINYINT`, `UTINYINT` | `TINYINT` |
| `SMALLINT` | `SMALLINT` |
| `USMALLINT`, `INTEGER` | `INT` |
| `UINTEGER`, `BIGINT` | `BIGINT` |
| `UBIGINT` | `DECIMAL(20,0)` |
| `HUGEINT`, `UHUGEINT` | `DECIMAL(38,0)` |
| `FLOAT` | `REAL` |
| `DOUBLE` | `FLOAT` |
| `DECIMAL(p,s)` | `DECIMAL(p,s)` (p clamped to 38) |
| `VARCHAR` | `NVARCHAR(MAX)` — **configurable**, see below |
| `BLOB` | `VARBINARY(MAX)` |
| `UUID` | `UNIQUEIDENTIFIER` |
| `DATE` | `DATE` |
| `TIME` | `TIME(7)` |
| `TIMESTAMP` | `DATETIME2(6)` — µs, DuckDB's own precision |
| `TIMESTAMP_MS` | `DATETIME2(3)` |
| `TIMESTAMP_NS` | `DATETIME2(7)` — 100 ns, lossy by 2 digits |
| `TIMESTAMP_S` | `DATETIME2(0)` |
| `TIMESTAMP WITH TIME ZONE` | `DATETIMEOFFSET(7)` |
| `INTERVAL` | `NVARCHAR(50)` — canonical DuckDB interval text |

**Unsupported types** (will error with a clear message):

- `LIST`, `STRUCT`, `MAP`, `ARRAY`, `UNION`, `ENUM` — no SQL Server equivalent

The `VARCHAR` row is the only one you can change, and it is the one worth
changing: `nvarchar(max)` is an off-row LOB and measured 4.1× slower to load
than a sized column. See
[Target Column Types and Table Shape](#target-column-types-and-table-shape) for
`MSSQL_VARCHAR(n)` / `MSSQL_NVARCHAR(n)`, `mssql_default_string_length` and
`mssql_ctas_text_type`.

> **Note**: `COPY TO`'s auto-create currently emits `time(6)` where CTAS emits
> `TIME(7)` for a DuckDB `TIME`. Both are lossless — DuckDB's `TIME` is
> microseconds — but the two paths disagree.

### CTAS Settings

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `mssql_ctas_use_bcp` | BOOLEAN | `true` | Use BCP protocol for data transfer (2-10x faster than INSERT) |
| `mssql_ctas_text_type` | VARCHAR | `NVARCHAR` | Text column type: `NVARCHAR` or `VARCHAR`. Also governs COPY |
| `mssql_ctas_drop_on_failure` | BOOLEAN | `false` | Drop table if data transfer phase fails |

Column lengths and the table's shape come from
[Target Column Types and Table Shape](#target-column-types-and-table-shape) —
cast a column to `MSSQL_NVARCHAR(n)` in the SELECT list to size it, since CTAS
has no options syntax of its own.

```sql
-- Disable BCP for legacy INSERT mode (slower, but compatible)
SET mssql_ctas_use_bcp = false;

-- Use VARCHAR instead of NVARCHAR for text columns
SET mssql_ctas_text_type = 'VARCHAR';

-- Auto-cleanup on failure (for production pipelines)
SET mssql_ctas_drop_on_failure = true;
```

### CTAS Behavior

- **BCP mode (default)**: Uses TDS BulkLoadBCP protocol for 2-10x faster data transfer
- **Two-phase execution**: CREATE TABLE DDL, then data transfer via BCP or INSERT
- **Streaming**: Large result sets are streamed without full buffering
- **Not atomic, and not covered by a transaction**: see [What a failed load leaves behind](#what-a-failed-load-leaves-behind)
- **Schema validation**: Target schema must exist before CTAS
- **Legacy INSERT mode**: Set `mssql_ctas_use_bcp = false` to use batched INSERT statements

### What a failed load leaves behind

Neither CTAS nor COPY is atomic. If one fails part-way — a type that will not
convert, a dropped connection, `Ctrl-C` — **what already reached SQL Server stays
there.** Plan for that: check the row count before treating a load as done, and
load into a staging table you can drop when the answer matters.

The reason is the same for both: the bulk-load protocol commits each batch as it
is sent. `mssql_copy_flush_rows` (102400 by default) is exactly that boundary, so
a load of 1M rows that dies at row 700000 leaves six committed batches on the
server, not zero.

Wrapping the load in an explicit transaction bounds the damage:

| | inside `BEGIN … COMMIT` | outside a transaction |
|---|---|---|
| `COPY TO` | rows roll back | partial rows remain; with `CREATE_TABLE true` the table remains too |
| `CREATE TABLE AS` | rows roll back; **the empty table remains** | partial rows and the table remain |

Both run their bulk load on the connection pinned to the transaction — one
session, one `INSERT BULK`, parallel writers disabled (a second connection would
sit outside the transaction, and its rows would not roll back with the rest).

What a `ROLLBACK` does **not** undo is the `CREATE TABLE` itself: CTAS runs its
DDL as its own statement before any row is sent, so an aborted CTAS leaves an
empty table of the right shape behind. Drop it, or have the extension do it:

```sql
-- Drop a half-written table when the load fails (default: false)
SET mssql_ctas_drop_on_failure = true;
```

That setting covers the failure the extension sees. It cannot cover a killed
process or a lost connection — there, SQL Server rolls back the batch in flight
and keeps the ones already committed, and the table stays.

When the target already exists, `COPY` gives the stronger guarantee, because
there is no DDL to leave behind:

```sql
BEGIN TRANSACTION;
COPY src TO 'mssql.dbo.target' (FORMAT bcp, CREATE_TABLE false);
COMMIT;   -- ROLLBACK instead leaves dbo.target exactly as it was
```

