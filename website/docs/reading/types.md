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
| `JSON`              | `VARCHAR`      | SQL Server 2025 and later; arrives as UTF-8 `varchar(max)`, read verbatim, and writable by INSERT and COPY as a string |

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

**Writing into a `geometry` or `geography` target works, with one assumption.**
The value goes as `geometry::STGeomFromWKB(0x…, srid)`, the server-side reader
for the OGC WKB a DuckDB `GEOMETRY` carries. It has to be wrapped: a bare
binary value is read as SQL Server's own Spatial Type Binary Format, which is a
different encoding — `0xe6100000 01 0c …` against WKB's `0x01 01000000 …` for
the same point — and fails with
`24210: Geometry type with an unexpected version of 0 received`.

The assumption is the **SRID**, which `.STAsBinary()` does not carry, so a
value that has been read from SQL Server has already lost it. A `geometry`
target is given **0**, the planar "undefined" SRID. A `geography` target
refuses 0 outright (error 24204) and is given **4326 / WGS 84**. If your data
is in another spatial reference system, set it server-side after the load
(`UPDATE t SET g = geometry::STGeomFromWKB(g.STAsBinary(), <srid>)`) or write
the WKB into a `varbinary(max)` column and convert it yourself.

An `INSERT` that names a spatial column always goes as statements, whatever
`mssql_insert_bcp_threshold` says: the bulk wire would declare the column as
nvarchar and send the WKB as text. So a spatial load of any size costs a
statement round trip per batch. For bulk volumes, land the WKB in a
`varbinary(max)` column with `COPY` and convert it in one server-side
statement.

`INSERT … RETURNING` works against such a table, including when the RETURNING
list names the spatial column: the generated `OUTPUT` clause reads it through
`.STAsBinary()`, exactly as a scan does.

**`COPY` into a table that has a spatial column drops that column**, silently
if it is nullable and with a misleading NULL error if it is not. Use `INSERT`
for spatial data until that is fixed.

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
declared lengths and collations with no explicit casts. The columns of an
`mssql_scan()` / `mssql_scan_params()` result are reported the same way (so
`typeof()` over one says `MSSQL_VARCHAR(20)`, not `VARCHAR`), except a `char` /
`varchar` under a code-page collation: a raw scan hands its bytes over
untranscoded, and it stays plain `VARCHAR`. Set the option to `false` to
restore plain `VARCHAR` reporting everywhere.

