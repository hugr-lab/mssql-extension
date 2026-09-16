---
title: Type Mapping
sidebar_position: 3
---

# Type Mapping

### Numeric Types

| SQL Server Type   | DuckDB Type    | Notes                        |
| ----------------- | -------------- | ---------------------------- |
| `TINYINT`         | `UTINYINT`     | Unsigned 0-255               |
| `SMALLINT`        | `SMALLINT`     | -32768 to 32767              |
| `INT`             | `INTEGER`      | Standard 32-bit integer      |
| `BIGINT`          | `BIGINT`       | 64-bit integer               |
| `BIT`             | `BOOLEAN`      | 0 or 1                       |
| `REAL`            | `FLOAT`        | 32-bit floating point        |
| `FLOAT`           | `DOUBLE`       | 64-bit floating point        |
| `DECIMAL(p,s)`    | `DECIMAL(p,s)` | Preserves precision/scale    |
| `NUMERIC(p,s)`    | `DECIMAL(p,s)` | Preserves precision/scale    |
| `MONEY`           | `DECIMAL(19,4)`| Fixed precision              |
| `SMALLMONEY`      | `DECIMAL(10,4)`| Fixed precision              |

### String Types

| SQL Server Type   | DuckDB Type    | Notes                        |
| ----------------- | -------------- | ---------------------------- |
| `CHAR(n)`         | `VARCHAR`      | Fixed-length, trailing spaces trimmed |
| `VARCHAR(n)`      | `VARCHAR`      | Variable-length              |
| `NCHAR(n)`        | `VARCHAR`      | UTF-16LE decoded             |
| `NVARCHAR(n)`     | `VARCHAR`      | UTF-16LE decoded             |

**Non-Unicode `CHAR` / `VARCHAR` carry a code page, and only the catalog casts it away.**
`CHAR`, `VARCHAR` and `TEXT` hold bytes in the *column's* code page —
`Latin1_General_CI_AS` is CP1252, `Cyrillic_General_CI_AS` is CP1251 — while
DuckDB `VARCHAR` is UTF-8 by contract. A **catalog scan** rewrites those columns
to `CAST(col AS NVARCHAR(...))` server-side, so three-part-name queries (and the
`COPY` / `CREATE TABLE AS` that read through them) always get UTF-8. A raw
`mssql_scan()` has no such rewrite: the bytes arrive unchanged, and for anything
outside ASCII the result is **not valid UTF-8** — it is not rejected, it is
stored and then mangled by whatever touches it (`upper('naïve')` → `'NA'`).

Add the cast yourself in the T-SQL:

```sql
SELECT * FROM mssql_scan('db',
    'SELECT id, CAST(name AS NVARCHAR(MAX)) AS name FROM dbo.customers');
```

