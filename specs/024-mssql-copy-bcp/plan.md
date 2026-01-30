# Implementation Plan: COPY TO MSSQL via TDS BulkLoadBCP

**Branch**: `024-mssql-copy-bcp` | **Date**: 2026-01-29 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/024-mssql-copy-bcp/spec.md`

## Summary

Implement high-throughput COPY TO support from DuckDB to SQL Server using TDS BulkLoadBCP (packet type 0x07). The feature registers a new CopyFunction with format `'bcp'` that streams DataChunks directly to SQL Server via binary BCP protocol, avoiding SQL string size limits. Supports both URL (`mssql://...`) and catalog (`catalog.schema.table`) target syntax with CREATE_TABLE and OVERWRITE options.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB (main branch), OpenSSL (vcpkg), existing TDS protocol layer
**Storage**: SQL Server 2019+ (remote target)
**Testing**: SQLLogicTest + C++ unit tests (Catch2 via DuckDB)
**Target Platform**: Linux (GCC), macOS (Clang), Windows (MSVC, MinGW)
**Project Type**: DuckDB extension (single project)
**Performance Goals**: 50K rows/second minimum throughput (simple types, LAN)
**Constraints**: Bounded memory (batch-based streaming), no full dataset buffering
**Scale/Scope**: 10M+ rows per COPY operation

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | PASS | Uses native TDS implementation, no ODBC/FreeTDS |
| II. Streaming First | PASS | DataChunks streamed directly to BCP packets, bounded batches |
| III. Correctness over Convenience | PASS | Explicit errors for unsupported operations (views, scan-in-txn) |
| IV. Explicit State Machines | PASS | Connection state guards (Idle required for BCP start) |
| V. DuckDB-Native UX | PASS | Standard COPY TO syntax, catalog target resolution |
| VI. Incremental Delivery | PASS | MVP focuses on write path, temp tables are P1 priority |

**Gate Result**: PASS - All constitution principles satisfied.

## Project Structure

### Documentation (this feature)

```text
specs/024-mssql-copy-bcp/
├── plan.md              # This file
├── spec.md              # Feature specification
├── research.md          # Phase 0: Protocol and API research
├── data-model.md        # Phase 1: Entity definitions
├── quickstart.md        # Phase 1: Usage guide
├── contracts/           # Phase 1: Wire format and syntax specs
│   ├── copy-syntax.md
│   └── bcp-wire-format.md
├── checklists/
│   └── requirements.md  # Spec quality checklist
└── tasks.md             # Phase 2: Implementation tasks (via /speckit.tasks)
```

### Source Code (repository root)

```text
src/
├── include/
│   ├── copy/                         # NEW: COPY function headers
│   │   ├── mssql_copy_function.hpp   # CopyFunction registration
│   │   ├── mssql_bcp_writer.hpp      # BulkLoadBCP packet builder
│   │   ├── mssql_bcp_config.hpp      # Configuration struct
│   │   └── mssql_target_resolver.hpp # URL/catalog target parsing
│   └── tds/
│       ├── tds_protocol.hpp          # EXTEND: Add BuildBulkLoadPacket()
│       └── encoding/
│           └── bcp_row_encoder.hpp   # NEW: Binary row encoding
├── copy/                             # NEW: COPY function implementation
│   ├── mssql_copy_function.cpp       # CopyFunction callbacks
│   ├── mssql_bcp_writer.cpp          # COLMETADATA/ROW/DONE encoding
│   ├── mssql_bcp_config.cpp          # Settings integration
│   └── mssql_target_resolver.cpp     # Target resolution logic
├── tds/
│   ├── tds_protocol.cpp              # EXTEND: BuildBulkLoadPacket()
│   └── encoding/
│       └── bcp_row_encoder.cpp       # NEW: Binary type encoding
└── mssql_extension.cpp               # EXTEND: RegisterMSSQLCopyFunctions()

test/
├── sql/
│   └── copy/                         # NEW: COPY integration tests
│       ├── copy_basic.test           # Regular table COPY
│       ├── copy_temp.test            # Temp table COPY
│       ├── copy_create_overwrite.test # CREATE_TABLE/OVERWRITE
│       ├── copy_catalog_target.test  # Catalog syntax
│       ├── copy_types.test           # Type mapping validation
│       ├── copy_errors.test          # Error conditions
│       └── copy_large.test           # Large-scale (10M rows)
└── cpp/
    ├── test_bcp_writer.cpp           # NEW: BCP packet encoding unit tests
    ├── test_target_resolver.cpp      # NEW: URL/catalog parsing tests
    └── test_bcp_row_encoder.cpp      # NEW: Type encoding tests
```

**Structure Decision**: Single project structure following existing DML patterns (`src/dml/insert/`). New `src/copy/` directory mirrors layout. Headers in `src/include/copy/`, implementations in `src/copy/`.

## Key Components

### 1. CopyFunction Registration (`mssql_copy_function.hpp/cpp`)

