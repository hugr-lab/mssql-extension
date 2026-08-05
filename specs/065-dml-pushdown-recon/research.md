# v0.3.0 DML rework — reconnaissance

**Date:** 2026-08-05. Four parallel investigations: current DML implementation,
DuckDB catalog API surface (+ duckdb-postgres as the reference), bulk-load
infrastructure readiness, and MERGE semantics mapping. Every claim below was
verified against the pinned submodule (v1.5.5, `d8cdaa33`) and the working
tree; the load-bearing hooks were ALSO verified identical in v1.4.4, so none
of this needs an `mssql_compat.hpp` shim.

**Scope (the user's v0.3.0 plan):** UPDATE/DELETE via temp-table staging +
collapse to direct T-SQL under full filter pushdown (no rowid round trip);
MERGE with full pushdown where possible; then JOIN / ORDER BY / simple
aggregate pushdown. Issue #239 (materialize own-catalog scans before a sink)
is a named dependency. Issue #140 (UPDATE without PK) falls out of the direct
path.

---

## 1. The decisive discoveries

Four findings shape the whole design; everything else is engineering.

### 1.1 The 3-arg PlanUpdate/PlanDelete overload — interception BEFORE the child is planned

`catalog.hpp:314/:317` declare overridable 3-arg `PlanDelete`/`PlanUpdate`
whose default plans the child and forwards to the 4-arg pure virtual we
implement today. The generator calls the **3-arg** form — so an override
receives `LogicalUpdate`/`LogicalDelete` with the logical child subtree
INTACT, can inspect the scan's pushed filters, and can decline to plan the
child at all, returning a **childless** physical operator. Fallback is one
base-class call — zero regression risk for the general case. This is more
reach than duckdb-postgres uses (they never skip the scan; their full-table
UPDATE round-trips every row).

The binder guarantees the chain shape: UPDATE → `LogicalProjection →
[LogicalFilter] → LogicalGet` (projection asserted, rowid refs appended
last); DELETE → `[LogicalFilter] → LogicalGet`.

### 1.2 MERGE INTO is fully present and hookable — and carries upsert with it

The pinned DuckDB has complete MERGE INTO (parser/binder/`PhysicalMergeInto`,
20+ tests) and a `Catalog::PlanMergeInto` hook whose default throws exactly
the error our users see today. **`INSERT ... ON CONFLICT` / `INSERT OR
REPLACE` is lowered to MERGE INTO at bind time** (`bind_insert.cpp:284,539`)
— one hook implementation unlocks the whole upsert family. duckdb-postgres
implements the hook as a 125-line composition: per-action synthetic
Logical nodes fed to its own PlanUpdate/PlanDelete/PlanInsert, wrapped in
DuckDB's generic `PhysicalMergeInto` (row classification by rowid /
source_marker NULL-ness comes free).

### 1.3 The staging seam exists but has ONE structural gap

Spec 063 built the writer machinery FOR these consumers (`load_policy.hpp:26`
names UPDATE/DELETE staging verbatim; the `#temp`-in-transaction row needs no
new rule). But `BulkLoadSession` is the *extra parallel writer* machinery
only: `TryStart` refuses `max_writers <= 1` (which `#temp` always is),
acquires from the pool only, and `Finish()` always releases — fatal for
staging, where `UPDATE ... JOIN #stage` must run NEXT on the SAME connection.
**The missing piece — a single-writer bulk-load session on a GIVEN held/pinned
connection — is needed identically by spec 062's in-transaction INSERT. Build
it once.** Everything else is reusable as-is: `BuildInsertBulkSql` (bare
`[#t]` naming), `BCPWriter`, temp DDL with tempdb metadata branches,
`ResolveVarcharCollation(target_is_temp=true)`, `ReleaseBcpConnectionOnError`
(has the `transaction_pinned` parameter).

### 1.4 #140 breaks at BIND, so the fix must move the guard

`MSSQLTableEntry::BindUpdateConstraints` (`mssql_table_entry.cpp:232-237`)
and `GetRowIdColumns` (`:421-426`) throw for PK-less tables during binding —
before any pushdown analysis can run. Even `UPDATE t SET x=1 WHERE id=1` with
a fully pushable predicate dies there. The direct-statement path removes the
PK requirement at its root (no scan → no rowid), but only if the bind-time
throw becomes conditional. DuckDB's API allows it: `GetRowIdColumns()` may
return an EMPTY vector (binds zero rowid columns — safe only because we
override the Plan hooks), letting plan time choose direct-vs-rowid and error
only when the rowid path is genuinely required. MERGE is the exception: its
binder unconditionally needs rowid for match classification, so PK-less MERGE
stays a bind error (upstream constraint, not ours).

