# Contract: SQL Server to DuckDB Type Mapping

**Feature**: 004-streaming-select-cancel
**Date**: 2026-01-15

## Overview

This document defines the mapping from SQL Server data types to DuckDB types, including wire format details and conversion rules.

## Supported Type Mappings

### Integer Types

| SQL Server Type | TDS Type ID | Wire Format | DuckDB Type | Conversion Notes |
|-----------------|-------------|-------------|-------------|------------------|
| TINYINT | 0x30 | uint8_t | TINYINT | Direct copy |
| SMALLINT | 0x34 | int16_t LE | SMALLINT | Direct copy |
| INT | 0x38 | int32_t LE | INTEGER | Direct copy |
| BIGINT | 0x7F | int64_t LE | BIGINT | Direct copy |
| INTN | 0x26 | len + value | TINYINT/SMALLINT/INTEGER/BIGINT | Map by length |

### Boolean Type

| SQL Server Type | TDS Type ID | Wire Format | DuckDB Type | Conversion Notes |
|-----------------|-------------|-------------|-------------|------------------|
| BIT | 0x32 | uint8_t | BOOLEAN | 0 = false, else true |
| BITN | 0x68 | len + value | BOOLEAN | Same with nullable |

### Floating-Point Types

| SQL Server Type | TDS Type ID | Wire Format | DuckDB Type | Conversion Notes |
|-----------------|-------------|-------------|-------------|------------------|
| REAL | 0x3B | IEEE 754 single | FLOAT | Direct copy |
| FLOAT | 0x3E | IEEE 754 double | DOUBLE | Direct copy |
| FLOATN | 0x6D | len + value | FLOAT/DOUBLE | 4 bytes → FLOAT, 8 bytes → DOUBLE |

### Decimal Types

| SQL Server Type | TDS Type ID | Wire Format | DuckDB Type | Conversion Notes |
|-----------------|-------------|-------------|-------------|------------------|
| DECIMAL | 0x6A | sign + magnitude | DECIMAL(p,s) | Preserve precision/scale |
| NUMERIC | 0x6C | sign + magnitude | DECIMAL(p,s) | Same as DECIMAL |
| MONEY | 0x3C | int64_t LE | DECIMAL(19,4) | Divide by 10000 |
| SMALLMONEY | 0x7A | int32_t LE | DECIMAL(10,4) | Divide by 10000 |
| MONEYN | 0x6E | len + value | DECIMAL(10,4) or DECIMAL(19,4) | By length |

### String Types

| SQL Server Type | TDS Type ID | Wire Format | DuckDB Type | Conversion Notes |
|-----------------|-------------|-------------|-------------|------------------|
| CHAR | 0xAF | fixed bytes | VARCHAR | Trim trailing spaces |
| VARCHAR | 0xA7 | len16 + bytes | VARCHAR | Direct, respect collation |
| NCHAR | 0xEF | fixed UTF-16LE | VARCHAR | Convert UTF-16LE → UTF-8, trim |
| NVARCHAR | 0xE7 | len16 + UTF-16LE | VARCHAR | Convert UTF-16LE → UTF-8 |

### Date/Time Types

| SQL Server Type | TDS Type ID | Wire Format | DuckDB Type | Conversion Notes |
|-----------------|-------------|-------------|-------------|------------------|
| DATE | 0x28 | 3-byte days | DATE | Days since 0001-01-01 |
| TIME | 0x29 | 3-5 byte ticks | TIME | Scale-dependent |
| DATETIME | 0x3D | 4+4 bytes | TIMESTAMP | Days + ticks conversion |
| DATETIME2 | 0x2A | time + date | TIMESTAMP | High-precision |
| DATETIMEN | 0x6F | len + value | TIMESTAMP | 4 bytes = SMALLDATETIME |

### Binary Types

| SQL Server Type | TDS Type ID | Wire Format | DuckDB Type | Conversion Notes |
|-----------------|-------------|-------------|-------------|------------------|
| BINARY | 0xAD | fixed bytes | BLOB | Direct copy |
| VARBINARY | 0xA5 | len16 + bytes | BLOB | Direct copy |
| UNIQUEIDENTIFIER | 0x24 | 16-byte GUID | UUID | Reorder bytes |

## Unsupported Types

The following types are NOT supported and will cause query failure:

