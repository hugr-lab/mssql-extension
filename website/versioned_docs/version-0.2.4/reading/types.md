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

### Binary Types

| SQL Server Type   | DuckDB Type    | Notes                        |
| ----------------- | -------------- | ---------------------------- |
| `BINARY(n)`       | `BLOB`         | Fixed-length binary          |
| `VARBINARY(n)`    | `BLOB`         | Variable-length binary       |

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

