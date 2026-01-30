# Research: COPY TO MSSQL via TDS BulkLoadBCP

**Feature**: 024-mssql-copy-bcp
**Date**: 2026-01-29

## 1. DuckDB CopyFunction API

### Decision
Implement COPY TO using DuckDB's `CopyFunction` sink API with format name `'bcp'`.

### Rationale
- DuckDB provides a well-defined extension point for custom COPY formats
- The sink pattern (Bind → InitGlobal → InitLocal → Sink → Combine → Finalize) matches our streaming requirements
- Format registration via `ExtensionLoader::RegisterFunction()` integrates with DuckDB's query planner

### Key Findings

**Required Callbacks:**
```cpp
CopyFunction function("bcp");
function.copy_to_bind = BCPCopyBind;
function.copy_to_initialize_global = BCPCopyInitGlobal;
function.copy_to_initialize_local = BCPCopyInitLocal;
function.copy_to_sink = BCPCopySink;
function.copy_to_combine = BCPCopyCombine;
function.copy_to_finalize = BCPCopyFinalize;
function.execution_mode = BCPCopyExecutionMode;
```

**Execution Mode**: `REGULAR_COPY_TO_FILE` (sequential, single connection)

**State Management:**
- `GlobalFunctionData` → holds pinned TdsConnection, row count, BCP state
- `LocalFunctionData` → per-thread buffer for accumulating rows before batched send

### Alternatives Considered
1. **TableFunction (sink)**: More complex, not designed for COPY syntax
2. **Custom statement parser**: Incompatible with DuckDB's query engine
3. **mssql_exec wrapper**: Would require SQL string generation, hitting size limits

---

## 2. TDS BulkLoadBCP Protocol

### Decision
Implement TDS packet type `0x07` (BULK_LOAD) with COLMETADATA, ROW, and DONE tokens per MS-TDS specification.

### Rationale
- Native TDS protocol avoids SQL string size limits (spec requirement FR-003)
- Streaming protocol matches memory-bounded requirement (FR-008)
- Same protocol SQL Server's bcp.exe and SqlBulkCopy use

### Key Findings

**Protocol Sequence:**
1. Send `INSERT BULK [table] (col1 type1, ...) WITH (KEEP_NULLS)` via SQL_BATCH (0x01)
2. Server responds with DONE token (success)
3. Send BULK_LOAD packets (0x07) containing:
   - COLMETADATA token (0x81): column definitions
   - ROW tokens (0xD1): row data
   - DONE token (0xFD): completion signal
4. Server responds with final DONE (row count)

**COLMETADATA Structure:**
```
0x81 [count:2] [column_data...]
  column_data = [usertype:4][flags:2][TYPE_INFO][colname:B_VARCHAR]
```

**ROW Structure:**
```
0xD1 [col1_data][col2_data]...
  Fixed types: raw bytes
  Variable types: [length:1-2][data]
  NULL: length=0 (BYTELEN) or 0xFFFF (USHORTLEN)
```

**DONE Structure:**
```
0xFD [status:2][curcmd:2][rowcount:8]
  status = 0x10 (DONE_COUNT)
  curcmd = 0x00C3 (INSERT)
```

### Alternatives Considered
1. **Batched INSERT VALUES**: Current approach, hits SQL size limits
2. **TVP (Table-Valued Parameters)**: More complex, less direct
3. **OPENROWSET BULK**: Requires file on SQL Server, not applicable

---

## 3. Type Encoding (Binary Wire Format)

### Decision
Reuse existing `src/tds/encoding/` infrastructure for binary encoding, extending as needed for BCP row tokens.

### Rationale
- Existing decimal, datetime, GUID encoding is correct
- UTF-16LE encoding already implemented
- Consistent with codebase architecture

### Key Findings

**DuckDB → TDS Type Mapping (for BCP):**

| DuckDB Type | TDS Token | Wire Format |
|-------------|-----------|-------------|
| BOOLEAN | BITNTYPE (0x68) | 1 byte: 0x00/0x01 |
| TINYINT | INTNTYPE (0x26) | 1 byte |
| SMALLINT | INTNTYPE (0x26) | 2 bytes LE |
| INTEGER | INTNTYPE (0x26) | 4 bytes LE |
| BIGINT | INTNTYPE (0x26) | 8 bytes LE |
| FLOAT | FLTNTYPE (0x6D) | 4 bytes IEEE 754 |
| DOUBLE | FLTNTYPE (0x6D) | 8 bytes IEEE 754 |
| DECIMAL(p,s) | DECIMALNTYPE (0x6A) | 1+4/8/12/16 bytes |
| VARCHAR | NVARCHARTYPE (0xE7) | 2-byte len + UTF-16LE |
| UUID | GUIDTYPE (0x24) | 16 bytes mixed-endian |
| BLOB | BIGVARBINARYTYPE (0xA5) | 2-byte len + data |
| DATE | DATENTYPE (0x28) | 3 bytes |
| TIME | TIMENTYPE (0x29) | 3-5 bytes (scale-dependent) |
| TIMESTAMP | DATETIME2NTYPE (0x2A) | 6-8 bytes |
| TIMESTAMP_TZ | DATETIMEOFFSETNTYPE (0x2B) | 8-10 bytes |

**Nullable Handling:**
- BYTELEN types (int, guid, etc.): length=0 for NULL
- USHORTLEN types (varchar, binary): length=0xFFFF for NULL

