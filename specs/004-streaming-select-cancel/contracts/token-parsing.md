# Contract: TDS Token Parsing

**Feature**: 004-streaming-select-cancel
**Date**: 2026-01-15

## Overview

This document defines the binary formats for TDS response tokens that must be parsed for query result streaming.

## Token Type Identifiers

| Token | Hex ID | Description |
|-------|--------|-------------|
| COLMETADATA | 0x81 | Column definitions for result set |
| ROW | 0xD1 | Single row of data |
| DONE | 0xFD | Statement completion |
| DONEPROC | 0xFE | Stored procedure completion |
| DONEINPROC | 0xFF | Intermediate result completion |
| ERROR | 0xAA | Error message |
| INFO | 0xAB | Informational message |
| ENVCHANGE | 0xE3 | Environment change notification |
| ORDER | 0xA9 | Column ordering (ignore) |
| RETURNSTATUS | 0x79 | Stored procedure return status |

## COLMETADATA Token (0x81)

### Structure

```
Offset  Size  Field         Description
------  ----  -----         -----------
0       1     TokenType     0x81
1       2     Count         Number of columns (uint16_t LE)
3       var   Columns[]     Column definitions (Count entries)
```

### Column Definition Structure

```
Offset  Size  Field         Description
------  ----  -----         -----------
0       4     UserType      Legacy user type (uint32_t LE, ignore)
4       2     Flags         Column flags (uint16_t LE)
6       var   TypeInfo      Type-specific information
var     var   ColName       Column name (B_VARCHAR)
```

### Column Flags (bitmask)

| Bit | Mask | Name | Description |
|-----|------|------|-------------|
| 0 | 0x0001 | Nullable | Column allows NULL |
| 1 | 0x0002 | CaseSensitive | Case-sensitive comparison |
| 3 | 0x0008 | Identity | IDENTITY column |
| 4 | 0x0010 | ColumnSet | Sparse column set |
| 5 | 0x0020 | Computed | Computed column |
| 12 | 0x1000 | FixedLenCLRType | Fixed-length CLR type |
| 13 | 0x2000 | Updatable | Column is updatable |

### TypeInfo Formats by Type Category

#### Fixed-Length Integer Types

| Type | TypeID | TypeInfo Size | Format |
|------|--------|---------------|--------|
| TINYINT | 0x30 | 1 | type_id only |
| SMALLINT | 0x34 | 1 | type_id only |
| INT | 0x38 | 1 | type_id only |
| BIGINT | 0x7F | 1 | type_id only |
| BIT | 0x32 | 1 | type_id only |

#### Nullable Integer Types (INTN)

```
Offset  Size  Field       Description
------  ----  -----       -----------
0       1     TypeID      0x26 (INTN)
1       1     Length      1, 2, 4, or 8 bytes
```

#### Floating-Point Types

| Type | TypeID | TypeInfo Format |
|------|--------|-----------------|
| REAL | 0x3B | type_id only |
| FLOAT | 0x3E | type_id only |
| FLOATN | 0x6D | type_id + length (4 or 8) |

#### Decimal/Numeric Types

```
Offset  Size  Field       Description
------  ----  -----       -----------
0       1     TypeID      0x6A (DECIMAL) or 0x6C (NUMERIC)
1       1     Length      Total byte length (5-17)
2       1     Precision   Total digits (1-38)
3       1     Scale       Decimal places (0-precision)
```

#### Money Types

| Type | TypeID | TypeInfo Format |
|------|--------|-----------------|
| SMALLMONEY | 0x7A | type_id only |
| MONEY | 0x3C | type_id only |
| MONEYN | 0x6E | type_id + length (4 or 8) |

#### String Types (Collation Required)

```
Offset  Size  Field       Description
------  ----  -----       -----------
0       1     TypeID      0xA7 (VARCHAR) or 0xAF (CHAR)
1       2     MaxLength   Maximum length (uint16_t LE)
3       5     Collation   Collation info (LCID + flags)
```

For NVARCHAR/NCHAR (0xE7/0xEF):
- MaxLength is in bytes (characters × 2)
- 0xFFFF indicates VARCHAR(MAX)

#### Date/Time Types

| Type | TypeID | TypeInfo Format |
|------|--------|-----------------|
| DATE | 0x28 | type_id only |
| TIME | 0x29 | type_id + scale (0-7) |
| DATETIME | 0x3D | type_id only |
| DATETIME2 | 0x2A | type_id + scale (0-7) |
| DATETIMEN | 0x6F | type_id + length (4 or 8) |

#### Binary Types

```
Offset  Size  Field       Description
------  ----  -----       -----------
0       1     TypeID      0xA5 (VARBINARY) or 0xAD (BINARY)
1       2     MaxLength   Maximum length (uint16_t LE)
```

#### UNIQUEIDENTIFIER

```
Offset  Size  Field       Description
------  ----  -----       -----------
0       1     TypeID      0x24
1       1     Length      16 (always)
```

### B_VARCHAR Format (Column Names)

```
Offset  Size  Field       Description
------  ----  -----       -----------
0       1     Length      String length in characters
1       var   Data        UTF-16LE encoded string (Length × 2 bytes)
```

## ROW Token (0xD1)

### Structure

```
Offset  Size  Field       Description
------  ----  -----       -----------
0       1     TokenType   0xD1
1       var   Values[]    Column values (order matches COLMETADATA)
```

