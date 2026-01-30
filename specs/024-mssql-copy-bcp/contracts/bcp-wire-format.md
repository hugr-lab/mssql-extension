# Contract: BCP Wire Format

**Feature**: 024-mssql-copy-bcp
**Date**: 2026-01-29

## Overview

This document specifies the TDS wire format for BulkLoadBCP packets sent from DuckDB to SQL Server.

---

## 1. Protocol Sequence

```
Client                              Server
  |                                    |
  |--- SQL_BATCH (INSERT BULK...) ---->|
  |<--- DONE (success) ----------------|
  |                                    |
  |--- BULK_LOAD (COLMETADATA) ------->|
  |--- BULK_LOAD (ROW) --------------->|  (repeated)
  |--- BULK_LOAD (ROW) --------------->|
  |...                                 |
  |--- BULK_LOAD (DONE) -------------->|
  |<--- DONE (row count) --------------|
```

---

## 2. INSERT BULK Statement

Sent via SQL_BATCH (packet type 0x01) before BCP data:

```sql
INSERT BULK [schema].[table] (
    col1 TYPE1,
    col2 TYPE2,
    ...
) WITH (KEEP_NULLS)
```

**Type declarations must match COLMETADATA exactly.**

---

## 3. TDS Packet Header (8 bytes)

```
Offset  Size  Field       Value
0       1     Type        0x07 (BULK_LOAD)
1       1     Status      0x00 (normal) or 0x01 (EOM)
2       2     Length      Big-endian, includes header
4       2     SPID        Big-endian
6       1     PacketID    Sequential
7       1     Window      0x00 (reserved)
```

---

## 4. COLMETADATA Token (0x81)

### Structure

```
COLMETADATA = 0x81
              column_count (USHORT)
              *column_data
```

### Column Data

```
column_data = user_type (ULONG = 0x00000000)
              flags (USHORT)
              TYPE_INFO
              col_name (B_VARCHAR)
```

**Flags (USHORT):**
```
Bit 0: fNullable (0x0001)
Bit 1: fCaseSen (0x0002)
Bits 2-3: usUpdateable (0x0004 | 0x0008)
Bit 4: fIdentity (0x0010)
```

For nullable columns: flags = `0x0009` (nullable + updatable)

---

## 5. TYPE_INFO by Type

### Integer Types (INTNTYPE = 0x26)

```
TYPE_INFO = 0x26 max_length
            ; max_length: 1=tinyint, 2=smallint, 4=int, 8=bigint
```

### Bit Type (BITNTYPE = 0x68)

```
TYPE_INFO = 0x68 0x01
            ; max_length always 1
```

### Float Types (FLTNTYPE = 0x6D)

```
TYPE_INFO = 0x6D max_length
            ; max_length: 4=real, 8=float
```

### Decimal/Numeric (DECIMALNTYPE = 0x6A)

```
TYPE_INFO = 0x6A max_length precision scale
            ; max_length by precision:
            ;   1-9:   5 bytes (1 sign + 4 mantissa)
            ;   10-19: 9 bytes (1 sign + 8 mantissa)
            ;   20-28: 13 bytes (1 sign + 12 mantissa)
            ;   29-38: 17 bytes (1 sign + 16 mantissa)
```

### Unicode String (NVARCHARTYPE = 0xE7)

```
TYPE_INFO = 0xE7 max_length (USHORT) collation (5 bytes)
            ; max_length: byte count (chars * 2), max 8000
            ; Use 0xFFFF for MAX
```

**Collation (5 bytes):**
```
Bytes 0-3: LCID bits 0-19, ColFlags bits 20-27, Version bits 28-31
Byte 4: SortId

Latin1_General_CI_AS example: 0x09 0x04 0xD0 0x00 0x34
```

### Binary (BIGVARBINARYTYPE = 0xA5)

```
TYPE_INFO = 0xA5 max_length (USHORT)
            ; max_length: byte count, max 8000
            ; Use 0xFFFF for MAX
```

### GUID (GUIDTYPE = 0x24)

```
TYPE_INFO = 0x24 0x10
            ; max_length always 16
```

### Date (DATENTYPE = 0x28)

```
TYPE_INFO = 0x28
            ; no additional fields
```

