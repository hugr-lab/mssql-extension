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

**Non-Unicode `CHAR` / `VARCHAR` carry a code page, and arrive as UTF-8.**
`CHAR`, `VARCHAR` and `TEXT` hold bytes in the *column's* code page --
`Latin1_General_CI_AS` is CP1252, `Cyrillic_General_CI_AS` is CP1251 -- while
DuckDB `VARCHAR` is UTF-8 by contract. On a single-byte page the column is read as
itself, one byte a character, and the client converts it. A catalog read of a
double-byte page, and of any `TEXT`, is cast to `NVARCHAR` on the server, so the
query's T-SQL (`EXPLAIN`) shows the cast there. A raw `mssql_scan()` has no cast to
keep, and the client converts every page. The client reads a byte exactly as the
server's `CAST` would: its tables come from a SQL Server.

`client_transcode false` on the ATTACH (or `mssql_client_transcode = false`) makes the
server convert every catalog column instead, and a raw `mssql_scan()` then returns the
bytes as they are: see
[Troubleshooting](../reference/troubleshooting.md#when-code-page-bytes-reach-you-unconverted).
Columns with a UTF-8 collation (`..._UTF8`, SQL Server 2019+) are UTF-8 already.

**`CHAR(n)` arrives without its padding, whatever the collation.** SQL Server
stores `CHAR(n)` / `NCHAR(n)` padded with spaces and compares strings ignoring
trailing spaces, so `'AB'` in a `char(4)` equals `'AB'` in a `char(6)`. DuckDB
compares bytes; the catalog scan therefore reads a fixed-length string without
the padding, so a join, `GROUP BY` or `DISTINCT` computed in DuckDB agrees with
the server. A `CHAR(n)` on a single-byte page arrives as itself and is trimmed on
read, as a UTF-8 one is with UTF8SUPPORT. One on a double-byte page is read as
`CAST(col AS NCHAR(n))` (past 4000, `RTRIM(CAST(col AS NVARCHAR(MAX)))`), and so is a
UTF-8 one on a connection the server did not grant UTF8SUPPORT
(`mssql_utf8_support = false`, or a server that does not acknowledge it). Until #405 the first two kept their padding (`'AB  '`),
and a `char(4)` joined with a `char(6)` found no rows. A raw `mssql_scan()` reads
whatever its own T-SQL returns.

**Prefer a UTF-8 collation (`*_UTF8`) for character columns read through the
extension.** The data then needs no conversion on the way, and with UTF8SUPPORT
it travels as UTF-8 bytes. What stays different whatever the collation: on the
table-scan path DuckDB compares strings byte for byte (case, and trailing
spaces in a `varchar`), while SQL Server compares them under the column's
collation; a statement the remote-pushdown rewriter sends whole is compared by
the server.

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
| `DATETIME2(7)`      | `TIMESTAMP_NS`  | Every 100 ns tick, 1677–2262; or `TIMESTAMP` with `datetime2_7_as` (below) |
| `DATETIMEOFFSET`    | `TIMESTAMP_TZ`  | Timezone-aware               |

**`datetime2(7)`** (the default scale) counts 100 ns ticks over 0001–9999. DuckDB's
`TIMESTAMP_NS` keeps every tick but holds only 1677–2262, so a value outside it -- a temporal
table's `9999-12-31 23:59:59.9999999` `ValidTo`, a `0001-01-01` -- is read as NULL, and each scan
that meets one logs a `WARNING` into `duckdb_logs`, once per column. A statement that needs such a
value as a row's key -- a row-by-row UPDATE or DELETE on a table keyed by the column, or on a table
with no key and such a column -- is refused by name, since the row cannot be found again by a NULL;
one pushed to the server whole is not affected. To read the whole range, set
`mssql_datetime2_7_as = 'timestamp'` before ATTACH, or give the connection `datetime2_7_as 'timestamp'` (an
ATTACH option, a secret parameter, `Datetime2_7_As=timestamp` in a connection string or
`datetime2_7_as=timestamp` in a URI):
the catalog then reads such a column as `TIMESTAMP`, the 100 ns digit truncated, as it reads
`time(7)` and `datetimeoffset(7)`. Read at ATTACH, for the catalog's life. What changes with it:

- A filter on the column decides as DuckDB does over the values it reads (`= c` matches every
  stored value from `c` to the next microsecond); against another column it runs in DuckDB.
- The column no longer identifies a row: a table whose primary key holds it uses another unique
  index; a table with no usable key and such a column -- a heap with a `datetime2(7)` column, say --
  can no longer be keyed by all its columns. Their row-by-row UPDATE / DELETE, and a MERGE whose `ON`
  compares the column, are refused by name; a statement pushed to the server whole is not affected.
- A value written through the column's type drops its seventh digit too: a table created from it
  (CTAS, COPY) gets `datetime2(6)`, and an UPDATE or INSERT that DuckDB evaluates writes microseconds.

A raw `mssql_scan` reads any `datetime2` as `TIMESTAMP`, whatever the setting.

### Special Types

| SQL Server Type     | DuckDB Type    | Notes                        |
| ------------------- | -------------- | ---------------------------- |
| `UNIQUEIDENTIFIER`  | `UUID`         | 128-bit GUID                 |
| `XML`               | `VARCHAR`      | PLP encoding, UTF-16LE decoded to UTF-8, up to 2 GB |
| `JSON`              | `JSON`         | SQL Server 2025 and later; arrives as UTF-8 `varchar(max)`, read verbatim, and writable by INSERT and COPY as a string. A raw `mssql_scan` reads it as `VARCHAR` (the server describes it so). Compared only in DuckDB: the server compares a `json` value by nothing but `IS NULL` |

**XML type notes:**
- **SELECT**: XML columns are read via the same PLP + UTF-16LE code path as NVARCHAR(MAX)
- **COPY TO (BCP)**: Supported — XML is remapped to NVARCHAR(MAX) on the wire (SQL Server auto-converts)
- **CTAS**: Supported via BCP protocol
- **INSERT/UPDATE via SQL literals**: Supported for small values (up to 4096 bytes). Larger XML values error with a recommendation to use COPY TO with BCP protocol

### Vector Type (SQL Server 2025)

| SQL Server Type          | DuckDB Type | Notes |
| ------------------------ | ----------- | ----- |
| `vector(n)`              | `FLOAT[n]`  | n from 1 to 1998; read from its bytes |
| `vector(n, float16)`     | `FLOAT[n]`  | each element widened exactly |

The extension asks the server for its vector wire form at login (`mssql_vector_support`, default
`true`, read when the database is attached). SQL Server 2025 then sends a float32 `vector(n)` as its
bytes, which read exactly. A `vector(n, float16)` still arrives as JSON text, and so does every vector
when the setting is off or the server does not grant the feature: the text is parsed on the client,
exactly for float16 and to within one unit in the last place for float32 (the server prints 8
significant digits, which do not name every float32). A `WARNING` in `duckdb_logs` says so when it
happens to a float32 column.

```sql
SELECT id, embedding FROM sqlserver.dbo.docs;             -- embedding: FLOAT[1536]
SELECT * FROM mssql_scan('sqlserver', 'SELECT embedding FROM dbo.docs');
```

A raw `mssql_scan()` reads a vector the server sends as bytes as `FLOAT[n]`, and one it sends as text
as `VARCHAR`, the server's own text: the text carries no dimension count to type it by.

**Writing.** `INSERT`, `UPDATE`, `MERGE`, `COPY` and CTAS take a `FLOAT[n]` value. A small `INSERT` or
`UPDATE` sends it as text with 9 significant digits, exact for float32, and the server converts it to
the column's type. COPY, a large `INSERT`, CTAS and a staged `UPDATE` / `MERGE` send its bytes, a
float16 column's elements rounded to nearest even. A CTAS or a COPY that creates its table creates
`vector(n)` from `FLOAT[n]`; a `DOUBLE[n]` is refused with the fix named (`::FLOAT[n]`), and so are a
NULL element, a NaN or an infinity, and a float16 value that rounds past 65504 (65520 or more; a
value below that is rounded to 65504). A filter on a vector column runs in
DuckDB, and it is never a row key.

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

A `SQL_VARIANT` column is a DuckDB `VARIANT`, through the catalog and a raw `mssql_scan()`
alike: each value keeps its own base type, read off the wire with no conversion on the
server.

| base type | inside the `VARIANT` |
|---|---|
| `bit` / `tinyint` / `smallint` / `int` / `bigint` | `BOOLEAN` / `UTINYINT` / `SMALLINT` / `INTEGER` / `BIGINT` |
| `real` / `float` | `FLOAT` / `DOUBLE` |
| `decimal(p,s)` / `numeric(p,s)` | `DECIMAL(p,s)` |
| `money` / `smallmoney` | `DECIMAL(19,4)` / `DECIMAL(10,4)` |
| `date` | `DATE` |
| `time(0-6)` / `time(7)` | `TIME` / `TIME_NS` (every 100 ns tick) |
| `datetime2(0)` / `(1-3)` / `(4-6)` / `(7)` | `TIMESTAMP_S` / `TIMESTAMP_MS` / `TIMESTAMP` / `TIMESTAMP_NS` (outside 1677-2262 `TIMESTAMP`, the 100 ns digit dropped) |
| `datetime` / `smalldatetime` | `TIMESTAMP` |
| `datetimeoffset(0-6)` / `(7)` | `TIMESTAMPTZ` / `TIMESTAMPTZ` in nanoseconds inside 1677-2262 -- the UTC instant; the offset is not kept |
| `uniqueidentifier` | `UUID` |
| `binary` / `varbinary` | `BLOB` (a `binary(n)` with its padding) |
| `char` / `varchar` | `VARCHAR`, converted from the value's own collation (any code page, UTF-8 included); `char(n)` trimmed of trailing spaces |
| `nchar` / `nvarchar` | `VARCHAR`; `nchar(n)` trimmed |

`variant_typeof(v)` names a value's type (`INT32`, `DECIMAL(9, 2)`, `TIME_NANOS`, ...), and
`v::INTEGER`, `v::VARCHAR` and the like convert it. A `varchar` under a collation this build has no
code page for -- one newer than the extension --, or under a UTF-8 collation with bytes that are not
UTF-8, is the value's bytes, a `BLOB` inside the `VARIANT`, with a `WARNING` in `duckdb_logs`
naming the column. What a `VARIANT` cannot say -- decimal
vs numeric, money vs decimal, the datetime types apart, varchar vs nvarchar, the collation, the
declared length -- is not kept.

A filter on the column runs in DuckDB, over the `VARIANT`: SQL Server compares a `sql_variant` by its
base type in an order of its own, so `WHERE v = 5` finds the `int` 5 and `WHERE v = '5'` the string
`'5'`, in DuckDB as on the server. Such a column is never a row key. Writing a `VARIANT` into SQL
Server comes with a later release: an `INSERT` / `UPDATE` / `COPY` / CTAS of one is refused by name
(a `NULL` is written as before); write such values with `mssql_exec()`.

`hierarchyid` and CLR UDT columns are read as text by the catalog scan, so three-part-name queries
return a `VARCHAR`; a raw `mssql_scan()` that selects one without a conversion fails: convert it in
the T-SQL.

### Catalog-Reported String Types

With `mssql_catalog_native_types = true` (the default, spec 060), bounded
string columns of attached tables report as `MSSQL_VARCHAR(n)` /
`MSSQL_NVARCHAR(n)` in `DESCRIBE` and `duckdb_columns()` rather than bare
`VARCHAR` — this is what lets a target created from an MSSQL source inherit
declared lengths and collations with no explicit casts. The columns of an
`mssql_scan()` / `mssql_scan_params()` result are reported the same way (so
`typeof()` over one says `MSSQL_VARCHAR(20)`, not `VARCHAR`), except a `varchar(max)`, a
`text`, a statement the server cannot describe, and a code-page `char` / `varchar` under
`client_transcode false`, whose bytes arrive unconverted. A CTAS into DuckLake refuses these
types: cast to `VARCHAR`, or attach the source with `native_types false`. Set the option to `false` to
restore plain `VARCHAR` reporting everywhere -- for tables already read, after
`mssql_invalidate_cache()`: a loaded table keeps the types it was read with.

