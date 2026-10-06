---
title: Query Execution
sidebar_position: 2
---

# Query Execution

### Streaming SELECT

Results are streamed directly into DuckDB without buffering the entire result set:

```sql
SELECT * FROM sqlserver.dbo.large_table;
```

### Filter and Projection Pushdown

The extension pushes filters and column selections to SQL Server:

```sql
-- Only 'id' and 'name' columns are fetched, filter applied server-side
SELECT id, name FROM sqlserver.dbo.customers WHERE status = 'active';
```

Supported filter operations for pushdown:

- Equality and comparisons: `=`, `>`, `<`, `>=`, `<=`, `<>`
- IN clause: `column IN (val1, val2, ...)`
- NULL checks: `IS NULL`, `IS NOT NULL`
- Conjunctions: `AND`, `OR`; `CASE`, `BETWEEN`
- Date/timestamp comparisons: `date_col >= '2024-01-01'`
- Boolean comparisons: `is_active = true` (converted to `= 1`)
- **Mapped functions** inside predicates:
  - dates: `year`, `month`, `day`, `hour`, `minute`, `second` — not over a
    `datetimeoffset`, whose date parts the server takes in the value's own
    offset and DuckDB in the session time zone
  - arithmetic: `+ - * %`, negation — over **exact numeric** operands whose
    result both sides compute identically. Left to DuckDB otherwise, so the
    answer is the one you would get without an attached server: a non-numeric
    operand (`date_col + 1` is a date in DuckDB, error 206 in T-SQL); `+ - *`
    on a `float`/`real` (SQL Server has no infinity, so it raises where DuckDB
    returns `inf`); a `decimal` product DuckDB types as `DECIMAL(18)` and
    overflows before the server would; a `decimal` result whose server
    precision passes 38 digits (the server rounds the scale, DuckDB keeps it
    exact); and `%` under `SET error_on_division_by_zero = false` (DuckDB
    answers `NULL` for `x % 0`, the server raises 8134)
  - substring matching: a simple `LIKE` or `GLOB` (which DuckDB rewrites into
    `prefix`/`suffix`/`contains`) translates to T-SQL `LIKE` with a constant
    pattern, including leading-wildcard forms. SQL Server evaluates it under
    the **column's collation**, exactly as it does `=`: on a case-insensitive
    collation (the installation default `SQL_Latin1_General_CP1_CI_AS`)
    `name LIKE '%abc%'` also returns `'ABC'`, where DuckDB alone would not.
    `ILIKE` is not pushed: the server's `LOWER` does not fold case as DuckDB's
    `lower` does (`LOWER(N'ẞ')` stays `ẞ`), so DuckDB evaluates it
- Rowid equality (expands to the primary-key columns)

