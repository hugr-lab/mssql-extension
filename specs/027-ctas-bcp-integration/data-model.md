# Data Model: CTAS BCP Integration

**Feature**: 027-ctas-bcp-integration
**Date**: 2026-02-02

## Overview

This feature extends existing data structures rather than creating new entities. The primary changes are configuration extensions and execution state modifications.

---

## Entity Modifications

### CTASConfig (Extended)

**Location**: `src/include/dml/ctas/mssql_ctas_config.hpp`

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `batch_size` | idx_t | 1000 | Rows per batch (used by INSERT mode) |
| `max_rows_per_statement` | idx_t | 1000 | Max rows per INSERT |
| `max_sql_bytes` | idx_t | 8MB | INSERT SQL size limit |
| `text_type` | TextType | NVARCHAR | Text column type |
| `drop_on_failure` | bool | false | Cleanup on error |
| **`use_bcp`** | bool | **true** | **NEW: Use BCP protocol for data transfer** |
| **`bcp_flush_rows`** | idx_t | 100000 | **NEW: Rows before BCP flush (from mssql_copy_flush_rows)** |
| **`bcp_tablock`** | bool | false | **NEW: Use TABLOCK hint (from mssql_copy_tablock)** |

### CTASExecutionState (Extended)

**Location**: `src/include/dml/ctas/mssql_ctas_executor.hpp`

| Field | Type | Description |
|-------|------|-------------|
| `catalog` | MSSQLCatalog* | Target catalog reference |
| `target` | CTASTarget | Schema/table names |
| `columns` | vector<CTASColumnDef> | Column definitions |
| `config` | CTASConfig | Execution configuration |
| `phase` | CTASPhase | Current execution phase |
| `ddl_sql` | string | Generated CREATE TABLE |
| `insert_executor` | unique_ptr<MSSQLInsertExecutor> | INSERT mode executor |
| **`bcp_writer`** | unique_ptr<BCPWriter> | **NEW: BCP mode writer** |
| **`bcp_columns`** | vector<BCPColumnMetadata> | **NEW: BCP column metadata** |
| **`bcp_rows_in_batch`** | idx_t | **NEW: Rows accumulated** |

### BCPConfig (Modified Default)

**Location**: `src/include/copy/bcp_config.hpp`

| Field | Type | Old Default | New Default |
|-------|------|-------------|-------------|
| `tablock` | bool | **true** | **false** |

---

## State Transitions

### CTASPhase Enum (Unchanged)

```
PENDING → DDL_EXECUTING → DDL_DONE → INSERT_EXECUTING → COMPLETED
                                   └→ BCP_EXECUTING → COMPLETED
```

**Change**: After DDL_DONE, the next phase depends on `config.use_bcp`:
- `use_bcp = true` → BCP_EXECUTING (new path)
- `use_bcp = false` → INSERT_EXECUTING (existing path)

### BCP Data Flow

```
AddChunk(DataChunk)
    ↓
BCPRowEncoder::Encode(row) → buffer
    ↓
BCPWriter::WriteRow(buffer)
    ↓
[accumulate until bcp_flush_rows]
    ↓
BCPWriter::FlushBatch() → TDS BulkLoadBCP packet
    ↓
Server processes batch
    ↓
Reset batch, continue
```

---

## Settings Mapping

### New Setting

| Setting | Type | Default | Scope | Description |
|---------|------|---------|-------|-------------|
| `mssql_ctas_use_bcp` | BOOLEAN | true | GLOBAL | Use BCP for CTAS data transfer |

### Modified Setting

| Setting | Type | Old Default | New Default | Description |
|---------|------|-------------|-------------|-------------|
| `mssql_copy_tablock` | BOOLEAN | true | **false** | Use TABLOCK hint |

### Inherited Settings (for BCP mode)

| Setting | Used For |
|---------|----------|
| `mssql_copy_flush_rows` | Rows before flush to server |
| `mssql_copy_tablock` | Table-level lock hint |

---

## Type Mapping

CTAS with BCP uses the same type mapping as COPY TO via TargetResolver:

| DuckDB Type | SQL Server Type | TDS Token | Wire Format |
|-------------|-----------------|-----------|-------------|
| BOOLEAN | bit | 0x68 | 1 byte |
| TINYINT | tinyint | 0x30 | 1 byte |
| SMALLINT | smallint | 0x34 | 2 bytes |
| INTEGER | int | 0x38 | 4 bytes |
| BIGINT | bigint | 0x7F | 8 bytes |
| FLOAT | real | 0x3B | 4 bytes |
| DOUBLE | float | 0x3E | 8 bytes |
| DECIMAL(p,s) | decimal(p,s) | 0x6A | Variable |
| VARCHAR | nvarchar(max) | 0xE7 | PLP UTF-16LE |
| BLOB | varbinary(max) | 0xA5 | PLP binary |
| DATE | date | 0x28 | 3 bytes |
| TIME | time(7) | 0x29 | 5 bytes |
| TIMESTAMP | datetime2(7) | 0x2A | 8 bytes |
| UUID | uniqueidentifier | 0x24 | 16 bytes |

---

## Validation Rules

1. **BCP mode requires valid DDL phase** - Table must be created before BCP starts
2. **Type compatibility** - All source column types must have valid TDS token mappings
3. **Connection state** - Connection must be in Idle state before starting BCP
4. **Flush threshold** - bcp_flush_rows must be > 0
5. **TABLOCK applicability** - TABLOCK only applies to non-temp tables
