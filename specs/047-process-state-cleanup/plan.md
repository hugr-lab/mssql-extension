# Implementation Plan: Process-Wide State Cleanup

**Branch**: `047-process-state-cleanup` | **Date**: 2026-05-17 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/047-process-state-cleanup/spec.md`

## Summary

Replace the singleton-based pool / context-managers / result-stream-registry ownership with per-`MSSQLCatalog` ownership via RAII (`unique_ptr` member + inline members). Closes 3 reproducible bug classes (cross-instance contamination, cross-instance cascade failure on DETACH, silent-shutdown leak) including production-reported [issue #96](https://github.com/hugr-lab/mssql-extension/issues/96). Retires spec 045's `g_context_managers` band-aid (`70a4d90`). Adds eager ATTACH credential validation (fixes "ATTACH passes with wrong password" UX bug surfaced during plan review). Marks `mssql_open`/`mssql_close`/`mssql_ping` as `[DEPRECATED]` and keeps their handle-manager singleton as legitimate (no catalog binding by API). Surface API stable; 3 internal singletons deleted; bench-neutral (hot paths already go through the catalog).

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard, C++11-compatible ABI on Linux for ODR safety)

**Primary Dependencies**: DuckDB (main branch), OpenSSL (vcpkg, statically linked), existing TDS protocol layer. **No new dependencies.**

**Storage**: In-memory only (per-catalog `unique_ptr<tds::ConnectionPool>`; per-instance `MSSQLDiagnosticState` holding the relocated handle map + result-stream registry).

**Testing**:
- C++ unit tests via DuckDB unittest harness (`test/cpp/`)
- SQL logic tests (`test/sql/`)
- New: `test/cpp/test_multi_instance_pool_isolation.cpp` (Scenarios 1-3 + SC-001..SC-003)
- New: `test/cpp/test_issue_96_attach_loop.cpp` (Scenario 4 + SC-009)
- Existing test suite (`make test` + `make integration-test`) — green at every commit per SC-007

**Target Platform**: Linux GCC, macOS Clang, Windows MSVC, Windows MinGW (Rtools 4.2). All four must remain green.

**Project Type**: DuckDB extension (single C++ project). Source under `src/`, tests under `test/`, headers mirror `src/include/`.

**Performance Goals**:
- **Zero per-row overhead**: hot paths (table scan, COPY, DML, ConnectionProvider) already go through `catalog.GetConnectionPool()`, not the singleton. Per-row dispatch is unchanged.
- **Construction overhead**: one-time pool factory call per `ATTACH` (currently goes through `MssqlPoolManager::GetOrCreatePool*`; after spec 047 the same factory is called from `MSSQLCatalog::Initialize`). No measurable per-row cost.
- **Bench parity gate**: `test/bench/bench_codec_e2e.sh` at 1M rows stays within ±2% of pre-spec-047 baseline (same gate spec 045 used).

**Constraints**:
- No public extension API change (Constraint #1 + SC-007). All 12+ extension functions keep signatures and observable semantics.
- TDS pool semantics preserved exactly (acquire / release / pin / idle timeout / connection limit / metrics).
- No backward-compatibility shims for the removed singletons (Constraint #4 — clean break).
- TokenCache stays singleton (clarification Q3 — reclassified as legitimate).

**Scale/Scope**:
- **3 singleton classes deleted**: `MssqlPoolManager`, `MSSQLContextManager`, `MSSQLResultStreamRegistry`. Plus the `g_context_managers` map + lock.
- **`MSSQLConnectionHandleManager` retained** as legitimate (no catalog binding by API); surrounding `mssql_open`/`mssql_close`/`mssql_ping` functions marked `[DEPRECATED]` (FR-010).
- **~15-25 call sites updated** (per spec Inventory + Phase 1-4 walkthroughs).
- **6 implementation phases** (per spec Plan section).
- **10 success criteria** (SC-001..SC-010), including issue #96 closure (SC-009) and ATTACH credential validation (SC-010).

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

Constitution v2.0.0 (6 principles). Compliance analysis:

| Principle | Compliance | Rationale |
|---|---|---|
| I. Native and Open | ✓ Unchanged | No TDS / ODBC / FreeTDS surface affected. Pure ownership refactor of existing C++ classes. |
| II. Streaming First | ✓ Unchanged | No buffering changes; hot paths preserved as-is. |
| III. Correctness over Convenience | ✓ **Strengthened** | Replaces silent-leak / silent-collide / cross-instance-contamination failure modes with explicit RAII teardown + per-catalog isolation. Failures (e.g., issue #96) become impossible-by-construction rather than papered over with band-aids. |
| IV. Explicit State Machines | ✓ **Strengthened** | `MSSQLCatalog` lifecycle becomes explicit RAII (catalog owns pool → pool owns connections → connections own sockets). Today's silent-shutdown leak is implicit-state. New model has documented teardown order. |
| V. DuckDB-Native UX | ✓ Unchanged + improved | No surface API change. FR-003 makes diagnostic enumeration walk DuckDB's own catalog list (more DuckDB-native than the singleton-keyed approach). |
| VI. Incremental Delivery | ✓ Aligned | 6-phase plan; each phase independently buildable + testable. Phases 1-3 land the core fix; Phases 4-6 are polish + tests. |

**Verdict**: PASS, no violations. Several principles are actively strengthened by the refactor. **No entries in Complexity Tracking.**

Re-check post-Phase 1: same (design follows the principles).

## Project Structure

### Documentation (this feature)

```text
specs/047-process-state-cleanup/
├── plan.md                          # This file (/speckit-plan output)
├── spec.md                          # Feature spec (/speckit-specify + /speckit-clarify outputs)
├── research.md                      # Phase 0 output (design decisions + plan-review corrections)
├── data-model.md                    # Phase 1 output (ownership graph before/after)
├── quickstart.md                    # Phase 1 output (per-phase validation + SC checklist)
├── contracts/
│   └── README.md                    # Internal C++ API additions; no public interface changes
├── checklists/
│   └── requirements.md              # Spec quality gate (/speckit-specify output)
└── tasks.md                         # Phase 2 output (/speckit-tasks command — NOT generated here)
```

### Source Code (repository root)

```text
src/
├── catalog/
│   ├── mssql_catalog.cpp            # MAJOR — owns unique_ptr<ConnectionPool>; inlines pool construction; adds RegisterStream/RetrieveStream; eager validation (FR-011)
│   ├── mssql_catalog.hpp            # type change + new members + new methods
│   └── mssql_transaction.cpp        # MINOR — Increment/DecrementPinnedCount → pool instance methods
├── connection/
│   ├── mssql_pool_manager.{cpp,hpp} # DELETE — singleton, 3 GetOrCreatePool* bodies move into MSSQLCatalog::Initialize
│   ├── mssql_connection_provider.cpp # NO CHANGE — already routes via catalog
│   ├── connection_pool.{cpp,hpp}    # MINOR — add pinned_count_ atomic + Increment/Decrement/GetPinnedCount methods
│   └── mssql_diagnostic.cpp         # MINOR — pool_stats walks catalog list; mssql_open/close/ping descriptions gain [DEPRECATED] prefix
├── dml/
│   ├── insert/mssql_insert_executor.cpp # MINOR — line 74: replace singleton call with catalog.GetConnectionPool()
│   └── update/mssql_update_executor.cpp # MINOR — line 73: same
├── query/
│   ├── mssql_result_stream.cpp      # MINOR — lines 82,89: replace singleton call with catalog.GetConnectionPool()
│   └── mssql_query_executor.cpp     # MINOR — line 40: same
├── mssql_storage.cpp                # MAJOR — delete MSSQLContextManager + g_context_managers; MSSQLAttach simplifies + parses lazy_validation option
├── mssql_functions.cpp              # MAJOR — delete MSSQLResultStreamRegistry; mssql_scan uses catalog.RegisterStream/RetrieveStream
└── include/                         # mirrors src/

test/
├── cpp/
│   ├── test_multi_instance_pool_isolation.cpp  # NEW — SC-001/002/003
│   └── test_issue_96_attach_loop.cpp           # NEW — SC-009
├── sql/
│   └── attach/
│       └── attach_validates_credentials.test   # NEW — SC-010 (3 cases: bad password, unreachable host, lazy_validation opt-out)
└── ...

specs/047-process-state-cleanup/
├── state_inventory.md               # FR-007 deliverable, produced in Phase 6
└── bench_results.md                 # Phase 6 — bench parity gate vs main-at-kickoff
```

**Structure Decision**: Existing single-project layout under `src/` + `test/`. **No new files added under `src/`** — all spec 047 changes are modifications to existing files plus 3 deleted singletons. `MSSQLConnectionHandleManager` stays in `src/include/connection/mssql_diagnostic.hpp` (legitimate, deprecated). New tests under `test/cpp/` and `test/sql/attach/`. (Earlier draft proposed a `connection_pool_factory.{cpp,hpp}` module + a per-instance `MSSQLDiagnosticState` container; both rejected during plan review as overengineered — see `research.md` §0.)

## Complexity Tracking

No constitution violations. Section empty.