---

## 2. Current state (what the rework replaces)

- UPDATE/DELETE: rowid VALUES-JOIN — per-row `Vector::GetValue` into
  `vector<vector<Value>>` buffers, batches shaped by
  `mssql_dml_batch_size`/`mssql_dml_max_parameters` (the "parameter" limit is
  fiction: statements inline literals; `mssql_dml_use_prepared` is loaded and
  never read — dead setting), inline-literal `UPDATE ... JOIN (VALUES ...)`.
  In explicit transactions everything defers to Finalize (rows buffered
  unbounded) because the pinned connection cannot stream the scan and take
  statements at once.
- SET expressions are computed client-side by DuckDB's projection; the
  extension ships results as literals. The bound expressions ARE visible at
  plan time, and a translator already exists —
  `FilterEncoder::EncodeExpression` (column refs, constants, the
  function-mapping table, comparisons, conjunctions, CASE, BETWEEN) — usable
  for SET clauses nearly as-is; its gap is an honest CAST→`CONVERT` story.
- The scan today silently DROPS filters it cannot encode
  (`needs_duckdb_filter` is only ever logged). Harmless for SELECT (extra
  rows), lethal for a collapsed UPDATE (extra rows get WRITTEN). The direct
  path must treat any unencodable filter as a hard bail-out to the rowid
  path — never droppable.
- The raw TDS token loop (send batch, read DONE, drain errors) is
  copy-pasted three times across update/delete/insert executors —
  consolidate during the rework.
- Bugs found in passing: INSERT RETURNING keeps only the LAST batch
  (`mssql_insert_executor.cpp:411-419` — >1000 rows silently lose RETURNING
  rows); INSERT lacks the defer-in-transaction guard UPDATE/DELETE have
  (same-catalog `INSERT ... SELECT` in a transaction is an untested collision
  on the pinned connection — #239 territory); `EnsurePKLoaded` swallows
  connection errors as "no PK".

---

## 3. Target architecture

### 3.1 UPDATE/DELETE — two paths, chosen at plan time

**Path A — direct statement (the collapse).** In the 3-arg override:
walk the logical child; require (1) chain exactly `Projection → Get` (DELETE:
`Get`), no residual `LogicalFilter`, no join; (2) `get.GetTable() ==
&op.table`; (3) plan-time dry-run of `FilterEncoder::Encode` over
`get.table_filters` with `needs_duckdb_filter == false`, plus the bind data's
`complex_filter_where_clause`; (4) for UPDATE, every SET expression in the
projection renders via `EncodeExpression` (context = target's full column
list). Then emit a childless operator running one
`UPDATE [s].[t] SET ... WHERE ...` / `DELETE FROM [s].[t] WHERE ...`;
count from the DONE token. No scan, no rowid, no PK requirement — **closes
#140** for every pushable statement. Transactions get simpler: one statement
on the pinned connection, no defer machinery.

**Path B — staged rowids (the general case).** Replaces the VALUES JOIN:
buffer chunks columnar (`[pk..., set...]` DataChunks, not Values — the
no-per-value-path rule), then on ONE held connection: `CREATE TABLE
#stage_<uid> (...)` (typed from `MSSQLUpdateColumn` mssql types +
`ResolveVarcharCollation(temp)`) → BCP the buffered chunks → one
`UPDATE t SET t.c = s.c FROM target t JOIN #stage s ON <pk>` → explicit
`DROP TABLE #stage` → release. Autocommit: the scan streams on its own pooled
connection, so staging streams during Sink on a second held connection —
bounded memory. Transaction: defer to Finalize on the pinned connection
(scan must drain first — spec 057 constraint), same memory profile as today.
A decision function keeps small updates on the VALUES join (BCP setup is not
free; mirrors spec 062 D1).

**Connection lifetime is the correctness boundary** (all agents converge on
this): any pool release between CREATE `#stage` and the join breaks it —
reset drops the table, no-reset risks a stale same-named table on another
pooled session. Hold or pin, never release-and-reacquire. `#stage` never
crosses a statement boundary, so this is compatible with
`mssql_reset_connection = true`; the DROP must still be explicit (under
`false` a leaked stage pollutes the pool).

### 3.2 MERGE — composition first, pushdown second

**Phase 1 (client-side).** `MSSQLCatalog::PlanMergeInto` mirroring
duckdb-postgres: per-action MSSQL sinks (existing rowid UPDATE/DELETE +
INSERT machinery, adapted to the merge input layout: expressions bound
against the merge projection, rowid at `op.row_id_start`, STRUCT rowid via
the existing extractor) under DuckDB's `PhysicalMergeInto`. Needs the
`keep_copy_alive=false` analogue: no held stream between chunks on the pinned
connection. Unlocks MERGE *and* `INSERT ... ON CONFLICT` in one move.

**Phase 2 (pushdown).** Same-catalog source → one server-side
`MERGE INTO t WITH (HOLDLOCK) USING (<source SELECT>) ...`; local source →
`#stage` via the staging seam, then MERGE against it. Clause folding to
T-SQL's limits (≤2 MATCHED etc.) is mechanically possible (CASE dispatch,
NULL-careful first-match-wins negation); non-mappable: ERROR actions, INSERT
under MATCHED/BY SOURCE — those stay on phase 1. **Contract decision
required:** duplicate source matches — T-SQL errors (8672) where DuckDB
silently applies an arbitrary row (verified on our binary). Options:
document the divergence / pre-dedup with ROW_NUMBER / refuse pushdown when
provably possible. Also: HOLDLOCK policy, RETURNING↔OUTPUT mapping
(`CASE WHEN $action='DELETE' THEN deleted.c ELSE inserted.c END`; error 334
on triggered targets → `OUTPUT INTO`).