`NVARCHAR(MAX)` rather than a fixed width: SQL Server does not raise on a
narrowing character `CAST`, so `NVARCHAR(50)` would silently truncate. Columns
with a UTF-8 collation (`..._UTF8`, SQL Server 2019+) need nothing — they are
already UTF-8 — but note that the default collation of a SQL Server
installation is *not* one of those. See
[Troubleshooting](../reference/troubleshooting.md#raw-mssql_scan-does-not-transcode-code-pages).

### Binary Types

| SQL Server Type   | DuckDB Type    | Notes                        |
| ----------------- | -------------- | ---------------------------- |
| `BINARY(n)`       | `BLOB`         | Fixed-length binary          |
| `VARBINARY(n)`    | `BLOB`         | Variable-length binary       |
| `ROWVERSION`      | `BLOB`         | 8 bytes; `TIMESTAMP` is its other name and has nothing to do with time |

**`ROWVERSION` / `TIMESTAMP` notes** (issue #296): the column is an 8-byte
counter, and `sys.types` calls it `timestamp`, which is why it is easy to
mistake for a date. It arrives as `binary(8)` on the wire and is read with no
conversion. Do not write it: SQL Server refuses an explicit value with error
273, so leave the column out of the `INSERT` column list and let the server
maintain it.

**It is not a time, so it is not mapped to `TIMESTAMP`.** The counter is
database-wide and advances once per write to *any* table, which is what
`@@DBTS` returns. Three rows, the second written two seconds after the first,
the third ten milliseconds after the second with three writes to another table
in between:

| row | counter | actually written at |
| --- | ------- | ------------------- |
| 1   | 8328    | 10:11:45.351        |
| 2   | 8329    | 10:11:47.351        |
| 3   | 8333    | 10:11:47.361        |

Two seconds of waiting moved it by one; ten milliseconds moved it by four.
Reported as a DuckDB `TIMESTAMP` those values would read
`1970-01-01 00:00:00.008328` and `1970-01-01 00:00:00.008333`: not a rounded
time but an invented one. Microsoft deprecates the `timestamp` spelling for
the same reason and asks for `rowversion`; both are accepted here.

`BLOB` rather than `BIGINT`, which the issue also suggested, for two reasons.
The counter is **unsigned**, so past 2^63 a `BIGINT` would report negative
values for perfectly ordinary rows. And the bytes are **big-endian**, so a
`BLOB` compares bytewise in exactly the order the counter advances: the
change-tracking query these columns exist for, `WHERE rv > <last seen>`,
orders and filters correctly with no conversion on either side, and the
comparison pushes down to the server as a `varbinary` parameter.

### Date/Time Types

| SQL Server Type     | DuckDB Type     | Notes                        |
| ------------------- | --------------- | ---------------------------- |
| `DATE`              | `DATE`          | Date only                    |
| `TIME`              | `TIME`          | Up to 100ns precision        |
| `DATETIME`          | `TIMESTAMP`     | 3.33ms precision             |
| `SMALLDATETIME`     | `TIMESTAMP`     | 1 minute precision           |
| `DATETIME2(0)`      | `TIMESTAMP_S`   | Scale-precise mapping (spec 045) |
| `DATETIME2(1-3)`    | `TIMESTAMP_MS`  | |
| `DATETIME2(4-6)`    | `TIMESTAMP`     | Microseconds |
| `DATETIME2(7)`      | `TIMESTAMP_NS`  | Round-trips losslessly |
| `DATETIMEOFFSET`    | `TIMESTAMP_TZ`  | Timezone-aware               |

### Special Types

| SQL Server Type     | DuckDB Type    | Notes                        |
| ------------------- | -------------- | ---------------------------- |
| `UNIQUEIDENTIFIER`  | `UUID`         | 128-bit GUID                 |
| `XML`               | `VARCHAR`      | PLP encoding, UTF-16LE decoded to UTF-8, up to 2 GB |
| `JSON`              | `VARCHAR`      | SQL Server 2025 and later; arrives as UTF-8 `varchar(max)` and is read verbatim |

**XML type notes:**
- **SELECT**: XML columns are read via the same PLP + UTF-16LE code path as NVARCHAR(MAX)
- **COPY TO (BCP)**: Supported — XML is remapped to NVARCHAR(MAX) on the wire (SQL Server auto-converts)
- **CTAS**: Supported via BCP protocol
- **INSERT/UPDATE via SQL literals**: Supported for small values (up to 4096 bytes). Larger XML values error with a recommendation to use COPY TO with BCP protocol

### Legacy LOB Types

`TEXT`, `NTEXT` and `IMAGE` columns **are readable**: `TEXT`/`NTEXT` arrive as
`VARCHAR`, `IMAGE` as `BLOB`. Both the catalog scan and raw `mssql_scan()`
decode the legacy LOB wire forms natively (issue #197).

### Spatial Types

`geometry` and `geography` columns map to DuckDB `GEOMETRY`: the scan rewrites
them to WKB (`.STAsBinary()`), so they compose with the DuckDB `spatial`
extension. On the write side, a GEOMETRY source column lands in a
`varbinary`/`binary`/`image` target as standard WKB.

### Other Server-Specific Types

`SQL_VARIANT`, `hierarchyid` and CLR UDT columns are auto-CAST to
`NVARCHAR(MAX)` by the catalog scan, so three-part-name queries return their
text form. Raw `mssql_scan()` queries that select such columns without a CAST
will fail — add an explicit `CAST(col AS NVARCHAR(MAX))` in the T-SQL.

### Catalog-Reported String Types

With `mssql_catalog_native_types = true` (the default, spec 060), bounded
string columns of attached tables report as `MSSQL_VARCHAR(n)` /
`MSSQL_NVARCHAR(n)` in `DESCRIBE` and `duckdb_columns()` rather than bare
`VARCHAR` — this is what lets a target created from an MSSQL source inherit
declared lengths and collations with no explicit casts. Set the option to
`false` to restore plain `VARCHAR` reporting.

