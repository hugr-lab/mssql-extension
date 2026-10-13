---
title: Transactions
sidebar_position: 6
---

# Transactions

The extension supports DuckDB transactions mapped to SQL Server transactions with connection pinning.

### Basic Transaction Usage

```sql
BEGIN;
INSERT INTO sqlserver.dbo.orders (customer_id, amount) VALUES (1, 99.99);
UPDATE sqlserver.dbo.customers SET order_count = order_count + 1 WHERE id = 1;
COMMIT;
```

All statements within a transaction execute on the same SQL Server connection. If any statement fails, use `ROLLBACK` to undo changes.

### Transaction Behavior

- **Autocommit (default)**: each statement is independent, with its own implicit transaction. An `UPDATE`, `DELETE` or `MERGE` on an attached catalog runs on it as a **transaction of one statement**: one pinned connection, committed or rolled back when DuckDB commits or rolls back the statement, as postgres_scanner and mysql_scanner do for every DuckDB transaction (issue #451). An error anywhere in the statement, Ctrl-C, or a result dropped before its query ended (DuckDB aborts it) undoes the write. See [Autocommit writes follow DuckDB](#autocommit-writes-follow-duckdb)
- **Explicit transactions**: `BEGIN` pins a connection; all subsequent operations reuse it until `COMMIT` or `ROLLBACK`
- **Isolation level**: SQL Server default (READ COMMITTED). Use `mssql_exec()` to change if needed
- **Connection reset**: After commit/rollback, the connection's session state is reset via TDS RESET_CONNECTION flag before pool reuse
- **One connection, one job**: because a transaction pins a single connection, it
  cannot stream a result set and receive a bulk load at once — a `COPY` that
  reads from the same catalog it writes to fails inside an explicit transaction.
  See [Reading and writing the same catalog in one transaction](./copy.md#reading-and-writing-the-same-catalog-in-one-transaction)
- **CTAS is outside the transaction**: it creates its table with autocommitting
  DDL and loads on connections of its own, so `ROLLBACK` undoes neither. See
  [CTAS is not part of the transaction](./copy.md#ctas-is-not-part-of-the-transaction)

- **Reads of the same catalog in one transaction are buffered.** The pinned
  connection can carry one result stream at a time, and DuckDB does not promise
  to finish reading one source before it starts the next — a correlated subquery
  in a predicate plans as a delim join and opens both. So inside an explicit
  transaction, a scan is read to completion as it starts and the connection is
  freed before anything else runs. See
  [Reading a catalog inside a transaction](#reading-a-catalog-inside-a-transaction)
  below.

### Autocommit writes follow DuckDB

Outside `BEGIN … COMMIT` every statement still runs in a transaction of its own
-- DuckDB's autocommit transaction -- and a statement that writes to an attached
catalog makes the SQL Server side part of it:

| Statement | In autocommit |
|---|---|
| `UPDATE`, `DELETE`, `MERGE` (pushed whole or not) | Joins: one pinned connection, `COMMIT` / `ROLLBACK` with DuckDB |
| `INSERT` (statements, bulk, or pushed `INSERT … SELECT`) | Commits on its own when it finishes -- and stays, whatever then happens to its result (a dropped handle, a stream closed early) -- with the bulk path's parallel writers -- unless `mssql_insert_use_bcp = false`, which makes it join, or the same statement has joined the catalog through another write (a DML CTE): then it runs on that connection too |
| `CREATE TABLE … AS SELECT` | Commits as it loads (DDL and rows), with parallel writers -- unless `mssql_ctas_use_bcp = false`, which makes it run whole in the transaction, here and inside `BEGIN … COMMIT` |
| `COPY … TO` (`FORMAT 'bcp'`) | Always commits as it loads: an export, like `COPY` to a file |
| `mssql_exec()` | Autocommits what it runs -- except when called inside a statement that joined its catalog, where it runs on that statement's connection |

So a statement such as

```sql
WITH u AS (UPDATE sqlserver.dbo.t SET v = v + 1 RETURNING id) SELECT error('boom') FROM u;
```

leaves the table unchanged, as it would a DuckDB table. A joined statement behaves
on its catalog as a transaction does: the scans of that catalog it reads are
materialized (read to completion at their start, see below), and it holds its
connection -- and the server's locks -- until DuckDB ends the query. An `INSERT`
that joins loads with one writer.

The two settings are `SET` per DuckDB connection or `SET GLOBAL` for all of them.

### Parallel writers and readers that do not wait {#parallel-writers-visibility}

A bulk-path `INSERT` (past `mssql_insert_bcp_threshold`) may load on several
connections at once (`mssql_copy_parallel_writers`). Each connection holds its own
server transaction until the load finishes, and then they are committed **one after
another**. "Together" means only that a failure before that point rolls all of them
back. It is not one atomic commit.

- **A reader under locking READ COMMITTED** (the SQL Server default) waits on the
  locks of the writers not yet committed. It sees all of the `INSERT` or none of it.
- **A reader that does not wait** sees each writer's rows as soon as that writer has
  committed. That is a database with `READ_COMMITTED_SNAPSHOT` on, a `SNAPSHOT`
  transaction, or `NOLOCK`. Between two commits it can see part of one `INSERT`.
  Measured: a reader counting rows under RCSI saw part of a 20 000-row `INSERT`.

Where such a reader must see each `INSERT` all or nothing, use one writer. Any of
these does it:

- `SET mssql_copy_parallel_writers = 1` — one connection, one commit. Parallel
  writers pay off only on loads of hundreds of thousands of rows.
- Run the `INSERT` inside `BEGIN … COMMIT`. It then runs on the transaction's one
  connection and commits with it.
- `SET mssql_insert_use_bcp = false` — statements instead of a bulk load, joined to
  the statement's DuckDB transaction.

A `COPY` or a `CREATE TABLE … AS SELECT` in autocommit holds no transaction for its
rows at all. Each batch (`mssql_copy_flush_rows` rows) commits as it is sent, so
**every** reader can see the load grow, whatever the number of writers. That is the
"commits as it loads" in the table above. For an all-or-nothing load:

- run the `COPY` inside `BEGIN … COMMIT`;
- or `SET mssql_ctas_use_bcp = false` for a CTAS.

### Reading a catalog inside a transaction

Reading an attached catalog behaves differently inside a transaction than outside
it, and it is worth knowing why.

**Outside a transaction**, every scan takes its own connection from the pool and
streams: rows arrive as they are needed and nothing is held in memory.

**Inside `BEGIN … COMMIT`**, all of them share the one connection the transaction
pinned. That is what makes read-your-writes work — a second connection would sit
outside the transaction and could not see its uncommitted rows, and under READ
COMMITTED it would block on their locks. The cost is that one connection cannot
stream two results at once.

Rather than fail, such scans are **materialized**: each one is read to completion
when it starts, into a buffer-managed collection that spills to disk if it is
large, and the connection is released immediately afterwards.

```sql
BEGIN;

-- Two scans of `sqlserver`: the outer table and the correlated subquery.
-- Both are read fully as they start, then served from memory.
SELECT o.id
FROM sqlserver.dbo.orders o
WHERE o.total > (SELECT avg(x.total) FROM sqlserver.dbo.orders x WHERE x.customer_id = o.customer_id);

COMMIT;
```

What this means in practice:

- **Memory is bounded by DuckDB's buffer manager**, not by the row count — a
  collection too large for memory spills to disk rather than growing in process
  memory.
- **The rule is wider than the failure it prevents.** At planning time DuckDB
  does not say in which order it will drain a plan's sources, so the decision
  cannot be "these two would have overlapped". Any plan holding two or more
  scans of one catalog inside a transaction materializes all of them — including
  a plain hash join between two of that catalog's tables, which would have
  drained one side and then the other and never needed it. Guessing the other
  way costs a hung connection, not a slow query.
- **If you are reading something large inside a transaction and do not need
  read-your-writes, read it outside one.** Autocommit will stream it.

Before this was handled, such a query failed mid-flight with
`Cannot execute: connection not in Idle state` on a single thread, or
`Connection closed while waiting for COLMETADATA` on several.

#### `mssql_scan()` is materialized on every call in a transaction

The rule above counts scans of attached tables, because the planner can see
those. `mssql_scan()` cannot take part in it: it runs its query during **binding**
— that is how it learns the result's column types, long before there is a plan to
count anything in — and so it would be holding the pinned connection before any
gate could ask how many scans there are.

So inside an explicit transaction, **every** `mssql_scan()` is read to completion
at bind time and its connection released, even when it is the only one in the
statement. The rows are still the transaction's own: the drain runs on the pinned
connection, so uncommitted writes are visible as usual.

```sql
BEGIN;
INSERT INTO sqlserver.dbo.orders (id, total) VALUES (1, 10);

-- Sees the uncommitted row: read on the same pinned connection, then buffered.
SELECT count(*) FROM mssql_scan('sqlserver', 'SELECT id FROM dbo.orders');

COMMIT;
```

In autocommit, `mssql_scan()` streams exactly as before — it takes a pooled
connection of its own and nothing is buffered.

### Multi-Statement SQL Batches

`mssql_scan()` supports multi-statement batches where intermediate statements don't return result sets:

```sql
-- Temp table workflow: create, populate, query
FROM mssql_scan('sqlserver', '
    SELECT * INTO #temp FROM dbo.large_table WHERE region = ''US'';
    SELECT * FROM #temp ORDER BY created_at
');
```

**Constraint**: Only one statement in the batch may produce a result set. Batches with multiple SELECTs will return a clear error message.

