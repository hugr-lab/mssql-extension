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

- **Autocommit (default)**: Each statement is independent with its own implicit transaction
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

- **Two reads of the same catalog in one transaction are buffered.** The pinned
  connection can carry one result stream at a time, and DuckDB does not promise
  to finish reading one source before it starts the next — a correlated subquery
  in a predicate plans as a delim join and opens both. So inside an explicit
  transaction, a query holding more than one scan of the same catalog reads each
  of them fully into memory as it starts, and frees the connection before moving
  on. See [Two scans of one catalog](#two-scans-of-one-catalog) below.

### Two scans of one catalog

A query that reads the same attached catalog twice behaves differently inside a
transaction than outside it, and it is worth knowing why.

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

-- Two scans of `mssql`: the outer table and the correlated subquery.
-- Both are read fully as they start, then served from memory.
SELECT o.id
FROM mssql.dbo.orders o
WHERE o.total > (SELECT avg(x.total) FROM mssql.dbo.orders x WHERE x.customer_id = o.customer_id);

COMMIT;
```

What this means in practice:

- **Only queries that would otherwise fail are affected.** One scan per catalog
  streams as before, in or out of a transaction. Autocommit is never affected.
- **Memory is bounded by DuckDB's buffer manager**, not by the row count — a
  collection too large for memory spills.
- **If you are reading something large inside a transaction and do not need
  read-your-writes, read it outside one.** Autocommit will stream it.

Before this was handled, such a query failed mid-flight with
`Cannot execute: connection not in Idle state` on a single thread, or
`Connection closed while waiting for COLMETADATA` on several.

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

