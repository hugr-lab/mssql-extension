# SQL Server ↔ DuckDB Type Mapping

Type mapping is bidirectional: SQL Server → DuckDB for SELECT operations (via `TypeConverter`) and DuckDB → SQL Server for INSERT operations (via `MSSQLValueSerializer`).

## Complete Type Mapping Table

### Integer Types

| SQL Server Type | TDS Type ID | Wire Format | DuckDB Type | Notes |
|---|---|---|---|---|
| TINYINT | 0x30 | 1 byte unsigned | UTINYINT | SQL Server TINYINT is unsigned (0-255) |
| SMALLINT | 0x34 | 2 bytes LE signed | SMALLINT | |
| INT | 0x38 | 4 bytes LE signed | INTEGER | |
| BIGINT | 0x7F | 8 bytes LE signed | BIGINT | |
| INTN | 0x26 | 1-byte len + value | Variable | Maps by max_length: 1→UTINYINT, 2→SMALLINT, 4→INTEGER, 8→BIGINT |

### Boolean Type

| SQL Server Type | TDS Type ID | Wire Format | DuckDB Type | Notes |
|---|---|---|---|---|
| BIT | 0x32 | 1 byte | BOOLEAN | 0=false, non-zero=true |
| BITN | 0x68 | 1-byte len + value | BOOLEAN | Nullable variant |

### Floating-Point Types

| SQL Server Type | TDS Type ID | Wire Format | DuckDB Type | Notes |
|---|---|---|---|---|
| REAL | 0x3B | 4 bytes IEEE 754 | FLOAT | 32-bit float |
| FLOAT | 0x3E | 8 bytes IEEE 754 | DOUBLE | 64-bit double |
| FLOATN | 0x6D | 1-byte len + value | FLOAT or DOUBLE | 4 bytes → FLOAT, 8 bytes → DOUBLE |

### Decimal and Money Types

| SQL Server Type | TDS Type ID | Wire Format | DuckDB Type | Notes |
|---|---|---|---|---|
| DECIMAL | 0x6A | sign + LE magnitude | DECIMAL(p,s) | Precision/scale from metadata |
| NUMERIC | 0x6C | sign + LE magnitude | DECIMAL(p,s) | Same encoding as DECIMAL |
| MONEY | 0x3C | 8 bytes LE | DECIMAL(19,4) | Value × 10000 |
| SMALLMONEY | 0x7A | 4 bytes LE | DECIMAL(10,4) | Value × 10000 |
| MONEYN | 0x6E | 1-byte len + value | DECIMAL | 4 bytes → (10,4), 8 bytes → (19,4) |

**DECIMAL wire format**: `[sign_byte][magnitude_bytes]`
- sign_byte: 0 = negative, 1 = positive
- magnitude: little-endian unsigned integer (unscaled value)

**DuckDB storage optimization** based on precision:
- p ≤ 4: stored as `int16_t`
- p ≤ 9: stored as `int32_t`
- p ≤ 18: stored as `int64_t`
- p > 18: stored as `hugeint_t`

### String Types

| SQL Server Type | TDS Type ID | Wire Format | DuckDB Type | Notes |
|---|---|---|---|---|
| CHAR | 0xAF | Fixed bytes | VARCHAR | Single-byte encoding, trailing spaces trimmed |
| VARCHAR | 0xA7 | 2-byte len LE + bytes | VARCHAR | Single-byte encoding |
| NCHAR | 0xEF | Fixed UTF-16LE | VARCHAR | UTF-16LE → UTF-8, trailing spaces trimmed |
| NVARCHAR | 0xE7 | 2-byte len LE + UTF-16LE | VARCHAR | UTF-16LE → UTF-8 |
| VARCHAR(MAX) | 0xA7 | PLP encoding | VARCHAR | max_length = 0xFFFF |
| NVARCHAR(MAX) | 0xE7 | PLP encoding | VARCHAR | max_length = 0xFFFF |