### 3.3 #239 — materialize own-catalog scans, better than the reference

duckdb-postgres's mechanism, verified: a physical-plan walk in the DML/CTAS
planners flips `requires_materialization / max_threads=1 /
can_use_main_thread` on every postgres scan's bind data; the scan's
InitGlobalState then drains the whole source into a `ColumnDataCollection`
before execution — the connection is free before the sink starts. Two
improvements available to us (they answer the issue's cost question): flag
only scans whose catalog IS the sink's catalog (postgres flags all), and only
when the conflict is real (pinned transaction connection) — autocommit pays
nothing. COPY has no catalog hook, but `CopyFunction::plan`
(`bind_copy.cpp:90-92`) hands the whole statement plan to the copy function
at bind time — enough reach, at the cost of owning the COPY plan shape.
This also fixes the latent INSERT-in-transaction collision (§2) and is the
prerequisite for MERGE/UPDATE with same-catalog sources in transactions.

### 3.4 Read-side pushdown (later phases)

Spec 061 (collation-aware ORDER BY) is drafted and small: an
`OrderIsCodePointStable` predicate over `_BIN2` collations (already carried
in the catalog since spec 060), a gate beside the existing spec-039
NULL-order check, an `off/safe/always` setting, order-asserting tests. It is
the natural opener of the pushdown phase; JOIN and simple aggregates need
their own reconnaissance (the spec-039 `MSSQLOptimizer` extension is the
precedent and insertion point) — not scoped here.

---

## 4. Proposed spec breakdown and order

| # | Spec | Contents | Depends on | Closes |
|---|------|----------|-----------|--------|
| 0 | release v0.2.3 | version bump, release notes 054→064, final report R of the SIMD series | #241 merged | — |
| 1 | **065 — direct UPDATE/DELETE** | 3-arg Plan hooks, fully-pushed predicate, SET translation (+CAST→CONVERT), childless executor, conditional #140 guard, token-loop consolidation, retire dead `mssql_dml_use_prepared` | nothing | **#140** |
| 2 | **062 — INSERT via BCP** (existing draft) | + the shared held-connection bulk session (the seam from §1.3); its open D3 is already answered by 063's infrastructure | 063 (done) | — |
| 3 | **066 — UPDATE/DELETE staging** | columnar buffering, stage lifecycle (unique names, explicit DROP), `UPDATE/DELETE ... JOIN #stage`, decision function vs VALUES join | 062's seam | — |
| 4 | **067 — #239 materialization** | plan-walk + scan drain + `CopyFunction::plan` for COPY; catalog-scoped and transaction-gated | nothing (unlocks 5) | **#239** |
| 5 | **068 — MERGE phase 1** | `PlanMergeInto` composition over existing sinks; upsert unlock | 1 (cleaner sinks) | MERGE + ON CONFLICT |
| 6 | **069 — MERGE pushdown** | same-catalog source → one T-SQL MERGE; staged source via seam; clause folding; 8672/HOLDLOCK contract | 3, 4, 5 | — |
| 7 | **061 + JOIN/agg recon** | ORDER BY safe-pushdown; then JOIN/aggregate investigation | — | — |

Rationale for the order: (1) the direct path is standalone, fixes the
loudest issue (#140), and delivers the most dramatic user-visible win
(a filtered UPDATE of 1M rows today: scan 1M rowids + ship 1M value tuples;
after: one statement). (2)+(3) share the seam — 062 builds it, 066 consumes
it. (4) is planner-only, isolated, and gates (6). MERGE lands in two
reviewable steps.

## 5. Risks and contracts to settle in the specs

1. **Unencodable-filter discipline** (direct path): hard bail-out, never
   drop. Needs a test that plants an unencodable filter and asserts the
   rowid path ran.
2. **SET-expression semantics**: the function-mapping table was written for
   scan predicates (wrong rows read = visible); for SET it decides what gets
   WRITTEN. Each mapped function needs a T-SQL-equivalence check before the
   direct path may use it; start with column refs + constants + arithmetic,
   widen deliberately.
3. **MERGE duplicate-source contract** (8672) — decide and test all three
   behaviors (DuckDB local, pushdown, client fallback).
4. **Stage lifetime**: unique naming, explicit DROP, hold-don't-release;
   `mssql_reset_connection=false` turns a leaked stage into pool-wide
   pollution.
5. **Memory in transactions**: staged DML defers whole affected sets to
   Finalize (as today) — document; spillable DataChunk buffering is the
   mitigation if it bites.
6. **Error attribution**: staged join errors surface at finalize with less
   row context than per-batch text statements (same trade spec 062 §5 makes).
7. Fix in passing (small, could ride spec 065): INSERT RETURNING multi-batch
   loss; `EnsurePKLoaded` error swallowing.

## 6. Source materials

Full agent reports (file:line evidence for every claim in §1–§3) are in the
session transcript; the four axes were: current DML map, DuckDB API +
duckdb-postgres reference, staging infra audit, MERGE semantics mapping.
Key reference files: `duckdb/src/execution/physical_plan/plan_update.cpp`,
`plan_merge_into.cpp`, duckdb-postgres `src/storage/postgres_update.cpp` /
`postgres_insert.cpp:428-446` (MaterializePostgresScans),
`src/include/copy/load_policy.hpp`, `specs/062-insert-via-bcp/spec.md`,
`specs/061-collation-aware-order-pushdown/spec.md`.

---

## 7. SEMANTIC CONTRACT DIRECTIVE (maintainer decision, 2026-08-05)

Settled during the join/agg reconnaissance, and it reframes the string-key
question:

**Default = NATIVE server semantics.** A user who attached a SQL Server
database expects the results SQL Server would give — comparisons follow the
column's collation and T-SQL's padding rule. This is already the shipped
contract: pushed WHERE filters are removed from the plan and never
re-checked, so `WHERE name = 'abc'` has matched `'ABC'` under CI collations
since spec 026. Pushed GROUP BY / JOIN keys follow the same rule — bare
column references, index seeks intact, which is the entire point of pushing.

**Strictness is an OPT-IN, not a gate.** The measured varbinary trick
(`GROUP BY CAST(s AS varbinary)` — byte-exact, defeats padding AND collation,
verified live 2026-08-05) becomes the implementation of an explicit marker —
an annotation in the spirit of spec 060's extension types
(`col::MSSQL_VARCHAR_STRICT` or a collation-annotated variant), NOT the
default. It costs index seeks, which for JOIN relocation defeats the purpose;
for pushed-down aggregation over a full scan it merely trades seek for scan
(hash aggregate scans anyway), so strict-mode GROUP BY remains usable.

**Consequences to carry into the specs:**
- The BIN2 predicate machinery (spec 061 D1) stops being a correctness GATE
  and becomes strict-mode's cheap path (BIN2 keys need no varbinary cast —
  modulo the padding caveat below).
- Spec 061's own default deserves re-examination under this frame: it was
  written with "DuckDB semantics are the contract" and ships `off`; the
  directive implies native-semantics ORDER BY pushdown could default ON,
  with `strict` as the opt-in. One coherent knob across ORDER/GROUP/JOIN
  (e.g. `mssql_pushdown_semantics = native | strict`) beats three.
- NEW FINDING feeding 061: T-SQL pads trailing spaces in comparisons EVEN
  under `_BIN2` (verified: `'a' = 'a '` TRUE, GROUP BY merges them, only
  LIKE does not pad). 061's "BIN2 is code-point-stable" claim holds for
  ordering of pad-equal values only up to tie-order; for columns that can
  carry trailing spaces the strict path needs the varbinary form even on
  BIN2 columns.
- Numeric aggregate wrappers stay mandatory regardless of the directive
  (they fix VALUES, not comparison semantics): `SUM(int)` must ship as
  `SUM(CAST(x AS bigint))` (8115 overflow verified), `AVG(int)` as
  `AVG(CAST(x AS float))` (integer truncation verified: 1 vs 1.5).