### Alternatives Considered
1. **SQL literal serialization**: Already exists but hits string limits
2. **ODBC type encoding**: Would require ODBC dependency (prohibited by constitution)

---

## 4. Connection & Transaction Integration

### Decision
Use existing `ConnectionProvider` for pinned connection management; integrate with `MSSQLTransaction` state.

### Rationale
- Connection pinning already implemented for DML operations
- Transaction descriptor management handles BEGIN TRAN/COMMIT/ROLLBACK
- Temp table visibility requires same-session guarantee

### Key Findings

**Connection Acquisition:**
```cpp
auto conn = ConnectionProvider::GetConnection(context, catalog);
// BCP operations...
ConnectionProvider::ReleaseConnection(context, catalog, conn);
```

**Transaction Check:**
```cpp
if (ConnectionProvider::IsInTransaction(context, catalog)) {
    // Check for mssql_scan in source (prohibited)
    // Use existing pinned connection
}
```

**Executing State Guard (R01a):**
- TdsConnection has state machine: Idle → Executing → Idle
- COPY MUST verify connection is Idle before starting
- If Executing (prior scan), throw: "cannot start COPY/BCP on a connection that is currently streaming results"

### Alternatives Considered
1. **New connection per COPY**: Would break temp table visibility
2. **Separate transaction layer**: Duplicates existing logic unnecessarily

---

## 5. Target Resolution

### Decision
Support two target syntaxes with unified resolution to (catalog, schema, table, is_temp).

### Rationale
- URL syntax provides explicit attach context
- Catalog syntax integrates with DuckDB's standard resolution
- Both must resolve to same underlying execution path

### Key Findings

**URL Syntax Parsing:**
```
mssql://<attach_alias>/<schema>/<table>
mssql://<attach_alias>/#temp_name
mssql://<attach_alias>/##global_temp
```

**Catalog Syntax:**
```
catalog.schema.table → resolved via DuckDB binder
```

**Table Existence Check (on pinned connection):**
```sql
-- Regular table
SELECT OBJECT_ID(N'[schema].[table]', N'U')
-- Temp table
SELECT OBJECT_ID('tempdb..#temp_name')
```

**View Detection:**
```sql
SELECT OBJECTPROPERTY(OBJECT_ID(N'[schema].[table]'), 'IsView')
```

### Alternatives Considered
1. **Catalog-only resolution**: Would miss temp tables (not in catalog)
2. **URL-only syntax**: Less ergonomic for catalog users

---

## 6. Codebase Integration Points

### Decision
Create new `src/copy/` directory following existing `src/dml/insert/` patterns.

### Rationale
- Parallel structure to existing DML implementation
- Clear separation of concerns
- Consistent with project conventions

### Key Files to Create

**Headers (`src/include/copy/`):**
- `mssql_copy_function.hpp` - CopyFunction registration
- `mssql_bcp_writer.hpp` - BulkLoadBCP packet builder
- `mssql_bcp_config.hpp` - Configuration struct
- `mssql_target_resolver.hpp` - URL/catalog target parsing

**Implementation (`src/copy/`):**
- `mssql_copy_function.cpp` - CopyFunction callbacks
- `mssql_bcp_writer.cpp` - COLMETADATA/ROW/DONE encoding
- `mssql_bcp_config.cpp` - Settings integration
- `mssql_target_resolver.cpp` - Target resolution logic

**TDS Layer (`src/tds/`):**
- Add `BuildBulkLoadPacket()` to `tds_protocol.hpp/cpp`
- Add ROW token encoder to `tds/encoding/`

**Extension Entry (`src/mssql_extension.cpp`):**
- Add `RegisterMSSQLCopyFunctions()` call in `LoadInternal()`

### Alternatives Considered
1. **Inline in mssql_extension.cpp**: Would become too large
2. **Under dml/**: COPY is distinct from INSERT/UPDATE/DELETE semantics

---

## 7. Progress Reporting & Debug Output

### Decision
Integrate with DuckDB's progress mechanism and existing MSSQL_DEBUG levels.

### Rationale
- FR-015 requires progress reporting
- FR-016 requires MSSQL_DEBUG integration
- Consistent with existing extension behavior

### Key Findings

**Progress Reporting:**
- DuckDB's `ClientContext` provides `SetProgress()` method
- Call periodically during Sink with (rows_sent / estimated_total)
- Estimation from source if available, otherwise incremental

**Debug Levels:**
- Level 1: COPY start/end with table name and total rows
- Level 2: Batch-level details (batch #, row count, bytes)
- Level 3: Packet-level trace (COLMETADATA, ROW tokens)

### Implementation
```cpp
if (MSSQL_DEBUG >= 1) {
    fprintf(stderr, "[MSSQL COPY] Starting BCP to [%s].[%s]\n",
            schema.c_str(), table.c_str());
}
```

---

## Summary

All NEEDS CLARIFICATION items resolved. Key decisions:

| Area | Decision |
|------|----------|
| DuckDB API | CopyFunction with format `'bcp'` |
| Protocol | TDS BULK_LOAD (0x07) with COLMETADATA/ROW/DONE |
| Type encoding | Binary wire format per MS-TDS spec |
| Connection | Existing ConnectionProvider with pinning |
| Target | Unified URL + catalog resolution |
| Structure | New `src/copy/` directory |
| Progress | DuckDB progress + MSSQL_DEBUG levels |