### Time (TIMENTYPE = 0x29)

```
TYPE_INFO = 0x29 scale
            ; scale: 0-7 (fractional seconds precision)
```

### DateTime2 (DATETIME2NTYPE = 0x2A)

```
TYPE_INFO = 0x2A scale
            ; scale: 0-7 (default 7 for microseconds)
```

### DateTimeOffset (DATETIMEOFFSETNTYPE = 0x2B)

```
TYPE_INFO = 0x2B scale
            ; scale: 0-7
```

---

## 6. ROW Token (0xD1)

```
ROW = 0xD1 *column_value
```

### Column Value Encoding

**Fixed-length (INTNTYPE, BITNTYPE, FLTNTYPE, etc.):**
```
column_value = length (BYTE) data
               ; length = 0 for NULL
               ; length = type's byte count otherwise
```

**Variable-length USHORTLEN (NVARCHARTYPE, BIGVARBINARYTYPE):**
```
column_value = length (USHORT) data
               ; length = 0xFFFF for NULL
               ; length = byte count otherwise
```

### Data Formats

**Integers (little-endian):**
```
tinyint:  [value:1]
smallint: [value:2]
int:      [value:4]
bigint:   [value:8]
```

**Bit:**
```
false: [0x00]
true:  [0x01]
```

**Float (IEEE 754 little-endian):**
```
real:  [value:4]
float: [value:8]
```

**Decimal:**
```
[sign:1] [mantissa:4/8/12/16]
; sign: 0x00=negative, 0x01=non-negative
; mantissa: little-endian unsigned integer
; actual_value = (sign ? 1 : -1) * mantissa / 10^scale
```

**Unicode String (UTF-16LE):**
```
[length:2] [utf16le_bytes:length]
```

**Binary:**
```
[length:2] [bytes:length]
```

**GUID (mixed-endian):**
```
[Data1:4 LE] [Data2:2 LE] [Data3:2 LE] [Data4:8 BE]
```

**Date (days since 0001-01-01):**
```
[days:3 LE unsigned]
```

**Time (10^(-scale) seconds since midnight):**
```
scale 0-2: [value:3 LE]
scale 3-4: [value:4 LE]
scale 5-7: [value:5 LE]
```

**DateTime2:**
```
[time_portion] [date_portion:3]
```

**DateTimeOffset:**
```
[datetime2_portion] [offset_minutes:2 signed LE]
; offset_minutes: -840 to +840
```

---

## 7. DONE Token (0xFD)

```
DONE = 0xFD
       status (USHORT)
       curcmd (USHORT)
       rowcount (ULONGLONG for TDS 7.2+)
```

**Values for BCP completion:**
```
status:   0x0010 (DONE_COUNT)
curcmd:   0x00C3 (INSERT)
rowcount: number of rows in this batch
```

---

## 8. Complete Example

COPY 2 rows to `dbo.Test (ID int, Name nvarchar(50))`:

```hex
# COLMETADATA
81                      # Token
02 00                   # 2 columns

# Column 1: ID
00 00 00 00             # UserType
09 00                   # Flags (nullable, updatable)
26                      # INTNTYPE
04                      # MaxLength = 4
02 00                   # ColName length
49 00 44 00             # "ID" UTF-16LE

# Column 2: Name
00 00 00 00             # UserType
09 00                   # Flags
E7                      # NVARCHARTYPE
64 00                   # MaxLength = 100 bytes
09 04 D0 00 34          # Collation
04 00                   # ColName length
4E 00 61 00 6D 00 65 00 # "Name" UTF-16LE

# ROW 1: (1, "Alice")
D1                      # Token
04                      # ID length
01 00 00 00             # ID = 1
0A 00                   # Name length = 10 bytes
41 00 6C 00 69 00 63 00 65 00  # "Alice" UTF-16LE

# ROW 2: (2, "Bob")
D1                      # Token
04                      # ID length
02 00 00 00             # ID = 2
06 00                   # Name length = 6 bytes
42 00 6F 00 62 00       # "Bob" UTF-16LE

# DONE
FD                      # Token
10 00                   # Status = DONE_COUNT
C3 00                   # CurCmd = INSERT
02 00 00 00 00 00 00 00 # RowCount = 2
```
