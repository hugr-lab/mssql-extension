# Implementation Plan: DML Rewrite (UPDATE/DELETE + MERGE Guard)

**Branch**: `050-dml-rewrite` | **Date**: 2026-05-22 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/050-dml-rewrite/spec.md`

## Summary

Replace the v0.1.x rowid-fetch + `VALUES`-batched UPDATE/DELETE
pipeline with a two-path architecture:

1. **Direct path** (`MSSQLDirectDelete` / `MSSQLDirectUpdate`) — single
   round-trip `DELETE/UPDATE … WHERE <pushdown> [OUTPUT …]` for plans
   that are structurally a `LogicalUpdate`/`LogicalDelete` over a
   bare `LogicalGet` (with optional `LogicalFilter` /
   `LogicalProjection`) and where every filter expression is fully
   pushable per spec 044's codec.
2. **Staging path** (`MSSQLStagingDelete` / `MSSQLStagingUpdate`) —
   session-scoped temp table `#upd_<uuid>` populated via spec 046's
   pipelined BCP, then a single `UPDATE/DELETE … FROM … JOIN
   #upd_<uuid> ON <rowid_predicate> [OUTPUT …]` statement.

Rowid strategy (`PrimaryKey` vs `PhysLoc`) is selected at plan time
from the per-table `TableDmlCapabilities` populated during catalog
load. Engine edition is probed once per connection and cached on the
connection state. Users can override via `mssql_rowid_strategy =
auto|pk|physloc`.

Transaction safety: staging operators probe `@@TRANCOUNT` at
Sink-phase start. Autocommit case wraps in `BEGIN`/`COMMIT` with
`SET XACT_ABORT ON`. Inside-user-transaction case uses
`SAVE TRANSACTION mssql_dml_<uuid>` (UUID truncated to fit the 32-char
limit) and `ROLLBACK TRANSACTION mssql_dml_<uuid>` on failure. DTC
transactions (`XACT_STATE() = 2`) throw `NotImplementedException`.

`MSSQLCatalog::PlanMergeInto` throws `NotImplementedException`
listing the INSERT-WHERE-NOT-EXISTS and UPDATE-FROM-SOURCE migration
patterns. This replaces v0.1.x's silently-broken rowid-based MERGE
that produced wrong results for `WHEN NOT MATCHED`. Real MERGE returns
in a future spec.

`MSSQLReturningParser` is renamed to `MSSQLDmlReturningParser` and
generalized to a `vector<TdsColumnMetadata>` constructor; the existing
INSERT path keeps working via a one-call shim.

## Technical Context