| SQL Server Type | TDS Type ID | Reason |
|-----------------|-------------|--------|
| XML | 0xF1 | Complex nested structure |
| GEOGRAPHY | 0xF0 | Spatial type, no DuckDB equivalent |
| GEOMETRY | 0xF0 | Spatial type, no DuckDB equivalent |
| SQL_VARIANT | 0x62 | Dynamic type, complex parsing |
| HIERARCHYID | 0xF0 | SQL Server-specific |
| IMAGE | 0x22 | Deprecated, use VARBINARY(MAX) |
| TEXT | 0x23 | Deprecated, use VARCHAR(MAX) |
| NTEXT | 0x63 | Deprecated, use NVARCHAR(MAX) |
| TIMESTAMP/ROWVERSION | 0xAD | Binary, not temporal |

## Detailed Conversion Rules

### DECIMAL/NUMERIC Conversion

```cpp
// TDS wire format: sign (1 byte) + magnitude (variable)
// Magnitude is little-endian integer

hugeint_t ConvertDecimal(const uint8_t* data, size_t length, uint8_t precision, uint8_t scale) {
    bool negative = data[0] != 0;

    // Read magnitude as little-endian integer
    hugeint_t magnitude = 0;
    for (size_t i = length - 1; i >= 1; i--) {
        magnitude = (magnitude << 8) | data[i];
    }

    // DuckDB DECIMAL stores unscaled value
    // Value = magnitude (already unscaled in TDS)
    return negative ? -magnitude : magnitude;
}
```

### MONEY Conversion

```cpp
// MONEY: int64_t representing value × 10000
// SMALLMONEY: int32_t representing value × 10000

Decimal ConvertMoney(int64_t raw_value) {
    // DuckDB DECIMAL(19,4) stores unscaled value
    // raw_value is already the unscaled representation
    return Decimal(raw_value);  // Scale = 4
}
```

### DATETIME Conversion

```cpp
// DATETIME format:
//   - 4 bytes: days since 1900-01-01 (int32_t)
//   - 4 bytes: ticks since midnight (int32_t, 1 tick = 1/300 second)

timestamp_t ConvertDatetime(int32_t days, int32_t ticks) {
    // Convert to days since 1970-01-01
    int32_t unix_days = days - 25567;  // 1900-01-01 to 1970-01-01

    // Convert ticks to microseconds
    int64_t microseconds = (int64_t)ticks * 10000 / 3;  // 1/300 sec → μs

    // DuckDB timestamp: microseconds since epoch
    return unix_days * 86400000000LL + microseconds;
}
```

### DATETIME2 Conversion

```cpp
// DATETIME2 format (scale-dependent):
//   - Time: 3-5 bytes (100ns units, scale-dependent precision)
//   - Date: 3 bytes (days since 0001-01-01)

timestamp_t ConvertDatetime2(const uint8_t* data, uint8_t scale) {
    size_t time_len = (scale <= 2) ? 3 : (scale <= 4) ? 4 : 5;

    // Read time (little-endian)
    int64_t time_ticks = 0;
    for (size_t i = 0; i < time_len; i++) {
        time_ticks |= (int64_t)data[i] << (i * 8);
    }

    // Read date
    int32_t days = data[time_len] | (data[time_len + 1] << 8) | (data[time_len + 2] << 16);

    // Convert to days since 1970-01-01
    int32_t unix_days = days - 719162;  // 0001-01-01 to 1970-01-01

    // Convert time to microseconds (from 100ns units)
    int64_t microseconds = time_ticks / 10;

    return unix_days * 86400000000LL + microseconds;
}
```

### DATE Conversion

```cpp
// DATE format: 3 bytes, days since 0001-01-01

date_t ConvertDate(const uint8_t* data) {
    int32_t days = data[0] | (data[1] << 8) | (data[2] << 16);

    // Convert to days since 1970-01-01
    return days - 719162;  // 0001-01-01 to 1970-01-01
}
```

### TIME Conversion

```cpp
// TIME format: 3-5 bytes, ticks since midnight (100ns units, scale-dependent)

dtime_t ConvertTime(const uint8_t* data, uint8_t scale) {
    size_t len = (scale <= 2) ? 3 : (scale <= 4) ? 4 : 5;

    int64_t ticks = 0;
    for (size_t i = 0; i < len; i++) {
        ticks |= (int64_t)data[i] << (i * 8);
    }

    // Convert to microseconds (from 100ns units)
    return ticks / 10;
}
```

### UNIQUEIDENTIFIER Conversion

