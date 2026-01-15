# Implementation Plan: Streaming SELECT and Query Cancellation

**Branch**: `004-streaming-select-cancel` | **Date**: 2026-01-15 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/004-streaming-select-cancel/spec.md`

## Summary

Implement query execution and result streaming from SQL Server to DuckDB:
- SQL_BATCH TDS packet construction with UTF-16LE encoding
- TDS token parsing (COLMETADATA, ROW, DONE*, ERROR, INFO)
- Streaming results into DuckDB DataChunks
- Query cancellation via Attention packet during execution and streaming
- Integration tests with 10M+ row datasets

This feature replaces the stub `mssql_scan` implementation with real TDS query execution.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB main branch (extension API, DataChunk), existing TDS layer from spec 003
**Storage**: In-memory (result streaming, no intermediate buffering)
**Testing**: DuckDB SQL tests + integration tests with SQL Server (10M+ rows)
**Target Platform**: Linux, macOS, Windows (cross-platform)
**Project Type**: Single project (DuckDB extension)
**Performance Goals**: 10M rows streamed in <60s on local network, cancellation <2s
**Constraints**: No result buffering (bounded memory), connection reusable after cancel
**Scale/Scope**: Queries returning arbitrarily large result sets

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | ✅ PASS | Native TDS SQL_BATCH, no external libraries |
| II. Streaming First | ✅ PASS | Results stream directly into DataChunks, no buffering |
| III. Correctness over Convenience | ✅ PASS | Unsupported types fail explicitly, errors propagated |
| IV. Explicit State Machines | ✅ PASS | Connection state transitions: idle→executing→idle/cancelling |
| V. DuckDB-Native UX | ✅ PASS | Uses existing `mssql_scan` function, standard warnings |
| VI. Incremental Delivery | ✅ PASS | Read-only SELECT; writes deferred to future spec |

**Post-Design Re-check**: All principles satisfied. Design streams results directly from TDS packets to DuckDB vectors without intermediate copies.

## Project Structure

### Documentation (this feature)

```text
specs/004-streaming-select-cancel/
├── plan.md              # This file
├── spec.md              # Feature specification
├── research.md          # TDS tokens, type mapping, DuckDB streaming API
├── data-model.md        # Entities: SqlBatch, ColumnMetadata, QueryResult, etc.
├── quickstart.md        # Usage guide with examples
├── contracts/
│   ├── token-parsing.md         # TDS token formats and parsing
│   └── type-mapping.md          # SQL Server → DuckDB type conversions
├── checklists/
│   └── requirements.md  # Specification quality checklist
└── tasks.md             # (Created by /speckit.tasks)
```

### Source Code (repository root)

```text
src/
├── include/
│   ├── tds/                        # TDS headers (extend existing)
│   │   ├── tds_types.hpp           # (existing) Add SQL Server type IDs
│   │   ├── tds_packet.hpp          # (existing) Already has helpers
│   │   ├── tds_connection.hpp      # (modify) Add ExecuteBatch, Cancel
│   │   ├── tds_token_parser.hpp    # NEW: Token stream parser
│   │   ├── tds_column_metadata.hpp # NEW: Column definitions
│   │   └── tds_row_reader.hpp      # NEW: Row data extraction
│   │
│   ├── encoding/                   # Encoding headers (extend existing)
│   │   ├── utf16.hpp               # (existing) UTF-16LE conversion
│   │   ├── type_converter.hpp      # NEW: SQL Server → DuckDB types
│   │   ├── decimal_encoding.hpp    # NEW: DECIMAL/NUMERIC wire format
│   │   ├── datetime_encoding.hpp   # NEW: DATE/TIME/DATETIME wire formats
│   │   └── guid_encoding.hpp       # NEW: UNIQUEIDENTIFIER wire format
│   │
│   └── query/                      # Query execution headers
│       ├── mssql_query_executor.hpp    # NEW: Execute query, yield DataChunks
│       └── mssql_result_stream.hpp     # NEW: Streaming result iterator
│
├── tds/                            # TDS implementation (extend existing)
│   ├── tds_types.cpp               # (modify) Add type ID constants
│   ├── tds_connection.cpp          # (modify) Add ExecuteBatch, SendAttention
│   ├── tds_token_parser.cpp        # NEW: Parse token stream
│   ├── tds_column_metadata.cpp     # NEW: Parse COLMETADATA
│   └── tds_row_reader.cpp          # NEW: Parse ROW tokens
│
├── encoding/                       # Encoding implementation (extend existing)
│   ├── utf16.cpp                   # (existing) UTF-16LE conversion
│   ├── type_converter.cpp          # NEW: Type mapping and conversion dispatch
│   ├── decimal_encoding.cpp        # NEW: DECIMAL/NUMERIC encoding
│   ├── datetime_encoding.cpp       # NEW: Date/time encoding
│   └── guid_encoding.cpp           # NEW: GUID byte reordering
│
├── query/                          # Query execution (DuckDB integration)
│   ├── mssql_query_executor.cpp    # NEW: Bind to pool, execute, stream
│   └── mssql_result_stream.cpp     # NEW: DataChunk generation
│
├── mssql_functions.cpp             # (modify) Replace stub with real impl
└── mssql_extension.cpp             # (no change expected)

