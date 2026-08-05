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
  See [Reading and writing the same catalog in one transaction](/writing/copy/#reading-and-writing-the-same-catalog-in-one-transaction)
- **CTAS is outside the transaction**: it creates its table with autocommitting
  DDL and loads on connections of its own, so `ROLLBACK` undoes neither. See
  [CTAS is not part of the transaction](/writing/copy/#ctas-is-not-part-of-the-transaction)

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