```cpp
namespace duckdb {
// In duckdb namespace - use MSSQL prefix
void RegisterMSSQLCopyFunctions(DatabaseInstance &db);

struct MSSQLCopyBindData : public TableFunctionData {
    BCPCopyTarget target;
    BCPCopyConfig config;
    vector<LogicalType> source_types;
    vector<string> source_names;
};

struct MSSQLCopyGlobalState : public GlobalFunctionData {
    shared_ptr<tds::TdsConnection> connection;
    unique_ptr<BCPWriter> writer;
    atomic<idx_t> rows_sent;
    mutex write_lock;
};

struct MSSQLCopyLocalState : public LocalFunctionData {
    ColumnDataCollection buffer;
    AppendState append_state;
};
} // namespace duckdb
```

### 2. BCP Writer (`mssql_bcp_writer.hpp/cpp`)

```cpp
namespace duckdb::mssql {
// In duckdb::mssql namespace - no prefix needed
class BCPWriter {
public:
    BCPWriter(tds::TdsConnection &conn, const BCPCopyTarget &target,
              const vector<BCPColumnMetadata> &columns);

    void WriteColmetadata();
    void WriteRows(DataChunk &chunk);
    void WriteDone(idx_t row_count);
    void Finalize();

private:
    tds::TdsConnection &conn_;
    BCPCopyTarget target_;
    vector<BCPColumnMetadata> columns_;
    bool colmetadata_sent_ = false;
};
} // namespace duckdb::mssql
```

### 3. Target Resolver (`mssql_target_resolver.hpp/cpp`)

```cpp
namespace duckdb::mssql {
struct TargetResolver {
    static BCPCopyTarget ResolveURL(ClientContext &context, const string &url);
    static BCPCopyTarget ResolveCatalog(ClientContext &context,
                                        const string &catalog,
                                        const string &schema,
                                        const string &table);
    static void ValidateTarget(ClientContext &context,
                               tds::TdsConnection &conn,
                               BCPCopyTarget &target,
                               const BCPCopyConfig &config);
};
} // namespace duckdb::mssql
```

### 4. BCP Row Encoder (`bcp_row_encoder.hpp/cpp`)

```cpp
namespace duckdb::tds::encoding {
// In encoding namespace - no prefix needed
class BCPRowEncoder {
public:
    static void EncodeRow(vector<uint8_t> &buffer,
                          DataChunk &chunk,
                          idx_t row_idx,
                          const vector<BCPColumnMetadata> &columns);

    static void EncodeValue(vector<uint8_t> &buffer,
                            const Value &value,
                            const BCPColumnMetadata &col);
private:
    static void EncodeInt(vector<uint8_t> &buffer, int64_t value, uint8_t size);
    static void EncodeDecimal(vector<uint8_t> &buffer, const hugeint_t &value,
                               uint8_t precision, uint8_t scale);
    static void EncodeNVarchar(vector<uint8_t> &buffer, const string_t &value);
    static void EncodeGUID(vector<uint8_t> &buffer, const hugeint_t &uuid);
    static void EncodeDatetime2(vector<uint8_t> &buffer, timestamp_t ts);
    // ... other type encoders
};
} // namespace duckdb::tds::encoding
```

## Execution Flow

```
COPY (SELECT ...) TO 'mssql://db/dbo/table' (FORMAT 'bcp')
         │
         ▼
┌─────────────────────────────────────────────────────────┐
│ 1. BCPCopyBind()                                        │
│    - Parse target URL or resolve catalog                │
│    - Parse options (CREATE_TABLE, OVERWRITE)            │
│    - Capture source types/names                         │
│    - Return MSSQLCopyBindData                           │
└─────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────┐
│ 2. BCPCopyInitGlobal()                                  │
│    - Get pinned connection from ConnectionProvider      │
│    - Check connection not Executing (R01a)              │
│    - Check table existence via OBJECT_ID()              │
│    - CREATE/DROP TABLE if needed                        │
│    - Send INSERT BULK statement                         │
│    - Create BCPWriter, send COLMETADATA                 │
│    - Return MSSQLCopyGlobalState                        │
└─────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────┐
│ 3. BCPCopyInitLocal() [per thread]                      │
│    - Create local ColumnDataCollection buffer           │
│    - Return MSSQLCopyLocalState                         │
└─────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────┐
│ 4. BCPCopySink() [repeated per chunk]                   │
│    - Append chunk to local buffer                       │
│    - If buffer >= batch_rows:                           │
│      - Lock write_mutex                                 │
│      - Encode rows to BCP ROW tokens                    │
│      - Send to SQL Server                               │
│      - Update rows_sent, report progress                │
│      - Unlock                                           │
└─────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────┐
│ 5. BCPCopyCombine()                                     │
│    - Flush any remaining local buffer rows              │
└─────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────┐
│ 6. BCPCopyFinalize()                                    │
│    - Send DONE token                                    │
│    - Read server response (row count)                   │
│    - Release connection (if not in transaction)         │
│    - Return total rows_sent                             │
└─────────────────────────────────────────────────────────┘
```

## Complexity Tracking

No constitution violations. All patterns align with existing extension architecture.

## Related Documents

- [spec.md](./spec.md) - Feature specification
- [research.md](./research.md) - Protocol and API research
- [data-model.md](./data-model.md) - Entity definitions
- [quickstart.md](./quickstart.md) - Usage guide
- [contracts/copy-syntax.md](./contracts/copy-syntax.md) - SQL syntax specification
- [contracts/bcp-wire-format.md](./contracts/bcp-wire-format.md) - TDS wire format

## Next Steps

Run `/speckit.tasks` to generate implementation tasks from this plan.