test/
├── sql/
│   └── query/
│       ├── basic_select.test       # Simple SELECT queries
│       ├── type_mapping.test       # Data type conversion tests
│       ├── error_handling.test     # SQL Server error propagation
│       ├── cancellation.test       # Query cancellation (mocked)
│       └── info_messages.test      # INFO token handling
└── integration/
    └── sqlserver/
        ├── streaming_10m.test      # 10M row streaming benchmark
        ├── cancel_execution.test   # Cancel during server execution
        ├── cancel_streaming.test   # Cancel during row streaming
        └── pool_integration.test   # Connection reuse verification
```

**Structure Decision**: Extends the existing TDS layer with token parsing and row reading. Type encoding/conversion logic goes in `src/encoding/` alongside the existing UTF-16 converter, keeping encoding concerns separate from protocol handling. New `query/` directory for DuckDB-specific query execution logic.

## Complexity Tracking

No constitution violations requiring justification.

## Key Design Decisions

### 1. SQL_BATCH Packet Construction

- SQL text encoded as UTF-16LE using existing `TdsPacket::AppendUTF16LE()`
- Large queries split into multiple packets with continuation status
- Connection state transitions: `idle` → `executing` before send

### 2. TDS Token Parsing (Response Stream)

Token types to parse:

| Token | Hex | Purpose |
|-------|-----|---------|
| COLMETADATA | 0x81 | Column definitions (once per result set) |
| ROW | 0xD1 | Individual data row |
| DONE | 0xFD | Query complete |
| DONEPROC | 0xFE | Stored proc complete |
| DONEINPROC | 0xFF | Intermediate complete |
| ERROR | 0xAA | SQL Server error |
| INFO | 0xAB | Informational message |
| ENVCHANGE | 0xE3 | Environment change (consume) |
| LOGINACK | 0xAD | (already handled in auth) |

Parsing strategy:
- Incremental buffer-based parsing (handle token spanning packets)
- Single-pass token stream (no backtracking)
- Error accumulation for batch errors

### 3. Type Mapping (SQL Server → DuckDB)

| SQL Server Type | TDS Type ID | DuckDB Type |
|-----------------|-------------|-------------|
| INT | 0x38 | INTEGER |
| BIGINT | 0x7F | BIGINT |
| SMALLINT | 0x34 | SMALLINT |
| TINYINT | 0x30 | TINYINT |
| BIT | 0x32 | BOOLEAN |
| FLOAT | 0x3E | DOUBLE |
| REAL | 0x3B | FLOAT |
| DECIMAL/NUMERIC | 0x6A/0x6C | DECIMAL(p,s) |
| MONEY | 0x3C | DECIMAL(19,4) |
| SMALLMONEY | 0x7A | DECIMAL(10,4) |
| CHAR/VARCHAR | 0xAF/0xA7 | VARCHAR |
| NCHAR/NVARCHAR | 0xEF/0xE7 | VARCHAR (UTF-8) |
| DATETIME | 0x3D | TIMESTAMP |
| DATETIME2 | 0x2A | TIMESTAMP |
| DATE | 0x28 | DATE |
| TIME | 0x29 | TIME |
| UNIQUEIDENTIFIER | 0x24 | UUID |
| VARBINARY/BINARY | 0xA5/0xAD | BLOB |

Unsupported types (fail with clear error):
- XML, GEOGRAPHY, GEOMETRY, SQL_VARIANT, HIERARCHYID, IMAGE, TEXT, NTEXT

### 4. Result Streaming Architecture

```
TDS Packets → TokenParser → RowReader → DataChunk Builder → DuckDB
     │              │            │              │
     │              │            │              └─ Vector at a time (2048 rows)
     │              │            └─ Extract values per column
     │              └─ Yield tokens (COLMETADATA, ROW, DONE, ERROR)
     └─ Read from socket, handle packet boundaries
```

Key invariants:
- Memory bounded by DataChunk size (2048 rows × column count)
- No full result buffering
- Rows flow directly into Vector data arrays

### 5. Query Cancellation

Two phases:
1. **During execution** (before COLMETADATA): Send Attention, wait for DONE with ATTN flag
2. **During streaming** (after ROW tokens): Send Attention, drain remaining tokens until DONE+ATTN

Post-cancellation:
- Connection returns to `idle` state
- Connection remains in pool (reusable)
- If cancel timeout: close connection, remove from pool

### 6. Error Handling

- ERROR tokens accumulated during result processing
- Thrown as DuckDB exception after result stream completes (or immediately for fatal errors)
- Severity levels: 0-10 info, 11-16 user error, 17-19 server error, 20+ fatal

### 7. DuckDB Integration

- Replace stub `MSSQLScanFunction` with real implementation
- Use `ClientContext` for warning propagation (`context.AddWarning()`)
- Use DuckDB's debug logging via `enable_logging(level = 'debug')`

## Dependencies on Prior Specs

- **Spec 002 (DuckDB Surface API)**: `mssql_scan` function signature, `MSSQLContextManager`
- **Spec 003 (TDS Connection Pooling)**: `ConnectionPool`, `TdsConnection`, connection state machine

## Artifacts Generated

| Artifact | Path | Description |
|----------|------|-------------|
| Research | `research.md` | TDS token formats, DuckDB DataChunk API, streaming patterns |
| Data Model | `data-model.md` | Entities: SqlBatch, ColumnMetadata, QueryResult, TokenParser |
| Quickstart | `quickstart.md` | Usage guide with query examples |
| Token Parsing Contract | `contracts/token-parsing.md` | TDS token byte layouts |
| Type Mapping Contract | `contracts/type-mapping.md` | SQL Server ↔ DuckDB type conversions |

## Next Steps

Run `/speckit.tasks` to generate implementation tasks based on this plan.
