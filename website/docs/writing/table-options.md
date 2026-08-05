---
title: Target Types & Table Shape
sidebar_position: 3
---

# Target Column Types and Table Shape

Every path that creates a SQL Server table — `CREATE TABLE`, `CREATE TABLE AS
SELECT`, `COPY TO` — used to give string columns `nvarchar(max)`. That is a poor
default for a table you are about to load: it measured **4.1× slower** to load
than a sized target, and it is stored as an off-row LOB.

Nothing below changes unless you ask for it: with no cast, no option and no
setting, the DDL is byte-identical to previous releases.

### Stating a column's type

`MSSQL_NVARCHAR(n)` and `MSSQL_VARCHAR(n [, collation])` state the type a target
column should get. They bind to a plain DuckDB `VARCHAR`, so every string
function keeps working on them.

```sql
CREATE TABLE mssql_db.dbo.customers (
    id       INTEGER,
    email    MSSQL_VARCHAR(320),      -- varchar(320), UTF-8 collated
    name     MSSQL_NVARCHAR(100)      -- nvarchar(100)
);

-- The same types state a target for CTAS and COPY, which have no DDL of their own
CREATE TABLE mssql_db.dbo.report AS
    SELECT id, upper(name)::MSSQL_NVARCHAR(100) AS name FROM local_table;
```

`n` is **SQL Server's own unit** for the type named: UTF-16 code units for
`MSSQL_NVARCHAR`, **bytes** for `MSSQL_VARCHAR`. `MSSQL_VARCHAR(50)` is exactly
`varchar(50)`, so the same `n` in the two types does not hold the same data —
50 bytes of UTF-8 is about 16 Cyrillic characters.

#### Naming a collation

`MSSQL_VARCHAR` takes an optional second argument: the collation for that one
column. Without it the column gets `mssql_utf8_collation`.

```sql
CREATE TABLE mssql_db.dbo.customers (
    email  MSSQL_VARCHAR(320),                                        -- session default
    city   MSSQL_VARCHAR(100, 'Cyrillic_General_100_CI_AS_SC_UTF8'),  -- this column only
    code   MSSQL_VARCHAR(16,  'Latin1_General_100_BIN2_UTF8')         -- binary ordering
);

-- Works the same in a cast, so CTAS and COPY can name it too
COPY (SELECT city::MSSQL_VARCHAR(100, 'Cyrillic_General_100_CI_AS_SC_UTF8') AS city FROM staging)
    TO 'mssql://mssql_db/dbo/cities' (FORMAT 'bcp');
```

The collation is stored in the schema and governs every later comparison
against the column, which is why it is worth naming deliberately rather than
inheriting: `_CI_AS` is case-insensitive and accent-sensitive, `_BIN2` orders by
code point. Pick the one your data is actually compared with.

A **non-UTF-8 collation is accepted**, and it means what it says: the column
stores that collation's code page, and SQL Server replaces every character
outside it with `?` on insert — no error, and nothing downstream can tell,
because `?` is valid UTF-8. That is a real choice with a real cost, worth making
when you are reproducing an existing schema or know the data is inside the page;
it is the same thing SQL Server does for any client. What the extension will not
do is make that choice *for* you, which is what issue #225 was about — omit the
argument and you get `mssql_utf8_collation`, never the database's code page.

`MSSQL_NVARCHAR` takes no collation argument at all: it stores UTF-16 and has no
code page to get wrong.

Only letters, digits and underscores are accepted; the name reaches T-SQL as a
bare identifier, so anything else is rejected at bind time rather than
concatenated into a `CREATE TABLE`.

On Microsoft Fabric only two collations exist and both are UTF-8; naming any
other is refused before the DDL is built. See [AZURE.md](/connection/azure/#two-collations-both-utf-8).

#### Using them with COPY

`COPY TO` takes its column types from the query it is given, so the cast goes in
the SELECT list — the same place it goes for CTAS:

```sql
-- one column at a time
COPY (
    SELECT id,
           email::MSSQL_VARCHAR(320) AS email,
           name::MSSQL_NVARCHAR(100) AS name,
           notes                                  -- left alone: nvarchar(max)
    FROM staging
) TO 'mssql://mssql_db/dbo/customers' (FORMAT 'bcp');

-- or size every unannotated string column at once, for this statement only
COPY (SELECT * FROM staging)
    TO 'mssql://mssql_db/dbo/customers' (FORMAT 'bcp', STRING_LENGTH 200);

-- shape the table it creates
COPY (SELECT * FROM staging)
    TO 'mssql://mssql_db/dbo/facts' (FORMAT 'bcp', TABLE_KIND 'columnstore', TABLOCK true);
```

A cast wins over `STRING_LENGTH`, which wins over
`mssql_default_string_length`. Copying **from** an attached MSSQL table needs
none of them: the catalog already reports the source's declared types, so the
target inherits them.

These options apply only when COPY **creates** the target. Loading into a table
that already exists uses that table's types, and `STRING_LENGTH` / `TABLE_KIND`
are ignored — COPY does not restructure someone else's table.

The bound is **informational on the DuckDB side**. The value is an ordinary
DuckDB string and nothing shortens it there; the bound applies when the data is
written, and an over-long value is **truncated to fit**, not rejected. That is
what SQL Server's own `CONVERT` does, and refusing a load the server would have
accepted is worse than matching it.

The cut respects character boundaries, which a byte count alone does not: a
UTF-8 sequence is never split, and neither is a UTF-16 surrogate pair — half of
one is a lone surrogate that the server stores and every later read reports as
damaged. A character that does not fit whole is dropped whole. The unit is SQL
Server's own: `nvarchar(n)` bounds **code units** (a non-BMP character costs
two), a UTF-8 `varchar(n)` bounds **bytes**.