**Not pushed down** (applied locally by DuckDB): unmapped functions —
`list_contains()`, `regexp_matches()`, and anything else without a T-SQL
mapping. That includes the string functions `lower`, `upper`, `trim`, `ltrim`,
`rtrim` and `length`: SQL Server's versions do not return DuckDB's results
(`UPPER('ß')` stays `ß`, DuckDB's `upper` gives `ẞ`), and a pushed
`upper(name) = 'STRAẞE'` would have lost the row. An expression the encoder cannot translate stays in DuckDB; results
are unchanged either way.

Some functions are unmapped **on purpose**, because the T-SQL form would
return different rows than DuckDB does (issue #242):

- `length`/`len` — `LEN` drops trailing spaces and counts UTF-16 code units;
  `length()` counts code points including them.
- `/` — SQL Server does integer division on integer operands (`5/2 = 2`);
  DuckDB's `/` is always floating (`5/2 = 2.5`).
- `date_diff`, `date_add`, `date_part` — their date-part argument is a T-SQL
  keyword, not a string literal. Write `year(col)` instead; DuckDB rewrites
  `date_part('year', col)` to it anyway, and that form does push.
- `dayofweek`, `week` — `@@DATEFIRST`-dependent / non-ISO week numbering.
- `%` on a `FLOAT`/`REAL` column — the operator itself maps, but T-SQL's
  modulo rejects approximate-numeric operands.

> Pushed string predicates follow the **server's comparison semantics**: on a
> case-insensitive collation, `WHERE name = 'abc'` matches `'ABC'` — exactly
> what the same query returns in SSMS. See the collation notes under
> [Target Column Types](../writing/table-options.md).
>
> **This applies to `UPDATE` and `DELETE` too**, where it is not merely a
> different answer but a different set of rows modified — see
> [String comparisons and collation](../writing/dml.md#collation).

### Remote Pushdown (whole statements)

A SELECT that reads only one attached SQL Server database and has something
for the server to do — a join, an aggregate, `DISTINCT`, `ORDER BY` or `LIMIT`
— is sent to the server **as one T-SQL statement**: the server joins,
aggregates, sorts and filters, and only the result crosses the wire. A plain
filter-and-columns SELECT stays on the table scan, which already sends its
`WHERE`. On by default (`mssql_remote_pushdown`), read when the database is
ATTACHed. `EXPLAIN` shows the T-SQL that is sent:

```sql
EXPLAIN SELECT c.region, count(*) AS orders, sum(o.total) AS revenue
FROM sqlserver.dbo.orders o JOIN sqlserver.dbo.customers c ON o.customer_id = c.id
WHERE o.placed >= DATE '2026-01-01'
GROUP BY c.region ORDER BY revenue DESC LIMIT 10;
-- Mssql Scan Params: SELECT TOP (10) [r2].[region] AS [region], COUNT_BIG(*) AS [orders], ...
```

What goes to the server:

- joins (`INNER` / `LEFT` / `RIGHT` / `FULL` / `CROSS`, `USING`, SEMI / ANTI),
  `WHERE`, `GROUP BY`, `HAVING`, `DISTINCT`, `ORDER BY`, `LIMIT` / `OFFSET`;
- aggregates: `count`, `sum`, `avg`, `min` / `max` (not of strings), `stddev`
  / `variance`, `string_agg`, and `FILTER (WHERE …)` on any of them;
- window functions: `row_number`, `rank`, `dense_rank`, `ntile`, `lag` /
  `lead`, `first_value` / `last_value`, aggregates `OVER (…)` with `ROWS`
  frames; and `QUALIFY`;
- subqueries in `FROM`, `IN` / `EXISTS` / scalar subqueries (correlated too),
  `WITH` (CTEs), and set operations nested in a subquery or a CTE (at a
  statement's top level each side is pushed and DuckDB combines them).

Anything without an exact T-SQL equivalent is left to DuckDB, and a
statement that cannot go whole is split: each part that can is pushed on its
own, and DuckDB does the rest. An `ORDER BY` the server cannot reproduce
exactly — a string key without a `LIMIT`, a nullable key without a `LIMIT` —
keeps the whole statement in DuckDB, aggregate included; `EXPLAIN` shows which
path a statement took. The rows and the column types are DuckDB's either way,
with the exceptions below.

:::warning String equality is the server's — including GROUP BY, DISTINCT and joins

A pushed statement compares strings under the **column's collation**. For
`WHERE` this is nothing new (filter pushdown already sends it). But **GROUP
BY, DISTINCT, `count(DISTINCT …)`, join keys, `PARTITION BY` and a nested
`UNION`'s deduplication** used to run in DuckDB and now run on the server. On a
case-insensitive collation — `SQL_Latin1_General_CP1_CI_AS` is SQL Server's
installation default — `'a'` and `'A'` become **one** group, one distinct
value, one partition:

```sql
-- legacy holds 'a', 'A', 'b', 'B' in a _CI_ collation
SELECT count(DISTINCT legacy) FROM sqlserver.dbo.t;   -- 2 pushed, 4 in DuckDB
```

Orders stay DuckDB's: a string `ORDER BY` is pushed only for a bounded UTF-8
`varchar`, compared as bytes, and only under a `LIMIT` (a window's `ORDER BY`
takes such a key without one); `min` / `max` of a string is never pushed.

Which path a statement takes can depend on table sizes
(`mssql_pushdown_join_rows_threshold`, `mssql_pushdown_min_rows`), and so can
these answers. To keep DuckDB's byte equality, attach with pushdown off —
`ATTACH '…' AS db (TYPE mssql, remote_pushdown false)`, or
`SET mssql_remote_pushdown = false;` before `ATTACH` for every database — or
attach the same database a second time with it off for the statements that
need it.

Two more things change for a database attached with pushdown on: its schema
`main`, when the server has no schema of that name, stands for the default
schema — for reads **and DDL** (`DROP TABLE db.main.x` drops `dbo.x`) — and
DuckDB skips its check for a name that could be either a catalog or a schema
of it. A real schema called `main` on the server is used as it is.
:::

The other differences: a **division by zero** in a computed column is `NULL`
on the server where DuckDB returns `inf` / `NaN` (SQL Server's float has no
infinity), and a floating-point `sum` / `avg` / `stddev` / `variance` can
differ in its last bits (the two add in different orders). Neither is ever
pushed inside a condition, an `ORDER BY` or a `DISTINCT`.

Not pushed (they run in DuckDB): `SELECT * EXCLUDE / REPLACE / COLUMNS(…)`,
`GROUP BY ALL` / `ROLLUP` / `CUBE`, `IGNORE NULLS`, `percent_rank` /
`cume_dist` / `nth_value`, `RANGE` frames with offsets, recursive CTEs,
`$n` parameters, statements that also read a local table or another catalog
(their single-catalog parts still go). To run such a statement on the server,
write the T-SQL yourself with [`mssql_scan`](../reference/functions.md):

```sql
SELECT * FROM mssql_scan('sqlserver', 'SELECT id, name FROM dbo.customers WHERE region = ''EU''');
```

**Cost.** Planning a pushed statement asks the server for its result shape
(`sp_describe_first_result_set`, one round trip). The shape is cached per
statement form — constants are sent as parameters
(`mssql_scan_parameterize_filters`, on by default), so `WHERE id = 1` and
`WHERE id = 2` share it — until the catalog's metadata is invalidated
(`mssql_invalidate_cache()`, DDL through the catalog, `mssql_refresh_cache()`,
`mssql_preload_catalog()`, or `mssql_catalog_cache_ttl`). Inside an explicit
transaction the cached shapes are used too, as long as the transaction has
not changed a schema itself; pushed statements there run on the
transaction's connection.

If a table changes **outside this catalog** — another client, a migration,
SSMS, or `mssql_exec` with `mssql_exec_invalidate_cache = false` — the next
pushed statement over it fails once with *"the statement's result shape
changed since it was cached … Run the statement again"*: running it again
describes it anew. A column whose type the statement takes from the catalog
stays wrong until the catalog is invalidated, as for the table scan; call
`mssql_invalidate_cache()` after such DDL (or set a
`mssql_catalog_cache_ttl`).

On very small tables a pushed aggregate can be a few milliseconds slower than
reading the rows; `mssql_pushdown_min_rows` sets a floor below which
statements are left to the table scan.

See [Remote Pushdown Settings](../reference/settings.md#remote-pushdown-settings).

### ORDER BY Pushdown (Experimental)

This setting governs the **table-scan** path only; a statement taken by [remote pushdown](#remote-pushdown-whole-statements) sends its `ORDER BY` / `TOP` regardless of it.

When enabled, ORDER BY clauses on simple column references and supported functions are pushed to SQL Server, avoiding a local sort in DuckDB. Combined ORDER BY + LIMIT is pushed as `SELECT TOP N ... ORDER BY ...`.

This feature is **disabled by default** and must be explicitly enabled:

```sql
-- Enable globally
SET mssql_order_pushdown = true;

-- Or per-database via ATTACH option
ATTACH 'Server=...' AS db (TYPE mssql, order_pushdown true);
```

**Setting precedence:** The global setting is checked first; if `true`, pushdown is enabled. The ATTACH option is checked second; `true` enables pushdown, `false` is a no-op (does not override global `true`).

**Supported expressions:**
- Simple column references: `ORDER BY id DESC`, `ORDER BY created_at`
- Single-argument functions with a non-string result: `ORDER BY year(date_col)`
- Multi-column: `ORDER BY region_id ASC, created_at DESC`
- Combined with LIMIT: `ORDER BY id ASC LIMIT 10` on a `NOT NULL` key →
  `SELECT TOP 10 ... ORDER BY [id] ASC`

**Which keys.** A pushed ORDER BY removes DuckDB's own sort, so a key is pushed
only when SQL Server sorts it the way DuckDB would:

- numeric, `bit` and date/time keys — except `datetime2(7)`, the default
  `datetime2`, which DuckDB reads as nanosecond timestamps and whose values
  outside 1677–2262 (the `9999-12-31` end of a temporal table's period) it
  reads as NULL;
- a bounded `varchar(n)` under a **UTF-8** collation (any: `_BIN2_UTF8`, such
  as the extension's CTAS default, or a case-insensitive `_UTF8` one), and only
  **under a LIMIT**: it is ordered by its bytes, `CAST(col AS varbinary(n))`,
  which is DuckDB's order. Not `char(n)`, which is stored blank-padded and read
  trimmed, and not `varchar(max)`. The one remaining difference is a trailing NUL
  character, which the server's comparison ignores (`ab` and `ab` + NUL tie).
  Ordered as text the server would pad with spaces — `ab` equal to `ab `, and
  `ab` + TAB before `ab` — hence the bytes; and since a key over bytes cannot
  use an index, a plain ORDER BY on the column is left to DuckDB;
- never a code-page `varchar` (its bytes are the code page's), a `char(n)`, a
  `varchar(max)`, an `nvarchar` /
  `nchar` under any collation (UTF-16 order puts a character above the BMP, an
  emoji, before U+E000–U+FFFF; DuckDB after), a string-valued function of a key
  (`upper(name)`), a date part of a `datetimeoffset` (the server takes it in
  the value's own offset, DuckDB in the session time zone), a
  `uniqueidentifier` (SQL Server compares its last six bytes first),
  `binary` / `varbinary` (compared zero-padded: `0x01` = `0x0100`), `json` or
  `sql_variant`.

**NULL placement.** SQL Server sorts NULL lowest (ASC → first, DESC → last) and
has no `NULLS FIRST` / `LAST`. When a nullable key asks for the other placement —
including DuckDB's default `NULLS LAST` on an ascending key — an ORDER BY with a
LIMIT is pushed with a leading `CASE WHEN key IS NULL THEN … END` key
(`ORDER BY id LIMIT 10` on a nullable `id` → `SELECT TOP 10 ... ORDER BY CASE
WHEN [id] IS NULL THEN 1 ELSE 0 END, [id] ASC`), and one without a LIMIT is left
to DuckDB: the leading key defeats any index, so the server would sort the
whole table instead. Asking for the server's own placement (`ASC NULLS FIRST`,
`DESC NULLS LAST`) pushes either way.

**Filters.** A TOP N is pushed only when every filter of the scan is on the
server too. A filter the extension cannot translate runs on the rows that come
back, and would otherwise be applied after the server had already cut them to N.

**Limitations:**
- Only prefix pushdown: stops at first non-pushable column, and under a LIMIT
  nothing is pushed unless every key is (the TOP N needs them all)
- Expressions like `ORDER BY col * 2` are not pushed

### Row Identity (rowid)

Tables with primary keys expose a virtual `rowid` column that provides stable row identification:

```sql
-- Query rowid alongside other columns
SELECT rowid, name, value FROM sqlserver.dbo.products LIMIT 5;
```

**rowid Type Mapping:**

| Primary Key Type | rowid Type | Example |
|------------------|------------|---------|
| Single column (INT) | `INTEGER` | `42` |
| Single column (BIGINT) | `BIGINT` | `9223372036854775807` |
| Single column (VARCHAR) | `VARCHAR` | `'ABC-001'` |
| Single column (UNIQUEIDENTIFIER) | `UUID` | `a1b2c3d4-e5f6-...` |
| Composite (multiple columns) | `STRUCT` | `{'region_id': 1, 'product_id': 100}` |

**Usage Examples:**

```sql
-- Scalar primary key (INT)
SELECT rowid, name FROM sqlserver.dbo.customers;
-- rowid: 1, 2, 3, ...

-- Composite primary key (VARCHAR + INT)
SELECT rowid, quantity FROM sqlserver.dbo.order_items;
-- rowid: {'tenant_code': 'ACME', 'item_id': 1}, ...

-- Filter using rowid (composite key)
SELECT * FROM sqlserver.dbo.order_items
WHERE rowid = {'tenant_code': 'ACME', 'item_id': 1};
```

**Limitations:**

- Tables without primary keys do not expose `rowid`
- Views do not support `rowid`
- `rowid` is read-only (cannot be used in INSERT/UPDATE)