```cpp
// GUID wire format (mixed-endian):
//   bytes 0-3: Data1 (little-endian uint32)
//   bytes 4-5: Data2 (little-endian uint16)
//   bytes 6-7: Data3 (little-endian uint16)
//   bytes 8-15: Data4 (big-endian, as-is)
// DuckDB UUID is stored as big-endian hugeint_t

hugeint_t ConvertGuid(const uint8_t* data) {
    uint8_t reordered[16];

    // Reverse byte order for first 4 bytes
    reordered[0] = data[3];
    reordered[1] = data[2];
    reordered[2] = data[1];
    reordered[3] = data[0];

    // Reverse byte order for bytes 4-5
    reordered[4] = data[5];
    reordered[5] = data[4];

    // Reverse byte order for bytes 6-7
    reordered[6] = data[7];
    reordered[7] = data[6];

    // Bytes 8-15 stay as-is
    memcpy(reordered + 8, data + 8, 8);

    // Convert to hugeint_t (big-endian)
    return LoadBigEndianHugeint(reordered);
}
```

### UTF-16LE to UTF-8 Conversion

```cpp
// NVARCHAR/NCHAR data is UTF-16LE encoded
// DuckDB stores strings as UTF-8

string ConvertUtf16ToUtf8(const uint8_t* data, size_t byte_length) {
    // Use existing UTF-16 converter from encoding/utf16.hpp
    return Utf16ToUtf8(data, byte_length);
}
```

## NULL Handling

### Fixed-Length Nullable Types (INTN, FLOATN, etc.)

```cpp
// Length byte = 0 indicates NULL
if (length == 0) {
    FlatVector::SetNull(vector, row_idx, true);
    return;
}
```

### Variable-Length Types (VARCHAR, VARBINARY)

```cpp
// Length = 0xFFFF (65535) indicates NULL
uint16_t length = ReadUint16LE(data);
if (length == 0xFFFF) {
    FlatVector::SetNull(vector, row_idx, true);
    return;
}
```

### Fixed Non-Nullable Types

These types cannot be NULL in the wire format. If a column is nullable, SQL Server uses the nullable variant (e.g., INTN instead of INT).

## Type Mapping Function

```cpp
LogicalType GetDuckDBType(const ColumnMetadata& col) {
    switch (col.type_id) {
        case 0x30: return LogicalType::TINYINT;
        case 0x34: return LogicalType::SMALLINT;
        case 0x38: return LogicalType::INTEGER;
        case 0x7F: return LogicalType::BIGINT;
        case 0x26: // INTN
            switch (col.max_length) {
                case 1: return LogicalType::TINYINT;
                case 2: return LogicalType::SMALLINT;
                case 4: return LogicalType::INTEGER;
                case 8: return LogicalType::BIGINT;
            }
            break;
        case 0x32: case 0x68: return LogicalType::BOOLEAN;
        case 0x3B: return LogicalType::FLOAT;
        case 0x3E: return LogicalType::DOUBLE;
        case 0x6D: // FLOATN
            return (col.max_length == 4) ? LogicalType::FLOAT : LogicalType::DOUBLE;
        case 0x6A: case 0x6C: // DECIMAL, NUMERIC
            return LogicalType::DECIMAL(col.precision, col.scale);
        case 0x3C: return LogicalType::DECIMAL(19, 4);  // MONEY
        case 0x7A: return LogicalType::DECIMAL(10, 4);  // SMALLMONEY
        case 0xAF: case 0xA7: case 0xEF: case 0xE7: // CHAR, VARCHAR, NCHAR, NVARCHAR
            return LogicalType::VARCHAR;
        case 0x28: return LogicalType::DATE;
        case 0x29: return LogicalType::TIME;
        case 0x3D: case 0x2A: case 0x6F: return LogicalType::TIMESTAMP;
        case 0xAD: case 0xA5: return LogicalType::BLOB;
        case 0x24: return LogicalType::UUID;
        default:
            throw InvalidInputException(
                "Unsupported SQL Server type: 0x%02X for column '%s'",
                col.type_id, col.name.c_str());
    }
}
```

## Error Messages

When an unsupported type is encountered:

```
MSSQL Error: Unsupported SQL Server type 'XML' (0xF1) for column 'xml_data'.
Consider casting to VARCHAR or excluding this column from your query.
```

When conversion fails:

```
MSSQL Error: Failed to convert value for column 'decimal_col' (DECIMAL(38,10)).
Value exceeds DuckDB DECIMAL precision limits.
```