All string data in TDS uses UTF-16LE encoding. The extension decodes to UTF-8 for DuckDB storage.

### Date/Time Types

| SQL Server Type | TDS Type ID | Wire Format | DuckDB Type | Notes |
|---|---|---|---|---|
| DATE | 0x28 | 3 bytes LE unsigned | DATE | Days since 0001-01-01 |
| TIME | 0x29 | 3-5 bytes LE | TIME | 100ns ticks since midnight |
| DATETIME | 0x3D | 4+4 bytes | TIMESTAMP | Days since 1900-01-01 + 1/300s ticks |
| DATETIME2 | 0x2A | time + 3-byte date | TIMESTAMP | 100ns time + days since 0001 |
| SMALLDATETIME | 0x3A | 2+2 bytes | TIMESTAMP | Days since 1900-01-01 + minutes |
| DATETIMEN | 0x6F | 1-byte len + value | TIMESTAMP | 4 bytes = SMALLDATETIME, 8 = DATETIME |
| DATETIMEOFFSET | 0x2B | time + date + offset | TIMESTAMP_TZ | Stored as UTC after subtracting offset |

**TIME byte length by scale**:
- Scale 0-2: 3 bytes
- Scale 3-4: 4 bytes
- Scale 5-7: 5 bytes

**Epoch conversions**:
- DATE/DATETIME2: 0001-01-01 epoch (719,162 days to 1970-01-01)
- DATETIME/SMALLDATETIME: 1900-01-01 epoch (25,567 days to 1970-01-01)
- DATETIME tick: 1/300 second → multiply by 10000/3 for microseconds

### Binary Types

| SQL Server Type | TDS Type ID | Wire Format | DuckDB Type | Notes |
|---|---|---|---|---|
| BINARY | 0xAD | Fixed bytes | BLOB | |
| VARBINARY | 0xA5 | 2-byte len LE + bytes | BLOB | |
| VARBINARY(MAX) | 0xA5 | PLP encoding | BLOB | max_length = 0xFFFF |

### UUID Type

| SQL Server Type | TDS Type ID | Wire Format | DuckDB Type | Notes |
|---|---|---|---|---|
| UNIQUEIDENTIFIER | 0x24 | 16 bytes mixed-endian | UUID | Byte reordering required |

**SQL Server GUID wire format (mixed-endian)**:

```
Bytes 0-3:  Data1 (little-endian uint32)
Bytes 4-5:  Data2 (little-endian uint16)
Bytes 6-7:  Data3 (little-endian uint16)
Bytes 8-15: Data4 (big-endian, as-is)
```

`GuidEncoding::ReorderGuidBytes()` converts to standard big-endian format before creating the DuckDB `hugeint_t` UUID representation.

### XML and JSON Types

| SQL Server Type | TDS Type ID | Wire Format | DuckDB Type | Notes |
|---|---|---|---|---|
| XML | 0xF1 | PLP encoding (UTF-16LE) | VARCHAR | UTF-16LE → UTF-8, up to 2 GB |
| JSON (2025) | 0xA7 | `varchar(max)` PLP, UTF-8 BIN2 collation | JSON | read verbatim; JSONSUPPORT (0x0D) is not requested, so the server sends it as `varchar(max)` (spec 093 D7) |

XML uses PLP (Partially Length-Prefixed) wire encoding with UTF-16LE data, identical to NVARCHAR(MAX). The column metadata includes a SCHEMA_PRESENT flag byte (and optional XML schema info if schema-bound).

**Read path**: XML columns are decoded via the same `ReadPLPType()` + `Utf16LEDecode()` code path as NVARCHAR(MAX).

**Write path (BCP/COPY TO)**: SQL Server rejects the native XML type (0xF1) in INSERT BULK / BCP COLMETADATA. The extension remaps XML columns to NVARCHAR(MAX) in the BCP wire format — SQL Server auto-converts NVARCHAR data to XML on the target column. No practical length limitation: NVARCHAR(MAX) supports up to 2 GB, same as XML.

