# Implementation Plan: Catalog Integration & Read-Only SELECT with Pushdown

**Branch**: `007-catalog-integration` | **Date**: 2026-01-18 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/007-catalog-integration/spec.md`

**Note**: This template is filled in by the `/speckit.plan` command. See `.specify/templates/commands/plan.md` for the execution workflow.

## Summary

Expose SQL Server schemas, tables, and views through the DuckDB catalog system, enabling queries via `catalog.schema.table` syntax. Implement projection and filter pushdown using parameterized execution via `sp_executesql` with collation-aware parameter binding. Support metadata caching with TTL and explicit refresh. All operations are read-only; writes rejected with clear errors.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB main branch (catalog API, DataChunk), existing TDS layer (specs 001-006), mbedTLS (via split TLS build)
**Storage**: In-memory (metadata cache with TTL), DuckDB secret manager for credentials
**Testing**: C++ unit tests (Catch2 via DuckDB), integration tests against live SQL Server
**Target Platform**: Linux, macOS, Windows (matches DuckDB extension platforms)
**Project Type**: Single C++ extension (existing structure)
**Performance Goals**: Filter pushdown query with `WHERE id = 1` on 1M row table < 100ms, metadata refresh < 1s
**Constraints**: Bounded memory via streaming (no full result buffering), sargable query forms only
**Scale/Scope**: Arbitrary table sizes via streaming, 1000s of tables/views in catalog

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Requirement | Compliance | Notes |
|-----------|-------------|------------|-------|
| I. Native and Open | No ODBC/JDBC/FreeTDS | PASS | Uses existing native TDS layer from specs 001-006 |
| II. Streaming First | No full result buffering | PASS | Leverages MSSQLResultStream from spec 004 |
| III. Correctness over Convenience | Fail explicitly when unsafe | PASS | Write ops rejected; unsupported filters stay local |
| IV. Explicit State Machines | Documented state transitions | PASS | Connection states already defined; adds metadata cache states |
| V. DuckDB-Native UX | Real DuckDB catalog | PASS | Core goal: schemas/tables browsable via SHOW/DESCRIBE |
| VI. Incremental Delivery | Read-first delivery | PASS | This spec is read-only; writes deferred to future specs |

**Constitution Gate Status**: PASS - No violations. Proceed to Phase 0.

## Project Structure

### Documentation (this feature)

```text
specs/007-catalog-integration/
├── plan.md              # This file (/speckit.plan command output)
├── research.md          # Phase 0 output (/speckit.plan command)
├── data-model.md        # Phase 1 output (/speckit.plan command)
├── quickstart.md        # Phase 1 output (/speckit.plan command)
├── contracts/           # Phase 1 output (/speckit.plan command)
└── tasks.md             # Phase 2 output (/speckit.tasks command - NOT created by /speckit.plan)
```

### Source Code (repository root)

```text
src/
├── include/
│   ├── mssql_extension.hpp        # Extension entry point
│   ├── mssql_storage.hpp          # ATTACH/DETACH, MSSQLCatalog
│   ├── mssql_secret.hpp           # Secret type
│   ├── connection/
│   │   ├── mssql_pool_manager.hpp # Connection pool management
│   │   └── mssql_settings.hpp     # Extension settings
│   ├── query/
│   │   ├── mssql_query_executor.hpp
│   │   └── mssql_result_stream.hpp
│   ├── catalog/                   # NEW: Catalog integration layer
│   │   ├── mssql_catalog.hpp      # MSSQLCatalog implementation
│   │   ├── mssql_schema_entry.hpp # DuckDB SchemaEntry subclass
│   │   ├── mssql_table_entry.hpp  # DuckDB TableEntry subclass
│   │   ├── mssql_table_function.hpp # Table function for scans
│   │   ├── mssql_metadata_cache.hpp # Metadata caching with TTL
│   │   └── mssql_column_info.hpp  # Column metadata including collation
│   ├── pushdown/                  # NEW: Filter/projection pushdown
│   │   ├── mssql_filter_translator.hpp # DuckDB filters → SQL Server WHERE
│   │   ├── mssql_projection_builder.hpp # Column list generation
│   │   └── mssql_prepared_statement.hpp # sp_executesql wrapper
│   └── tds/                       # Existing TDS protocol layer
│       └── ...
├── catalog/                       # NEW: Implementation files
│   ├── mssql_catalog.cpp
│   ├── mssql_schema_entry.cpp
│   ├── mssql_table_entry.cpp
│   ├── mssql_table_function.cpp
│   ├── mssql_metadata_cache.cpp
│   └── mssql_column_info.cpp
├── pushdown/                      # NEW: Implementation files
│   ├── mssql_filter_translator.cpp
│   ├── mssql_projection_builder.cpp
│   └── mssql_prepared_statement.cpp
└── ...

test/
├── cpp/
│   ├── test_connection_pool.cpp   # Existing
│   ├── test_tls_connection.cpp    # Existing
│   ├── test_metadata_cache.cpp    # NEW: Cache TTL, refresh tests
│   ├── test_filter_pushdown.cpp   # NEW: Filter translation tests
│   └── test_collation_handling.cpp # NEW: Collation-aware binding tests
└── sql/                           # NEW: Integration tests
    ├── catalog_discovery.test     # SHOW SCHEMAS, SHOW TABLES, DESCRIBE
    ├── projection_pushdown.test   # SELECT specific columns
    ├── filter_pushdown.test       # WHERE clause pushdown
    └── collation_handling.test    # VARCHAR collation scenarios
```

**Structure Decision**: Extends existing single-project C++ extension structure. New modules (`catalog/`, `pushdown/`) added under `src/` following established patterns. Integration tests added in `test/sql/` using DuckDB's SQL test format.

## Complexity Tracking

> No constitutional violations identified. This section intentionally empty.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| (none) | N/A | N/A |

---

## Post-Design Constitution Check

*Re-evaluation after Phase 1 design completion.*

| Principle | Requirement | Compliance | Design Evidence |
|-----------|-------------|------------|-----------------|
| I. Native and Open | No ODBC/JDBC/FreeTDS | PASS | Design uses existing TDS layer; sp_executesql is native SQL Server RPC |
| II. Streaming First | No full result buffering | PASS | MSSQLResultStream streams directly to DataChunk; metadata cache is bounded |
| III. Correctness over Convenience | Fail explicitly when unsafe | PASS | Write operations throw NotImplementedException; unsupported filters stay local; collation conflicts don't corrupt data |
| IV. Explicit State Machines | Documented state transitions | PASS | MSSQLMetadataCache has explicit states (Empty→Loaded→Stale→Invalid); connection pool states from spec 003 |
| V. DuckDB-Native UX | Real DuckDB catalog | PASS | MSSQLCatalog/SchemaEntry/TableEntry inherit DuckDB base classes; SHOW SCHEMAS/TABLES/DESCRIBE work natively |
| VI. Incremental Delivery | Read-first delivery | PASS | This spec is read-only SELECT; INSERT/UPDATE/DELETE explicitly deferred to specs 05.03/05.04 |

**Row Identity Model Compliance**: N/A for this spec (read-only). UPDATE/DELETE support deferred.

**Version Baseline Compliance**: PASS - Design targets SQL Server 2019+; parameters transmitted as UTF-16LE via sp_executesql NVARCHAR.

**Post-Design Gate Status**: PASS - Design fully compliant with constitution.
