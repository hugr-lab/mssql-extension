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
  - strings: `lower`, `upper`, `trim`, `ltrim`, `rtrim`
  - dates: `year`, `month`, `day`, `hour`, `minute`, `second`
  - arithmetic: `+ - * %`, negation
  - substring matching: `prefix`/`suffix`/`contains` and their
    case-insensitive variants translate to `LIKE` (constant patterns) —
    including leading-wildcard forms
- Rowid equality (expands to the primary-key columns)

**Not pushed down** (applied locally by DuckDB): unmapped functions —
`list_contains()`, `regexp_matches()`, and anything else without a T-SQL
mapping. An expression the encoder cannot translate stays in DuckDB; results
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

### ORDER BY Pushdown (Experimental)

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

**The order is always DuckDB's.** A pushed ORDER BY removes DuckDB's own sort,
so a key is pushed only when SQL Server sorts it the way DuckDB would:

- numeric, `bit` and date/time keys;
- of the strings, only `varchar` / `char` under a `_BIN2` UTF-8 collation (such
  as the extension's CTAS default, `Latin1_General_100_BIN2_UTF8`), whose bytes
  order as code points. Under any other collation — including the installation
  default `SQL_Latin1_General_CP1_CI_AS`, which orders linguistically, and a
  code-page `_BIN2`, which orders the code page's bytes — DuckDB sorts. So it
  does for `nvarchar` / `nchar` under any collation: their UTF-16 order puts a
  character above the BMP (an emoji) before U+E000–U+FFFF, DuckDB after. What
  still differs on a qualifying key is trailing spaces: the server treats `ab`
  and `ab ` as equal (a tie, so a TOP N may pick either), and sorts a value
  continuing with a character below the space (`ab` + TAB) before `ab`;
- never a string-valued function of a key (`upper(name)`), a
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
- Only prefix pushdown: stops at first non-pushable column
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