**Language/Version**: C++ — C++11-compatible at the ABI/header
surface (same constraint as specs 043/044 per `CLAUDE.md` "Build
Troubleshooting → ODR Errors on Linux"). Internal `.cpp` TUs may use
later features.

**Primary Dependencies**:
- DuckDB (main branch, extension API) — unchanged.
- Spec 044 (`encoding::FormatSqlLiteral`,
  `encoding::AppendDdlColumnType`) — already merged on `main` by
  the time 050 starts. No `vcpkg.json` or `CMakeLists.txt` changes
  for the dependency itself.
- Spec 046 (pipelined BCP writer) — must be merged before 050
  starts to satisfy SC-004. 050 functions without 046 but at
  v0.1.x upload throughput, which fails the 5× speedup target.
- Existing TDS protocol layer (`src/tds/`) — call-site additions
  only.

**Storage**: Tempdb on the SQL Server side holds `#upd_<uuid>`
tables during staging operations. Sizing documented in spec
Edge Cases.

**Testing**:
- DuckDB SQLLogicTest framework (`test/sql/dml/`) — new test
  files per User Story (see tasks.md). Existing `test/sql/dml/`
  files reviewed and rewritten where they exercised v0.1.x rowid
  behavior.
- C++ unit tests (`test/cpp/`) — one new test for the
  `ResolveRowIdStrategy` decision matrix
  (`test_resolve_rowid_strategy.cpp`).
- Integration benchmark (`test/bench/bench_dml_throughput.sh`,
  new) — 1M-row UPDATE against the Docker SQL Server, before/after
  numbers recorded in `bench_results.md`. Local-only; no CI perf
  gate.

**Target Platform**: Linux, macOS, Windows MSVC, Windows MinGW —
same matrix as specs 043/044. No new platform risk; DML uses the
same TDS / BCP / codec infrastructure already tested across the
matrix.

**Project Type**: DuckDB community extension (C++ shared library
loadable into DuckDB).

**Performance Goals**:
- **Direct path**: single round-trip; latency dominated by network
  RTT + server execution. No measurable extension overhead.
- **Staging path**: ≥ 5× speedup over v0.1.18 for 1M-row UPDATE on
  remote SQL Server (SC-004). Driver: BCP rowid upload replaces
  N × per-batch `VALUES` round-trips.

**Constraints**:
- No new external dependencies (no MERGE library, no extra parsers).
- Spec 044 and 046 must be on `main` before 050 lands; otherwise
  the spec's value props fail.
- Atomicity contract: every DML is either fully applied or fully
  rolled back. No chunked partial commits in v0.2.0.
- Net code size ≤ +800 LOC (SC-009).
- Backward compat: every previously-green non-MERGE DML test stays
  green or is rewritten with rationale (SC-010).

**Scale/Scope**:
- One PR.
- ~9 new source files (4 operator pairs + capabilities header +
  materialize-scans helper + parser rename + merge guard).
- ~5 source files removed (legacy rowid extractor + delete/update
  executor/statement/target stack).
- Estimated 22 source-file edits including header includes and the
  catalog dispatch rewrite.
- ~6 new SQLLogicTest files + 1 C++ unit test + 1 shell benchmark.
- Ballpark ~700 net LOC growth (consistent with source doc
  estimate; SC-009 caps at +800).

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

Constitution v2.0.0 principles applied:

| # | Principle | Status | Notes |
|---|-----------|--------|-------|
| I | Native and Open | **PASS** | No new dependencies; the staging temp-table pattern is the same one `postgres-extension` uses. |
| II | Streaming First | **PASS** | The scan side streams chunks straight into pipelined BCP; the staging operator does not materialize the rowid set in extension memory beyond a single chunk's window. |
| III | Correctness over Convenience | **PASS** | The whole motivation is removing v0.1.x's silently-broken behavior on MERGE and on PK-less tables. The MERGE guard prefers a loud error to a silent corruption. |
| IV | Explicit State Machines | **PASS** | Transaction wrap is a small explicit FSM (`probe @@TRANCOUNT → branch → wrap → commit/savepoint-discard / rollback`). DTC case is a leaf-error state, not a silent fallback. |
| V | DuckDB-Native UX | **PASS** | New settings registered via DuckDB's standard `ClientContext::TryGetCurrentSetting`. Errors thrown via DuckDB exception types (`BinderException`, `NotImplementedException`) so DuckDB CLI formatting is preserved. |
| VI | Incremental Delivery | **PASS** | Five user stories. US1 (catalog detection) is independently testable via a diagnostic table function. US2 (direct path) is a pure read of `@@ROWCOUNT` from a single statement. US3 (staging) builds on US1 and the existing BCP infrastructure. US4 (transactions) is independently testable via injected failures. US5 (MERGE guard + parser rename) is one error-path + one mechanical refactor. |

**Row Identity Model**: this spec defines and locks the row-identity
model for the v0.2.0 series. PK is preferred when available;
`%%physloc%%` is the fallback; concurrency caveats are documented.
Compatible with `MSSQLTransaction`'s pinned-connection model from spec
001-mssql-transactions (pinning makes `@@TRANCOUNT` and savepoints
behave as expected).

## Project Structure

### Documentation (this feature)

```text
specs/050-dml-rewrite/
├── plan.md                 # This file
├── spec.md
├── research.md             # Phase 0 output: design decisions
├── data-model.md           # Phase 1 output: C++ types, settings, T-SQL templates
├── quickstart.md           # Phase 1 output: local repro recipe
├── checklists/
│   └── requirements.md
└── tasks.md                # Phase 2 output: ordered work breakdown
```

### Source Code (repository root)

```text
src/
├── catalog/
│   ├── mssql_catalog.cpp                       # MODIFY: PlanUpdate/Delete rewrite, PlanMergeInto override
│   ├── mssql_table_entry.cpp                   # MODIFY: load + expose TableDmlCapabilities
│   └── (existing PK / metadata files unchanged)
├── connection/
│   └── mssql_connection_provider.cpp           # MODIFY: cache engine_edition on connection state
├── dml/
│   ├── mssql_dml_returning_parser.{hpp,cpp}    # NEW: renamed + generalized
│   ├── delete/
│   │   ├── mssql_direct_delete.{hpp,cpp}       # NEW
│   │   └── mssql_staging_delete.{hpp,cpp}      # NEW
│   ├── update/
│   │   ├── mssql_direct_update.{hpp,cpp}       # NEW
│   │   └── mssql_staging_update.{hpp,cpp}      # NEW
│   ├── merge/
│   │   └── mssql_merge_guard.{hpp,cpp}         # NEW: PlanMergeInto override
│   ├── mssql_dml_capabilities.{hpp,cpp}        # NEW: TableDmlCapabilities + ResolveRowIdStrategy
│   ├── mssql_materialize_scans.{hpp,cpp}       # NEW: scan-side rowid wiring
│   └── insert/
│       └── mssql_returning_parser.{hpp,cpp}    # REMOVED (becomes shim in mssql_dml_returning_parser)
├── include/dml/                                 # mirror headers
│   ├── mssql_dml_capabilities.hpp              # NEW
│   ├── mssql_dml_returning_parser.hpp          # NEW
│   ├── mssql_materialize_scans.hpp             # NEW
│   ├── mssql_rowid_strategy.hpp                # NEW (enum + Resolve…)
│   ├── delete/{mssql_direct_delete.hpp,
│   │           mssql_staging_delete.hpp}       # NEW
│   ├── update/{mssql_direct_update.hpp,
│   │           mssql_staging_update.hpp}       # NEW
│   └── merge/mssql_merge_guard.hpp             # NEW
└── (everything else unchanged)
```

### Tests

```text
test/sql/dml/
├── dml_capabilities.test                       # NEW (US1)
├── direct_update_delete.test                   # NEW (US2)
├── staging_update_join.test                    # NEW (US3 — PK + physloc variants)
├── dml_transactions.test                       # NEW (US4 — autocommit / user-txn / DTC)
└── merge_disabled.test                         # NEW (US5)

test/cpp/
└── test_resolve_rowid_strategy.cpp             # NEW (US1/US3 — decision matrix)

test/bench/
└── bench_dml_throughput.sh                     # NEW (SC-004)
```

### Files removed

```text
src/dml/delete/mssql_delete_executor.cpp
src/dml/delete/mssql_delete_statement.cpp
src/dml/delete/mssql_delete_target.cpp
src/dml/delete/mssql_physical_delete.cpp
src/dml/update/mssql_physical_update.cpp
src/dml/update/mssql_update_executor.cpp
src/dml/update/mssql_update_statement.cpp
src/dml/update/mssql_update_target.cpp
src/dml/mssql_rowid_extractor.cpp
```

Their headers in `src/include/dml/` are removed at the same time.

## Phased Delivery

### Phase 0 — Research (output: [research.md](./research.md))

Decisions to lock before any implementation:

- **R1**: Two-path architecture (direct + staging) vs unified staging
  path. Decision: keep both for the latency win on simple targeted
  edits, gated by a conservative whitelist.
- **R2**: Rowid-strategy resolution algorithm + per-table capability
  matrix. Decision: locked in data-model.md §E1–§E2.
- **R3**: Transaction wrap mechanism. Decision: probe `@@TRANCOUNT`
  every time (no cached state), savepoints inside user transactions,
  full BEGIN/COMMIT in autocommit, DTC = NotImplementedException.
- **R4**: MERGE strategy. Decision: explicit guard, deferred.
- **R5**: Direct-path eligibility whitelist. Decision: conservative
  initial set (`LogicalGet`, `LogicalFilter`, `LogicalProjection`);
  expand later based on real query shapes.

### Phase 1 — Design (outputs: [data-model.md](./data-model.md),
[quickstart.md](./quickstart.md))

