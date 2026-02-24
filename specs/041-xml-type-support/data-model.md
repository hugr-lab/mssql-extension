# Data Model: XML Data Type Support

**Feature**: 041-xml-type-support
**Date**: 2026-02-24

## Type Mapping

| SQL Server Type | TDS Type ID | Wire Encoding | DuckDB Type | Notes |
|-----------------|-------------|---------------|-------------|-------|
| `xml` | 0xF1 | PLP + UTF-16LE | VARCHAR | No XML parsing; raw text |

## Wire Format

### COLMETADATA Token (Reading)

```
[UserType: 4 bytes = 0x00000000]
[Flags: 2 bytes]
[TypeId: 1 byte = 0xF1]
// No additional metadata (no collation, no length, no precision)
[ColName: B_VARCHAR]
```

**Key**: XML has NO extra metadata after the type ID — unlike NVARCHAR which has 2-byte length + 5-byte collation.

### Row Data (Reading)

PLP format (same as NVARCHAR(MAX)):

```
[TotalLength: 8 bytes LE]     // 0xFFFFFFFFFFFFFFFE = NULL, 0xFFFFFFFFFFFFFFFF = unknown
[ChunkLength: 4 bytes LE]     // 0 = terminator
[ChunkData: ChunkLength bytes] // UTF-16LE encoded XML
[ChunkLength: 4 bytes LE]     // next chunk or 0 terminator
...
[Terminator: 4 bytes = 0x00000000]
```

### BCP COLMETADATA Token (Writing)

```
[UserType: 4 bytes = 0x00000000]
[Flags: 2 bytes]
[TypeId: 1 byte = 0xF1]
// No additional metadata
[ColName: B_VARCHAR]
```

### BCP Row Data (Writing)

Same PLP format as reading, with UTF-16LE encoded data:

```
[UNKNOWN_PLP_LEN: 8 bytes = 0xFFFFFFFFFFFFFFFE]
[ChunkLength: 4 bytes LE]
[ChunkData: UTF-16LE encoded string]
[Terminator: 4 bytes = 0x00000000]
```

## Internal Column Representation

### MSSQLColumnInfo (Catalog)

| Field | Value for XML |
|-------|---------------|
| `sql_type_name` | `"xml"` |
| `duckdb_type` | `LogicalType::VARCHAR` |
| `max_length` | `-1` (from sys.columns, indicates MAX) |
| `precision` | `0` |
| `scale` | `0` |
| `collation_name` | `""` (empty) |

### TDS ColumnMetadata (Protocol)

| Field | Value for XML |
|-------|---------------|
| `type_id` | `0xF1` |
| `max_length` | `0xFFFF` (set during parse, PLP indicator) |
| `collation` | Not present in wire format |
| `IsPLPType()` | `true` (because max_length == 0xFFFF) |

## State Transitions

No new state transitions. XML type support uses existing:
- Connection state machine (Idle → Executing → Idle)
- PLP reading state (reading chunks until terminator)
- BCP protocol state (COLMETADATA → ROW tokens → DONE)