**DML (INSERT/UPDATE via SQL literals)**: Small XML values (up to 4096 bytes serialized) are allowed. Larger values are rejected with an error recommending COPY TO with BCP protocol, since SQL literal serialization has size limits while XML documents can be up to 2 GB.

### SQL_VARIANT Type

| SQL Server Type | TDS Type ID | Wire Format | DuckDB Type | Notes |
|---|---|---|---|---|
| SQL_VARIANT | 0x62 | 4-byte len + base type, property bytes, value | VARIANT | Each value keeps its base type (spec 093 T1) |

Read the same way by an attached catalog and by a raw `mssql_scan` / `mssql_scan_params`, with no
conversion on the server: the envelope's base type picks the family codec that decodes the value. See
the types page for each base type's DuckDB type.

Written from a `VARIANT` (spec 093 T2): each value's base type is the narrowest SQL Server type that
holds every value of its VARIANT tag (spec 093 D3's write table, `codec/variant/variant_write`). On the
statement path the value is `CAST(CAST(<literal> AS <base type>) AS sql_variant)` -- the outer cast
keeps a multi-row `VALUES` list from folding its rows to one type by data type precedence; on the bulk
wire (INSERT BULK, COPY, CTAS, a staged UPDATE / DELETE's `#stage`, a staged MERGE's `#src`) the
column is declared `0x62` with a LONGLEN maximum of 8009 and each value is its envelope (measured to
be taken: `specs/093-type-kernels/measured/S2.md`), a string base value under the database's default
collation on every path (the literal by T-SQL's rule; the envelope from the catalog's database
collation, or the COPY probe's for an existing table). A database collation this build's table lacks
is written on the bulk path as `Latin1_General_100_BIN2_UTF8`, so such a value compares and sorts by
BIN2_UTF8 rather than the database's collation. A subnormal `DOUBLE` (a `float` base value) written by the
statement path is stored as 0 -- the server's parse of the literal flushes it, as it does for a plain
`float` column's literal --, while the bulk paths keep its bits; native RPC parameters (#515) are what
would remove the difference. A plain `float` column's statement path also stores -0.0 as +0 (its
literal `-0.0` is a decimal literal to T-SQL; the float codec's behaviour before T2); a `VARIANT`'s
literal is a float literal and keeps the sign. A CTAS creates the column as `sql_variant`.

### HIERARCHYID and user CLR types

| SQL Server Type | TDS Type ID | Wire Format | DuckDB Type | Notes |
|---|---|---|---|---|
| HIERARCHYID | 0xF0 (UDT `sys.hierarchyid`) | PLP, the MS-SSCLRT 2.2.2 bit string (at most 892 bytes) | VARCHAR | Decoded on the client into its `ToString()` text, `/1/2.5/-3/` (spec 093 T4) |
| user CLR type | 0xF0 (UDT `schema.name`) | PLP, the type's own serialized bytes | BLOB (raw scan) / VARCHAR (catalog) | A raw scan reads the bytes, as `CAST(x AS varbinary(max))` gives them; the catalog reads `CAST(x AS NVARCHAR(MAX))`, the type's `ToString()` |

A `hierarchyid` is read bare by the catalog and a raw `mssql_scan` / `mssql_scan_params` alike, with
no conversion on the server. Its text does not compare or sort like the tree (`/10/` comes after `/2/`
in the tree, before it as text), so a filter or an ORDER BY on the column runs in DuckDB, over the text,
and the column is never a row key; `IS NULL` is pushed. A value is written as a string literal the
server parses (`INSERT … VALUES ('/1/2/')`). A user CLR type's bytes mean nothing without its
assembly, so a raw scan gives them as they are; the describe names it by `user_type_schema` /
`user_type_name` (its `system_type_name` is NULL).

### Not read by a raw scan yet

An attached catalog reads `geometry` / `geography` as WKB through `.STAsBinary()`. A raw
`mssql_scan` / `mssql_scan_params` that selects one bare is refused at bind, naming the column and the
type; spec 093 T5 decodes them on the client. Their COLMETADATA and rows are parsed, so a batch that
returns them to `mssql_exec`, or a DML whose trigger does, does not fail.

| SQL Server Type | TDS Type ID | Read by a raw scan |
|---|---|---|
| UDT (GEOGRAPHY, GEOMETRY) | 0xF0 | not yet (spec 093 T5) |

```
MSSQL Error: column 'g' is of the CLR type sys.geometry, which a raw mssql_scan does not read yet. ...
```

## NULL Handling

| Type Category | NULL Indicator |
|---|---|
| Fixed non-nullable (INT, BIT, etc.) | Cannot be NULL in wire format |
| Nullable variant (INTN, FLOATN, BITN) | Length byte = 0 |
| Variable-length (VARCHAR, VARBINARY) | Length = 0xFFFF |
| DECIMAL/NUMERIC | Length byte = 0 |
| PLP types (MAX, XML) | Total length = 0xFFFFFFFFFFFFFFFF |

SQL Server uses nullable variant types (INTN, FLOATN, etc.) for columns declared as nullable. Fixed types like INT/BIT are only used when the column is NOT NULL.

## Value Serialization (DuckDB → T-SQL)

`MSSQLValueSerializer` (`src/dml/insert/mssql_value_serializer.cpp`) converts DuckDB values to T-SQL literal strings for INSERT statements.

### Serialization Rules

| DuckDB Type | T-SQL Output | Example |
|---|---|---|
| BOOLEAN | `0` or `1` | `1` |
| TINYINT..BIGINT | Decimal string | `42` |
| UBIGINT | `CAST(n AS DECIMAL(20,0))` | `CAST(18446744073709551615 AS DECIMAL(20,0))` |
| FLOAT | 9-digit precision | `3.14159274` |
| DOUBLE | 17-digit precision | `3.1415926535897931` |
| DECIMAL(p,s) | Scaled decimal string | `123.45` |
| VARCHAR | `N'escaped'` | `N'O''Brien'` |
| BLOB | `0xhex` | `0x48656C6C6F` |
| UUID | `'guid-string'` | `'a1b2c3d4-...'` |
| DATE | `'YYYY-MM-DD'` | `'2024-01-15'` |
| TIME | `'HH:MM:SS.fffffff'` | `'12:30:45.1234567'` |
| TIMESTAMP | `CAST('...' AS DATETIME2(7))` | `CAST('2024-01-15T12:30:45.1234567' AS DATETIME2(7))` |
| TIMESTAMP_TZ | `CAST('...' AS DATETIMEOFFSET(7))` | `CAST('2024-01-15T12:30:45.1234567+00:00' AS DATETIMEOFFSET(7))` |
| NULL | `NULL` | `NULL` |

### String Escaping

- **Values**: Single quotes doubled (`'` → `''`), wrapped in `N'...'` for Unicode
- **Identifiers**: Bracket-quoted (`[name]`), right brackets doubled (`]` → `]]`)

### Float/Double Validation

NaN and Infinity are rejected with an error, since SQL Server does not support these IEEE 754 special values.

## Encoding Implementation Files

| File | Purpose |
|---|---|
| `src/tds/encoding/type_converter.cpp` | Main type dispatch (GetDuckDBType, ConvertValue) |
| `src/tds/encoding/datetime_encoding.cpp` | Date/time wire format parsing |
| `src/tds/encoding/decimal_encoding.cpp` | DECIMAL/MONEY wire format parsing |
| `src/tds/encoding/guid_encoding.cpp` | GUID mixed-endian handling |
| `src/tds/encoding/utf16.cpp` | UTF-16LE ↔ UTF-8 conversion |
| `src/dml/insert/mssql_value_serializer.cpp` | DuckDB Value → T-SQL literal |
