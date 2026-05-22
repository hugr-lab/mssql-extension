# Research: DML Rewrite (spec 050)

Resolved questions and design decisions for the UPDATE/DELETE
pipeline rewrite. All items below are decision-locked; no
`NEEDS CLARIFICATION` markers remain in spec.md or plan.md.

## R1. Two-path architecture (direct + staging) vs unified staging path

**Decision**: Keep both paths. Use a conservative whitelist to select
direct; staging is the default and handles everything else.

**Rationale**:

- `DELETE FROM mssql.dbo.t WHERE id = 5` should run as a single
  round-trip statement. Pushing every such operation through the
  staging path (create temp table → BCP one row → JOIN-back delete)
  is correct but adds 4–5 unnecessary round-trips and tempdb churn
  for the trivial case.
- The two paths share enough infrastructure (capability detection,
  RETURNING parser, transaction wrap is staging-only) that the
  duplication cost is small.
- The whitelist is intentionally narrow at v0.2.0 launch and can
  expand later based on real query shapes — the "wrong direction"
  here is to start permissive and discover a corner case in
  production.

**Alternatives considered**:

- *Unified staging-only path*. Rejected: ~30ms latency floor for
  every trivial DML on a remote SQL Server. Bad for ad-hoc usage.
- *Permissive direct-path eligibility* (allow joins to other tables
  if they would push down). Rejected for v0.2.0: too easy to
  misjudge what "pushable" means at plan time without exercising
  the spec 044 filter encoder along the entire path. Revisit after
  several PRs of real-world experience.

## R2. RowID strategy resolution algorithm

**Decision**: `auto` setting prefers PK, falls back to physloc,
errors otherwise. Explicit `pk` or `physloc` is a hard constraint and
errors if unsupported.

**Rationale**:

- PK-based rowid is concurrency-safe (row identity stable under
  concurrent writes as long as the PK isn't updated).
- `%%physloc%%` is the only universal rowid on a heap or B-tree
  without PK, but it changes on row movement (page split, heap
  forward pointer, etc.). Falling back to physloc when PK isn't
  available beats refusing all DML on heaps, but the concurrency
  caveat is real and is documented.
- Explicit strategy override (`SET mssql_rowid_strategy = 'pk'`) is
  the escape hatch for users who know their workload requires the
  safer path even at the cost of a clear error on PK-less tables.

**Alternatives considered**:

- *Always require PK; error on missing*. Rejected: too restrictive
  for analytical / staging tables, which are a common SQL Server
  pattern (data warehousing, ETL landing zones).
- *Always use physloc; ignore PK*. Rejected: silent concurrency
  hazard, and physloc doesn't work on columnstore / memory-OLTP /
  Synapse Dedicated.
- *Server-side decision (per-statement `OUTPUT $action`-style trick)*.
  Rejected: requires server-side temp table writes anyway; doesn't
  simplify anything.

## R3. Per-table capability matrix

**Decision**: Compute at metadata load, cache on `MSSQLTableEntry`.
Single query joining `sys.tables`, `sys.indexes`, and the existing PK
metadata path; one row per table.

**Rationale**:

- The PK metadata is already loaded per table (see
  `src/include/catalog/mssql_primary_key.hpp`); the additional
  flags (`is_memory_optimized`, `is_external`, columnstore-index
  presence) cost two extra columns + one join. Negligible.
- Per-statement re-probing was considered and rejected — would add
  one RTT per DML. Capabilities are stable for the catalog's
  lifetime; if the user runs `ALTER INDEX … REBUILD WITH (DATA_COMPRESSION =
  COLUMNSTORE_ARCHIVE)`, they re-attach.

**The query** (final form):

```sql
SELECT
    t.object_id,
    t.is_memory_optimized,
    t.is_external,
    CASE WHEN EXISTS (
        SELECT 1 FROM sys.indexes i
        WHERE i.object_id = t.object_id AND i.is_primary_key = 1
    ) THEN 1 ELSE 0 END AS has_primary_key,
    CASE WHEN EXISTS (
        SELECT 1 FROM sys.indexes i
        WHERE i.object_id = t.object_id AND i.type IN (5, 6)
    ) THEN 1 ELSE 0 END AS has_columnstore
FROM sys.tables t
WHERE t.object_id = OBJECT_ID(@table_name);
```

(In practice integrated into the existing per-table metadata fetch
to avoid an extra round-trip.)

## R4. Engine-edition probe

**Decision**: `SELECT SERVERPROPERTY('EngineEdition')` once per
connection lifetime, cached on the connection state object. Mapped
to a `MSSQLEngineEdition` enum.

**Rationale**:

- The probe is one round-trip and the result never changes within a
  connection. Caching is trivial; not caching means an extra RTT on
  every catalog load.
- The mapping (per spec.md, EngineEdition 1–11) is small and stable;
  hard-coded enum is fine.

## R5. Transaction wrap mechanism

**Decision**: Probe `@@TRANCOUNT` at Sink-phase start of every
staging operator. Branch:

| `@@TRANCOUNT` | `XACT_STATE()` | Action |
|---|---|---|
| 0 | (don't probe — irrelevant) | `BEGIN TRAN; SET XACT_ABORT ON;` … `COMMIT` / `ROLLBACK` |
| > 0 | 2 | Throw `NotImplementedException` (DTC) |
| > 0 | 1 | `SAVE TRANSACTION mssql_dml_<uuid>` … `ROLLBACK TRANSACTION mssql_dml_<uuid>` on failure |
| > 0 | -1 | Throw `NotImplementedException` (transaction is "doomed"; user must `ROLLBACK`) |

`XACT_STATE()` is only probed when `@@TRANCOUNT > 0` (autocommit
case doesn't need it).

**Rationale**:

- `@@TRANCOUNT` is the ground-truth signal. Caching
  `MSSQLTransaction::HasPinnedConnection()` was considered but
  rejected: any pass-through `mssql_exec('BEGIN TRAN')` would
  invalidate the cache silently.
- Savepoints are the only safe way to bound a sub-operation's
  rollback scope without affecting the user's outer transaction —
  SQL Server's "nested" `BEGIN TRAN` syntax is misleading (inner
  `ROLLBACK` destroys the outer).
- DTC and "doomed" states need a clear error. We don't try to
  recover; the user has to handle the outer transaction state.

**Alternatives considered**:

- *Always use savepoints* (don't probe). Rejected: requires an
  existing transaction; in autocommit mode there's nothing to
  save into.
- *Always issue our own `BEGIN TRAN`*. Rejected: inside the user's
  transaction, an inner `COMMIT` decrements `@@TRANCOUNT` to a
  state the user doesn't expect, and an inner `ROLLBACK` nukes the
  outer transaction.

## R6. Savepoint naming + length cap

**Decision**: `mssql_dml_<22 hex chars>` (10 char prefix + 22 hex
char truncated UUID = 32 chars total).

**Rationale**:

- SQL Server savepoint name max length is 32 chars.
- 22 hex chars = 88 bits of entropy. Even with millions of staging
  DMLs per second across nested queries, collision probability is
  negligible.
- The `mssql_dml_` prefix is greppable in `sys.dm_exec_sessions` /
  SQL Server traces, making diagnosis easy.

## R7. Cleanup semantics on success and error paths

**Decision**:

- **Success**: explicit `DROP TABLE #upd_<uuid>` before
  commit/savepoint-discard. Prevents tempdb residence in pooled
  long-lived connections.
- **Autocommit error**: `ROLLBACK TRAN` undoes the `CREATE TABLE`;
  no explicit DROP needed.
- **Savepoint error**: `ROLLBACK TRAN mssql_dml_<uuid>` undoes the
  `CREATE TABLE`; no explicit DROP needed.
- **Connection drop**: session teardown reclaims tempdb;
  best-effort.

**Rationale**:

- Without the explicit DROP on success in pooled-connection mode,
  `#upd_<uuid>` would persist until the session ends (which, for a
  pooled connection, may be never until pool eviction). Tempdb
  bloat over time.
- Error paths leverage SQL Server's own rollback semantics; no
  client-side cleanup code needed.

## R8. Scan-side rowid emission shape

**Decision**: `MaterializeMssqlScans(plan, strategy)` walks the
operator tree once, finds every MSSQL `PhysicalTableScan` (matched
by name), sets `bind_data.emit_rowid = true`,
`bind_data.rowid_strategy = strategy`,
`requires_materialization = true`, `max_threads = 1`.

**Rationale**:

- Mirrors `postgres-extension`'s `MaterializePostgresScans` pattern.
  Same trade-off: single-threaded scan for rowid coherence vs
  parallel scan throughput.
- `max_threads = 1` is the simplest correct answer. Parallel scan
  with rowid emission would require coordinating row order
  guarantees across worker connections; out of scope for v0.2.0.
- `requires_materialization` prevents the DML operator from
  re-pulling from the scan (which would re-execute the SELECT and
  potentially see different rows under concurrent writers).

**Alternatives considered**:

- *Parallel rowid scan with per-worker staging tables*. Rejected:
  significant complexity for a workload (DML) that is typically
  not the bottleneck.

## R9. Direct-path eligibility check

**Decision**: Whitelist of plan-node types between
`LogicalUpdate`/`LogicalDelete` and the leaf `LogicalGet`:

- `LogicalGet`
- `LogicalFilter` (with every predicate fully pushable per spec 044)
- `LogicalProjection` (with expressions that reference only the
  target's own columns and constants)

Anything else → staging path.

**Rationale**:

- Most "simple" DML (point UPDATE, point DELETE, IN-list DELETE)
  falls into this shape.
- Joins, aggregations, subqueries, set ops, window functions, and
  computed values referring to other tables all require materialized
  evaluation on DuckDB's side, which only the staging path can
  handle.
- The check is implemented as a single recursive walk; ~30 LOC.

**Open question for future**: should single-table self-joins (e.g.
`UPDATE t SET ... FROM t t1 JOIN t t2 …`) be direct? Today: no
(staging). Possible future expansion if real queries demand it.

## R9b. MERGE override mechanism — confirm DuckDB target API

**Decision**: `MSSQLCatalog` adds a `PlanMergeInto` override that
throws `NotImplementedException`. This presumes DuckDB's `Catalog`
base class exposes `PlanMergeInto` as a virtual override point at
the extension's pinned DuckDB version (`src/include/catalog/mssql_catalog.hpp`
currently overrides `PlanInsert`, `PlanCreateTableAs`,
`PlanDelete`, `PlanUpdate` — `PlanMergeInto` would join this set).

**Verification task** (T-001a, added implicitly to Phase 1 setup):
before any US5 implementation, verify the DuckDB version the
extension targets exposes `Catalog::PlanMergeInto` (or equivalent
binder/planner hook). If the virtual does not exist:

1. The guard moves to a binder hook (`BindCreateIndex`-style) that
   intercepts `MERGE INTO` statements targeting an MSSQL table at
   bind time and throws before planning starts.
2. The spec's FR-040 wording is generalized from "PlanMergeInto"
   to "MERGE intercept at bind time"; spec.md US5 acceptance
   scenarios stay unchanged (still asserts the
   `NotImplementedException` and message contents).

This is a research item, not a blocker. The source design doc
assumed the override exists; the verification just confirms it.

## R10. MERGE: defer vs partial implementation

**Decision**: Defer entirely. Replace v0.1.x behavior with a clear
`NotImplementedException` at plan time.

**Rationale**:

- v0.1.x MERGE silently produces wrong results for `WHEN NOT
  MATCHED` (no rowid for new rows) — actively dangerous.
- A correct MERGE implementation requires either (a) server-side
  `MERGE INTO` execution (different on each engine edition,
  triggers, OUTPUT semantics) or (b) a staging-table approach with
  per-clause materialization — both substantial work that doesn't
  fit in 050's scope.
- The error message lists the two migration patterns
  (`INSERT … WHERE NOT EXISTS`, `UPDATE … FROM source`) so users
  can rewrite without ambiguity.

**Alternative considered**:

- *Partial MERGE supporting only `WHEN MATCHED THEN UPDATE`*.
  Rejected: half-implemented MERGE is worse than no MERGE — users
  will write three-clause MERGEs and not realize the other two
  clauses silently no-op.

## R11. RETURNING parser generalization

**Decision**: Rename `MSSQLReturningParser` →
`MSSQLDmlReturningParser`, lift from `src/dml/insert/` to
`src/dml/`. New constructor signature:

```cpp
MSSQLDmlReturningParser(
    const vector<TdsColumnMetadata>& output_columns,
    const vector<idx_t>& returning_column_ids,
    /* …existing args… */
);
```

The existing INSERT path keeps working via a one-call shim:

```cpp
// src/dml/insert/mssql_insert.cpp
auto parser = make_uniq<MSSQLDmlReturningParser>(
    target.GetTdsColumns(),
    target.GetReturningColumnIds(),
    /* … */);
```

**Rationale**:

- INSERT-specific knowledge (`MSSQLInsertTarget` reference) was
  artificial coupling — the parser only needs the column metadata
  and the returning-column id list. Decoupling is mechanical.
- All four new operators (direct/staging × update/delete)
  consume the same generalized parser.

## R12. Settings registration

**Decision**: Three settings registered via the existing
`mssql_*` settings infrastructure:

- `mssql_rowid_strategy` (string: `auto` | `pk` | `physloc`;
  default `auto`)
- `mssql_direct_dml` (boolean; default `true`)
- `mssql_dml_log_transactions` (boolean; default `false`)

**Rationale**:

- Three is enough to cover all the visible behavior toggles users
  might want during v0.2.0.
- `mssql_dml_log_transactions` is purely diagnostic — logs every
  `BEGIN`/`COMMIT`/`SAVE`/`ROLLBACK` to the extension's debug
  channel. Off by default; users turn it on when chasing a wedge.

## R13. Benchmark scope (SC-004)

**Decision**: One representative workload, run twice (baseline
v0.1.18 vs spec-050 PR head):

- 1M-row UPDATE on a single INT column with WHERE on a PK column,
  against the Docker SQL Server (`mcr.microsoft.com/mssql/server:2022-latest`).
- Time-to-completion recorded in `bench_results.md`.
- Target: ≥ 5× speedup on spec-050.

**Rationale**:

- Single workload keeps the benchmark short and reproducible.
- 1M rows is enough to be staging-dominated; smaller workloads
  would be RTT-bound and not measure the staging-path improvement.
- Larger workloads (100M) would be useful but require a beefier
  SQL Server instance than the standard CI container; deferred to
  manual benchmarking outside the spec gate.

## R14. Coordination with spec 044 and spec 046

**Decision**:

- Spec 044 must be merged before 050 starts in earnest. Hard
  dependency for direct-path literal formatting and staging DDL.
- Spec 046 should be merged before 050 to satisfy SC-004. If 046
  slips, 050 still functions (correctness intact); SC-004 is
  renegotiated to match v0.1.x BCP throughput.
- Coordination is one-way (050 depends on 044/046, not vice
  versa); no per-file collision risk because 050 touches `src/dml/`
  and `src/catalog/` while 044 touches `src/tds/encoding/` and 046
  touches `src/copy/`.

## R15. Removal of the legacy rowid pipeline

**Decision**: Remove the v0.1.x rowid extractor + delete/update
executor/statement/target files as part of the 050 PR. No
deprecation period.

**Rationale**:

- These files exist solely to support the broken v0.1.x pipeline.
  Keeping them around as "legacy fallback" would mean shipping two
  DML pipelines and asking users to choose between them — bad
  surface area.
- The new pipeline covers every case the old one did (with PK)
  plus PK-less tables (with physloc). Nothing the old pipeline
  uniquely supported is lost.
- The MERGE guard explicitly replaces v0.1.x MERGE; tests that
  exercise the old MERGE behavior are deleted or rewritten.