### Value Encoding by Type

#### Fixed-Length Types

Direct binary representation, no length prefix:
- TINYINT: 1 byte
- SMALLINT: 2 bytes LE
- INT: 4 bytes LE
- BIGINT: 8 bytes LE
- REAL: 4 bytes IEEE 754
- FLOAT: 8 bytes IEEE 754
- MONEY: 8 bytes (int64 ÷ 10000)
- SMALLMONEY: 4 bytes (int32 ÷ 10000)
- DATETIME: 8 bytes (4 days + 4 ticks)

#### Nullable Fixed-Length Types (INTN, FLOATN, etc.)

```
Offset  Size  Field       Description
------  ----  -----       -----------
0       1     Length      0 = NULL, else actual length
1       var   Data        Value data (Length bytes)
```

#### Variable-Length Types (VARCHAR, VARBINARY)

```
Offset  Size  Field       Description
------  ----  -----       -----------
0       2     Length      Data length (uint16_t LE)
                          0xFFFF = NULL
2       var   Data        Value data (Length bytes)
```

#### DECIMAL/NUMERIC

```
Offset  Size  Field       Description
------  ----  -----       -----------
0       1     Length      0 = NULL, else data length
1       1     Sign        0 = positive, 1 = negative
2       var   Magnitude   Little-endian integer (Length-1 bytes)
```

#### DATE

```
Offset  Size  Field       Description
------  ----  -----       -----------
0       3     Days        Days since 0001-01-01 (uint24 LE)
```

#### TIME

```
Offset  Size  Field       Description
------  ----  -----       -----------
0       3-5   Ticks       Time value (scale-dependent)
                          Scale 0-2: 3 bytes
                          Scale 3-4: 4 bytes
                          Scale 5-7: 5 bytes
```

#### DATETIME2

```
Offset  Size  Field       Description
------  ----  -----       -----------
0       var   Time        Time portion (3-5 bytes, scale-dependent)
var     3     Date        Date portion (3 bytes, days since 0001-01-01)
```

#### UNIQUEIDENTIFIER

```
Offset  Size  Field       Description
------  ----  -----       -----------
0       1     Length      0 = NULL, 16 = value present
1       16    GUID        Mixed-endian GUID format:
                          bytes 0-3: little-endian
                          bytes 4-5: little-endian
                          bytes 6-7: little-endian
                          bytes 8-15: big-endian
```

## DONE/DONEPROC/DONEINPROC Token

### Structure

```
Offset  Size  Field       Description
------  ----  -----       -----------
0       1     TokenType   0xFD, 0xFE, or 0xFF
1       2     Status      Status flags (uint16_t LE)
3       2     CurCmd      Current command (uint16_t LE)
5       8     RowCount    Row count (uint64_t LE, if DONE_COUNT set)
```

### Status Flags

| Flag | Value | Description |
|------|-------|-------------|
| DONE_FINAL | 0x0000 | Final result |
| DONE_MORE | 0x0001 | More results to follow |
| DONE_ERROR | 0x0002 | Error occurred |
| DONE_INXACT | 0x0004 | In transaction |
| DONE_COUNT | 0x0010 | RowCount is valid |
| DONE_ATTN | 0x0020 | Attention acknowledgment |
| DONE_SRVERROR | 0x0100 | Severe error |

## ERROR Token (0xAA)

### Structure

```
Offset  Size  Field       Description
------  ----  -----       -----------
0       1     TokenType   0xAA
1       2     Length      Token length (uint16_t LE)
3       4     Number      Error number (uint32_t LE)
7       1     State       Error state
8       1     Class       Severity class (0-25)
9       var   MsgText     US_VARCHAR: uint16 length + UTF-16LE
var     var   ServerName  B_VARCHAR: uint8 length + UTF-16LE
var     var   ProcName    B_VARCHAR: uint8 length + UTF-16LE
var     4     LineNumber  Line number (uint32_t LE)
```

### US_VARCHAR Format

```
Offset  Size  Field       Description
------  ----  -----       -----------
0       2     Length      String length in characters (uint16_t LE)
2       var   Data        UTF-16LE encoded string (Length × 2 bytes)
```

## INFO Token (0xAB)

Same structure as ERROR token.

## ENVCHANGE Token (0xE3)

### Structure

```
Offset  Size  Field       Description
------  ----  -----       -----------
0       1     TokenType   0xE3
1       2     Length      Token length (uint16_t LE)
3       1     Type        Change type
4       var   NewValue    B_VARCHAR (new value)
var     var   OldValue    B_VARCHAR (old value)
```

### Change Types (relevant)

| Type | Description | Action |
|------|-------------|--------|
| 1 | Database | Log, update context |
| 2 | Language | Ignore |
| 3 | Character Set | Ignore |
| 4 | Packet Size | Update connection |

## Parsing Algorithm

```
1. Read token type byte
2. Based on token type:
   - COLMETADATA: Parse column count, then each column definition
   - ROW: Parse values according to current column metadata
   - DONE*: Parse status and row count
   - ERROR/INFO: Parse message structure
   - ENVCHANGE: Parse and optionally handle
   - Unknown: Skip based on length field if present, else error
3. Yield parsed token to consumer
4. Repeat until DONE_FINAL received
```

## Error Handling

- If token type is unrecognized and has no length field: throw parse error
- If data is incomplete: buffer and wait for more data
- If type conversion fails: throw type error with column info
