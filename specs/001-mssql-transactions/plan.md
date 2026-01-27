# Implementation Plan: MSSQL Transactions (DuckDB-Compatible MVP)

**Branch**: `001-mssql-transactions` | **Date**: 2026-01-26 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/001-mssql-transactions/spec.md`

## Summary

Implement DuckDB transaction support for the MSSQL extension by mapping DuckDB transactions to SQL Server transactions using a **pinned connection** strategy. When a DuckDB transaction begins, a SQL Server connection is acquired from the pool and pinned to that transaction for exclusive use. All DML operations, `mssql_scan()`, and `mssql_exec()` calls within the transaction use this pinned connection. On commit/rollback, the connection is returned to the pool after verifying clean transaction state.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB main branch (TransactionManager, Transaction classes), existing TDS layer (ConnectionPool, TdsConnection)
**Storage**: In-memory (pinned connection stored in MSSQLTransaction object)
**Testing**: DuckDB SQLLogicTest framework (`make test`, `make integration-test`)
**Target Platform**: Linux (primary), macOS, Windows (x64)
**Project Type**: Single (DuckDB extension)
**Performance Goals**: Transaction operations should add minimal overhead (<1ms per begin/commit/rollback on local connections)
**Constraints**: Single-threaded execution within transactions (MaxThreads = 1); no parallel scans on pinned connection
**Scale/Scope**: Single active transaction per DuckDB ClientContext; connection pool limit unchanged

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | ✅ Pass | Uses existing native TDS implementation; no new external dependencies |
| II. Streaming First | ✅ Pass | No additional buffering; operations stream through existing TDS pipeline |
| III. Correctness over Convenience | ✅ Pass | Explicit transaction boundaries; catalog scan restriction prevents consistency issues |
| IV. Explicit State Machines | ✅ Pass | Connection pinning and transaction state are explicit and testable |
| V. DuckDB-Native UX | ✅ Pass | Standard BEGIN/COMMIT/ROLLBACK semantics; catalog-integrated DML works in transactions |
| VI. Incremental Delivery | ✅ Pass | MVP restricts catalog scans in transactions; full support can be added later |

**Row Identity Model**: Unchanged; UPDATE/DELETE still require primary keys per constitution.

## Project Structure

### Documentation (this feature)

```text
specs/001-mssql-transactions/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output (N/A - no API contracts for this feature)
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (repository root)

```text
src/
├── include/
│   ├── catalog/
│   │   └── mssql_transaction.hpp     # MODIFY: Add pinned connection, mutex, transaction state
│   └── connection/
│       └── mssql_connection_provider.hpp  # NEW: Interface for getting connections (pool or pinned)
├── catalog/
│   └── mssql_transaction.cpp         # MODIFY: Implement BEGIN/COMMIT/ROLLBACK with pinned connection
├── connection/
│   └── mssql_connection_provider.cpp # NEW: Connection acquisition logic
├── table_scan/
│   └── catalog_scan_bind.cpp         # MODIFY: Add transaction check in bind phase
├── dml/
│   ├── insert/
│   │   └── mssql_insert_executor.cpp # MODIFY: Use connection provider
│   ├── update/
│   │   └── mssql_update_executor.cpp # MODIFY: Use connection provider
│   └── delete/
│       └── mssql_delete_executor.cpp # MODIFY: Use connection provider
├── query/
│   └── mssql_query_executor.cpp      # MODIFY: Use connection provider for mssql_scan
└── mssql_functions.cpp               # MODIFY: Use connection provider for mssql_exec

test/sql/
├── transaction/                      # NEW: Transaction test suite
│   ├── transaction_commit.test       # T1: DML commit tests
│   ├── transaction_rollback.test     # T2: Rollback tests
│   ├── transaction_mssql_scan.test   # T4: Read-your-writes tests
│   ├── transaction_mssql_exec.test   # T5: mssql_exec in transaction
│   ├── transaction_catalog_restriction.test  # T3: Catalog scan rejection
│   └── transaction_autocommit.test   # T6: Autocommit mode preservation
```

**Structure Decision**: Single project structure; extends existing `src/catalog/` and `src/connection/` directories.

## Complexity Tracking

No constitution violations requiring justification.
