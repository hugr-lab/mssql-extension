---
title: INSERT / UPDATE / DELETE
sidebar_position: 4
---

# INSERT

### Basic INSERT

```sql
-- Single row
INSERT INTO sqlserver.dbo.my_table (name, value)
VALUES ('test', 42);

-- Multiple rows
INSERT INTO sqlserver.dbo.my_table (name, value)
VALUES ('first', 1), ('second', 2), ('third', 3);
```

### INSERT from SELECT

```sql
INSERT INTO sqlserver.dbo.target_table (name, value)
SELECT name, value FROM local_source_table;
```

### INSERT with RETURNING

Get inserted values back (uses SQL Server's OUTPUT INSERTED):

```sql
INSERT INTO sqlserver.dbo.my_table (name)
VALUES ('test')
RETURNING id, name;
```

```sql
INSERT INTO sqlserver.dbo.my_table (name, value)
VALUES ('a', 1), ('b', 2)
RETURNING *;
```

### Batch Configuration

Large inserts are automatically batched. Configure batch size:

```sql
-- Set batch size (default: 1000, SQL Server limit)
SET mssql_insert_batch_size = 500;

-- Maximum SQL statement size (default: 8MB)
SET mssql_insert_max_sql_bytes = 4194304;
```

### Identity Columns

Identity (auto-increment) columns are automatically excluded from INSERT statements. The generated values are returned via RETURNING clause.

## UPDATE

UPDATE operations are supported for tables with primary keys. The extension uses rowid-based targeting for efficient updates.

### Basic UPDATE

```sql
-- Update single row
UPDATE sqlserver.dbo.products SET price = 19.99 WHERE id = 1;

-- Update multiple rows
UPDATE sqlserver.dbo.products SET status = 'discontinued' WHERE category = 'legacy';

-- Update with expressions
UPDATE sqlserver.dbo.products SET price = price * 1.10 WHERE category = 'premium';
```

### UPDATE with Multiple Columns

```sql
UPDATE sqlserver.dbo.customers
SET name = 'John Doe', email = 'john@example.com', updated_at = NOW()
WHERE id = 42;
```

### Batch Configuration

Large updates are automatically batched:

```sql
-- Set batch size (default: 500)
SET mssql_dml_batch_size = 500;
```

### Limitations

- **RETURNING clause is not supported** for UPDATE operations
- Tables must have a primary key (uses rowid for row identification)
- Updates use a single `UPDATE ... FROM target JOIN (VALUES ...)` statement per batch, joining on the primary key (scalar or composite)

## String comparisons and collation {#collation}

:::warning A `DELETE` can remove more rows than a DuckDB user expects

`UPDATE` and `DELETE` choose their rows the same way a `SELECT` does: the
`WHERE` clause is pushed to SQL Server, and **SQL Server's collation decides
what matches** — not DuckDB's byte comparison.

On a case-insensitive collation (`_CI_AS`, the default for most installations)
that means:

```sql
-- table dbo.T contains 'abc' and 'ABC'
SELECT count(*) FROM mssql.dbo.T WHERE name = 'abc';   -- 2
DELETE FROM mssql.dbo.T WHERE name = 'abc';            -- deletes BOTH
```

The same statement against a native DuckDB table deletes one row. This is
consistent — the `DELETE` removes exactly the rows the equivalent `SELECT`
returns, and exactly what SSMS would do — but it is not what a reader who
thinks in DuckDB semantics will predict, and on a destructive statement the
difference is not recoverable.
:::

### Why it works this way

Pushing the predicate is what makes the operation fast, and the pushed
predicate is evaluated by the server. `UPDATE`/`DELETE` then target rows by
primary key (see the rowid note in Limitations above), so the rows acted on are
precisely the rows the scan returned — with the server's comparison rules
already applied.

The consequences worth knowing:

| collation | `WHERE name = 'abc'` also matches | so `DELETE` also removes |
|---|---|---|
| `_CI_AS` (case-insensitive) | `'ABC'`, `'Abc'` | those rows |
| `_CI_AI` (accent-insensitive too) | `'ábc'` | those rows |
| `_BIN2` / `_BIN` | nothing extra | nothing extra |

Trailing spaces are their own case: SQL Server pads on comparison, so
`name = 'abc'` matches `'abc '` under **every** collation including `_BIN2`.

### Making a statement collation-exact

**Look before you delete.** The matching statement is the cheapest check there
is, and it is exact — the `SELECT` returns precisely the rows the `DELETE`
will take:

```sql
SELECT * FROM mssql.dbo.T WHERE name = 'abc';   -- these rows, exactly
DELETE FROM mssql.dbo.T WHERE name = 'abc';
```

**To force exact-byte comparison**, write the T-SQL yourself with an explicit
`COLLATE`, via [`mssql_exec()`](/reference/functions):

```sql
SELECT mssql_exec('mssql',
  'DELETE FROM dbo.T WHERE name = N''abc'' COLLATE Latin1_General_BIN2');
```

That is case- and accent-sensitive whatever the column's collation says. Note
it still will not distinguish a trailing space, because SQL Server pads on
comparison under every collation — for that, add a sentinel:
`WHERE name + N'~' = N'abc~'`.

### Planned change

Spec 061 proposes making server-side `UPDATE`/`DELETE` **collation-exact by
default** — emitting the native predicate *and* a forced `COLLATE …_BIN2`
comparison, so the rows modified are the rows DuckDB's own predicate selects.
That is a deliberate divergence from `SELECT`, which keeps native server
semantics: a destructive statement should be the conservative one. Until that
lands, the behaviour on this page is what applies.

## DELETE

DELETE operations are supported for tables with primary keys.

### Basic DELETE

```sql
-- Delete single row
DELETE FROM sqlserver.dbo.products WHERE id = 1;

-- Delete multiple rows
DELETE FROM sqlserver.dbo.products WHERE status = 'discontinued';

-- Delete all rows (use with caution)
DELETE FROM sqlserver.dbo.products;
```

### DELETE with Complex Conditions

```sql
DELETE FROM sqlserver.dbo.order_items
WHERE order_id IN (SELECT id FROM sqlserver.dbo.orders WHERE status = 'cancelled');
```

### Batch Configuration

Large deletes are automatically batched:

```sql
-- Set batch size (default: 500)
SET mssql_dml_batch_size = 500;
```

### Limitations

- **RETURNING clause is not supported** for DELETE operations
- Tables must have a primary key (uses rowid for row identification)

