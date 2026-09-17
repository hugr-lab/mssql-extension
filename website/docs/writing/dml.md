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

### Two paths: statements and BCP

An INSERT reaches SQL Server one of two ways, and the extension picks per
statement:

| the INSERT | how it is sent |
| --- | --- |
| more rows than `mssql_insert_bcp_threshold` (default 1000), no `RETURNING`, no explicit identity column | the BCP protocol (`INSERT BULK`), the same wire COPY and CTAS use — measured 1M rows in 1.8 s where the statements took 74 s |
| anything else — `RETURNING`, a named identity column, `mssql_insert_use_bcp = false`, or up to the threshold rows | batched `INSERT … VALUES` statements |

The threshold is decided by counting the rows as they arrive, not by a plan
estimate, so an `INSERT … SELECT` behind a filter goes the right way too.

**Every INSERT is atomic**, on either path. In autocommit the statement runs
on one connection inside one server transaction — `BEGIN TRANSACTION` before
its first batch, `COMMIT` after the last — so a failure anywhere leaves the
table exactly as it was, and the error says so:

```text
INSERT via BCP failed at batch 5 of a writer (1808 row(s) in it): MSSQL: BCP failed:
The INSERT statement conflicted with the CHECK constraint ...; rolled back, the
8192 row(s) from the batches before it included
```

Inside a `BEGIN … COMMIT` block the rows sit in the open transaction until
its `COMMIT` or `ROLLBACK`, and the message says that instead. The same
holds for `UPDATE` and `DELETE`.

**The bulk path behaves like an INSERT statement.** A raw bulk load ignores
CHECK constraints, does not fire triggers and writes a column's DEFAULT
where the data says NULL — that is `COPY`'s contract. An INSERT that goes
through BCP sends `INSERT BULK` with `CHECK_CONSTRAINTS`, `FIRE_TRIGGERS` and
`KEEP_NULLS`, so constraints are checked, triggers fire and an explicit NULL
stays NULL.

**Batch size and parallel writers** come from the bulk-load settings, which
describe the load rather than the statement that started it:
`mssql_copy_flush_rows` (the batch boundary the server sees; 102 400 lands
compressed rowgroups on a columnstore) and `mssql_copy_parallel_writers`.
Parallel writers apply only where their loads cannot block each other —
each writer holds its own server transaction until the INSERT commits them
all, so two writers whose locks conflict would wait on each other with
nothing to time out. That is a target with **no nonclustered index** on it:
a heap under TABLOCK, or a clustered columnstore without it. Under
`mssql_copy_tablock = auto` (heap on, clustered off) bare heaps and bare
columnstores fan out, while a table with a clustered rowstore index — or one
carrying so much as a single nonclustered index, which costs a heap a `Sch-M`
lock instead of the compatible `BU` one and costs a columnstore the load
itself — loads on one writer.
Inside a transaction it is always one writer, the transaction's own
connection. With several writers there is a
window between the first and the last `COMMIT` in which a failed commit
leaves the earlier writers' rows in place; one writer has none.

### Batch Configuration

Statements carry at most 1000 constants each — SQL Server auto-parameterises
a multi-row `VALUES` INSERT up to that line, so distinct inserts of one shape
share a single cached plan; past it every statement compiles its own — and
`mssql_insert_batch_size` caps the rows on top of that:

```sql
-- Rows per INSERT statement (default 1000; 1000 / columns applies first)
SET mssql_insert_batch_size = 500;

-- Maximum SQL statement size (default: 8MB)
SET mssql_insert_max_sql_bytes = 4194304;

-- Rows up to which an INSERT is sent as statements (default 1000)
SET mssql_insert_bcp_threshold = 5000;

-- Statements only, whatever the size
SET mssql_insert_use_bcp = false;
```

### Identity Columns

A column the INSERT does not name is left out of the statement or the bulk
column list, so the server generates its identity value or applies its
DEFAULT. The generated values are returned via the RETURNING clause.

**Naming the identity column works too.** An INSERT that supplies a value for
it — explicitly, or positionally with a value for every column — is bracketed
for you with `SET IDENTITY_INSERT … ON` / `OFF` on the statement's own
connection, so the values land verbatim and the session is clean afterwards.
Two things to know:

- **It stays on the statement path**, whatever `mssql_insert_bcp_threshold`
  says: tens of rows, not millions. Loading many rows *with* their identity
  values is `COPY`'s job — it takes its column list from the source by name,
  keeps a supplied identity value and lets the server assign when the column is
  omitted, and it is the fast path (1M rows in 0.51 s against 74.71 s through
  statements).
- `SET IDENTITY_INSERT` needs **ALTER** permission on the table, which INSERT
  does not; without it the statement fails with a message that says so (the
  server's own text for error 1088 claims the table may not exist). A NULL —
  or `DEFAULT`, which reaches the extension as the same NULL — in a named
  identity column is refused before anything is sent: leave the column out of
  the list to let the server assign, or supply a value for every row; a
  statement mixing the two has to be split.

An INSERT with **no column list** against a table with an identity column is
refused by DuckDB's binder (`has N columns but N-1 values were supplied`) before
the extension sees it; name the columns, as SQL Server's own error 8101 asks
for the same statement. Issue #327 records why the extension cannot widen that
check.

## UPDATE

UPDATE operations are supported for tables with a primary key or a usable unique index. The extension uses rowid-based targeting for efficient updates.

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
- Tables must have a **rowid key**: a primary key, or a unique index that is not filtered, not disabled, and whose key columns are all NOT NULL (a `BIGINT IDENTITY … UNIQUE` is the common shape). A primary key that cannot address a row — `DATETIME`, `SQL_VARIANT` — falls through to another unique index if there is one, and the refusal otherwise names every index it rejected and why
- Updates use a single `UPDATE ... FROM target JOIN (VALUES ...)` statement per batch, joining on the rowid key (scalar or composite)

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
their rowid key (see the rowid note in Limitations above), so the rows acted on are
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
`COLLATE`, via [`mssql_exec()`](../reference/functions.md):

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

DELETE operations are supported for tables with a primary key or a usable unique index.

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
- Tables must have a rowid key — a primary key or a usable unique index, as for UPDATE above

