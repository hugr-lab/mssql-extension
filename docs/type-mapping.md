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

### Unsupported Types

| SQL Server Type | TDS Type ID | Reason |
|---|---|---|
| XML | 0xF1 | Complex nested structure |
| UDT (GEOGRAPHY, GEOMETRY, HIERARCHYID) | 0xF0 | User-defined CLR types |
| SQL_VARIANT | 0x62 | Dynamic type container |
| IMAGE | 0x22 | Deprecated (use VARBINARY(MAX)) |
| TEXT | 0x23 | Deprecated (use VARCHAR(MAX)) |
| NTEXT | 0x63 | Deprecated (use NVARCHAR(MAX)) |

Unsupported types produce the error:
```
MSSQL Error: Unsupported SQL Server type 'XML' (0xF1) for column 'col_name'.
Consider casting to VARCHAR or excluding this column.
```

## NULL Handling

| Type Category | NULL Indicator |
|---|---|
| Fixed non-nullable (INT, BIT, etc.) | Cannot be NULL in wire format |
| Nullable variant (INTN, FLOATN, BITN) | Length byte = 0 |
| Variable-length (VARCHAR, VARBINARY) | Length = 0xFFFF |
| DECIMAL/NUMERIC | Length byte = 0 |
| PLP types (MAX) | Total length = 0xFFFFFFFFFFFFFFFF |

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