Numbers are the opposite case and still error: a `BIGINT` that does not fit an
`int` is a different number, and nothing about the target type says the low 32
bits were wanted.

A `MSSQL_VARCHAR` column is given a UTF-8 collation (`mssql_utf8_collation`), or
the one you name in the second argument. Without a UTF-8 collation a single-byte
column silently replaces every character outside the database's code page with
`?` on insert, so this is not optional: on a server that never granted the TDS
`UTF8SUPPORT` feature the statement errors instead.

### Types reported by the catalog

Attached tables report their declared string types, so a target created from an
MSSQL source inherits them with no cast at all:

```sql
-- dst gets src's column types, including lengths and collations
COPY (SELECT * FROM mssql_db.dbo.src) TO 'mssql://mssql_db/dbo/dst' (FORMAT 'bcp');
```

```console
D DESCRIBE SELECT email FROM mssql_db.dbo.customers;
┌─────────────┬────────────────────┐
│ column_name │    column_type     │
│  email      │ MSSQL_VARCHAR(320) │
└─────────────┴────────────────────┘
```

Set `mssql_catalog_native_types = false` to report a bare `VARCHAR` as before —
useful for tooling that matches on the literal type name. Table entries are
cached, so call `mssql_invalidate_cache()` to apply the change to tables already
loaded.

### Defaults for unannotated columns

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `mssql_ctas_text_type` | VARCHAR | `NVARCHAR` | Type an unannotated `VARCHAR` gets from CTAS **and** COPY: `NVARCHAR` or `VARCHAR` |
| `mssql_default_string_length` | BIGINT | `0` | Length for an unannotated `VARCHAR` column (0 = MAX) |
| `mssql_default_table_kind` | VARCHAR | `HEAP` | Shape of a created table: `HEAP` or `COLUMNSTORE` |
| `mssql_catalog_native_types` | BOOLEAN | `true` | Report `MSSQL_VARCHAR(n)` / `MSSQL_NVARCHAR(n)` for attached string columns |
| `mssql_utf8_collation` | VARCHAR | `Latin1_General_100_CI_AS_SC_UTF8` | Collation for created `varchar` columns; empty inherits the database default |

`mssql_default_string_length` says "no string in this data is longer than n".
Anything longer is truncated to `n` on a character boundary, as described above.
Above SQL Server's inline limit (4000 for `nvarchar`, 8000 for `varchar`) the
column stays MAX. A cast beats this for anything but a uniform schema.

### Table shape: `CREATE TABLE ... WITH (...)`

| Option | Values | Description |
|--------|--------|-------------|
| `table_kind` | `HEAP`, `COLUMNSTORE` | `COLUMNSTORE` adds a clustered columnstore index |
| `clustered_index` | column list | Clustered rowstore index over those key columns |
| `data_compression` | `PAGE`, `ROW`, `NONE` | Table compression |

```sql
CREATE TABLE mssql_db.dbo.facts (id BIGINT, amount DECIMAL(18,2))
    WITH (table_kind = 'columnstore');

CREATE TABLE mssql_db.dbo.orders (id BIGINT, placed_at TIMESTAMP)
    WITH (clustered_index = 'id', data_compression = 'PAGE');
```

The index is created **before** the load, not after: a clustered columnstore
built after the rows are in place costs a full rebuild, while building it first
lets the load write compressed rowgroups directly.

`DATA_COMPRESSION` applies during a bulk load only when the load takes a table
lock — see `mssql_copy_tablock`. Without it the rows land uncompressed and stay
that way until someone rebuilds, which is not what the statement asked for.

An option name that is not in the table above is an error, not a silently
ignored request.