- Lock the C++ types (`TableDmlCapabilities`, `RowIdStrategy` enum,
  the new direct/staging operators' Sink/Source shape, the renamed
  `MSSQLDmlReturningParser` constructor signature).
- Lock the T-SQL templates (`CREATE TABLE #upd_<uuid>`, `UPDATE …
  FROM … JOIN`, `DELETE … FROM … JOIN`, `OUTPUT INSERTED/DELETED`,
  `BEGIN TRAN` / `SET XACT_ABORT ON` / `SAVE TRANSACTION` / `ROLLBACK
  TRANSACTION` / `XACT_STATE()`).
- Quickstart: `make`, `make test`, `make integration-test`, plus the
  manual `test/bench/bench_dml_throughput.sh` invocation.

### Phase 2 — Implementation (output: [tasks.md](./tasks.md))

Ordered by user story (US1 → US5), each independently testable.

### Phase 3 — Stabilization

- Triage every failing pre-existing DML test to either pass on the
  new path or be rewritten with a recorded rationale (SC-010).
- Run the 1M-row throughput benchmark vs v0.1.18 baseline; record
  in `bench_results.md`.
- README + docs updates per FR-070.

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Direct-path whitelist too narrow → most plans go to staging → SC-004 throughput target risks regression for simple DML | Whitelist covers `LogicalGet`, `LogicalFilter`, `LogicalProjection`. Conservative on purpose; expand later based on real query shapes. Simple targeted UPDATE/DELETE still gets the direct fast path. |
| `@@TRANCOUNT` probe adds 1 RTT per staging op | Documented in spec as ≤ 0.1% of total time on multi-second DML. The correctness benefit dwarfs the latency cost. |
| Pipelined BCP (spec 046) lands late or misses 050 | Spec 050 builds on top of 046 but does not strictly require it for correctness. SC-004 (throughput) is contingent; if 046 slips, SC-004 is renegotiated, not 050 blocked. |
| `%%physloc%%` concurrency footgun on PK-less tables | Documented caveat in spec + README. v0.2.0 does not auto-wrap in SERIALIZABLE. |
| Tempdb pressure on very large UPDATEs (multi-GB staging tables) | Documented sizing recommendation. v0.2.0 keeps atomicity over chunking. |
| MERGE guard breaks users who relied on v0.1.x's silent-corruption behavior | Error message lists the two migration patterns. Migration window documented in README before release. |
| Plan tree shapes change in a DuckDB main update | Whitelist is one switch statement; updating it is a localized edit. |

## Open Questions (resolved or accepted as risks)

- **Is `SELECT @@TRANCOUNT` cacheable across operators in the same
  query?** Yes within a single operator's lifetime, no across
  operators. We probe once per operator's Sink-phase start.
- **Should the direct path emit `SET XACT_ABORT ON` too?** No — the
  direct path is a single statement, autocommit semantics already
  give atomicity; `XACT_ABORT` only matters for multi-statement
  batches.
- **Savepoint name collisions across nested DuckDB queries (e.g. a
  subquery that performs DML)?** UUID-suffixed names + the 32-char
  truncation give 22 hex chars of entropy = 88 bits. Effectively
  collision-free.
- **What happens if the user's outer transaction is in a "doomed"
  state (`XACT_STATE() = -1`)?** Throw `NotImplementedException`;
  the user must `ROLLBACK` first. Documented in the DTC error path.
