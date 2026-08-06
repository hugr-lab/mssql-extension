# Spec 065 — Direct UPDATE/DELETE: one statement when everything pushes down

**Status:** Draft (recon complete — see `research.md`; this is step 1 of the
v0.3.0 order settled there)
**Date:** 2026-08-06
**Depends on:** nothing new (the plan hooks exist in the pinned DuckDB and in
v1.4.4 — verified, no compat shim)
**Closes:** #140 (UPDATE/DELETE on tables without a primary key — for every
pushable statement)

---

## 1. Why

`UPDATE mssql.dbo.t SET x = 1 WHERE region = 'EU'` today: the scan streams
every matching row's rowid to the client, the executor buffers them as
`vector<vector<Value>>`, and ships them BACK as inline-literal
`UPDATE ... JOIN (VALUES ...)` batches of 500. A filtered update of 1M rows
crosses the wire twice for no reason the plan requires: the WHERE clause was
already translated to T-SQL — *for the scan*.

And the rowid machinery is why #140 exists at all: no primary key → no rowid
→ a `BinderException` before planning even starts, even for statements that
never needed a rowid.

## 2. The design (from research.md §1.1, §3.1)

### D1 — intercept before the child is planned

Override the **3-arg** `PlanUpdate` / `PlanDelete`
(`duckdb/src/include/duckdb/catalog/catalog.hpp:314,:317` — the overload the
generator actually calls; its default plans the child and forwards to our
existing 4-arg hooks). The override inspects the INTACT logical child and
either:

- emits a **childless** `MSSQLPhysicalDirectDML` operator (no scan is ever
  planned), or
- falls through to the base default — today's rowid path, byte-for-byte.

Inspect before `CreatePlan` (physical planning `std::move`s state out of
logical nodes).

### D2 — the fully-pushed predicate

The direct path is taken **only** when ALL of:

1. Chain shape: UPDATE → `LogicalProjection → LogicalGet`, DELETE →
   `LogicalGet` — no residual `LogicalFilter`, no join/cross-product (binder
   guarantees the base shape; anything extra means DuckDB-side work exists).
2. `get.GetTable() == &op.table` (the scan reads the target itself).
3. A plan-time dry-run of `FilterEncoder::Encode` over `get.table_filters`
   reports `needs_duckdb_filter == false`, i.e. every table filter has a
   T-SQL rendering. **Any unencodable filter is a hard bail-out to the rowid
   path — never dropped.** (The scan may drop OPTIONAL/DYNAMIC filters —
   extra rows read is harmless; extra rows WRITTEN is not. research.md §2.)
4. The bind data's `complex_filter_where_clause` (already text, encoded at
   optimizer time) is appended as-is; `order_by_clause`/`top_n` present →
   bail (no ordered/limited DML semantics to preserve).
5. UPDATE only: every SET expression in the projection renders through
   `EncodeExpression` with a context built from the target's full column
   list. Start conservative: column refs, constants, arithmetic, CASE,
   BETWEEN, the mapped functions — each already vetted for predicate use.
   `BOUND_DEFAULT` → bail (server DEFAULT is a different value than DuckDB's
   NULL-default). Any `supported == false` → bail.

Semantics note (research.md §7 directive): this changes NOTHING about whose
comparison semantics apply — the scan already filters server-side with the
same encoded WHERE. Direct DML writes exactly the rows today's plan reads.

### D3 — the executor and the shared token loop

`UPDATE [s].[t] SET [c] = <expr>, ... WHERE <where>` /
`DELETE FROM [s].[t] WHERE <where>` as one SQL_BATCH on the
ConnectionProvider connection (pinned inside transactions — no scan is
running, so no defer machinery, no buffering). Affected count from the DONE
token → the single-BIGINT "Count" row `op.types` expects.

The raw token loop (send batch → collect DONE counts → drain errors) exists
in **triplicate** across the update/delete/insert executors
(mssql_update_executor.cpp:246-329 and friends). D3 extracts ONE
`ExecuteDmlBatch` helper and points all four call sites (three old + direct)
at it. Pure consolidation, no behavior change — and the direct path's
executor is then ~50 lines.

### D4 — #140: move the guard from bind to plan

- `MSSQLTableEntry::BindUpdateConstraints` stops throwing for PK-less
  tables; `GetRowIdColumns` returns an **empty vector** for them (legal:
  binds zero rowid columns; safe because both Plan hooks are ours —
  research.md §1.4).
- At plan time: fully-pushed → direct path (no rowid needed); NOT fully
  pushed AND no PK → the error moves here, naming both outs:
  `"UPDATE/DELETE on '%s' requires either a primary key (row
  identification) or a fully pushable WHERE clause. Unpushable: <first
  unencodable filter/SET>"`.
- MERGE keeps the bind-time PK requirement (DuckDB's MERGE binder needs
  rowid for match classification — upstream, out of scope).

### D5 — riding cleanups

- Retire `mssql_dml_use_prepared` (loaded, never read — dead since
  inception; research.md §2).
- `EnsurePKLoaded` stops converting discovery errors into "no PK"
  (mssql_table_entry.cpp:321-324): a connection hiccup currently degrades a
  table to PK-less until cache invalidation — with D4 that silently changes
  which PATH a statement takes; rethrow instead.

### D6 — setting

`mssql_direct_dml` (BOOLEAN, default **true**): the kill switch. Not a
semantics toggle — the direct path preserves today's row selection (D2
note) — but new machinery deserves a diagnosis lever, and `false` restores
the rowid path exactly.

## 3. Explicitly out of scope (later steps of the v0.3.0 order)

- Staging via `#temp` for the NOT-fully-pushable case (spec 066) — D2's
  bail-out lands on today's VALUES-join path unchanged.
- INSERT via BCP (spec 062), MERGE (068/069), materialization (#239 / 067).
- RETURNING for UPDATE/DELETE (`op.return_chunk` is ignored today and stays
  ignored; OUTPUT-based RETURNING is future work noted in research.md §7).
- The INSERT RETURNING multi-batch loss (research.md §2) — real bug, its own
  small fix/PR, not this spec.

## 4. Tests

1. **The bite test (mirrors both_paths_agree's lever):** a statement with an
   unencodable filter (e.g. `WHERE list_contains(...)` shape or an
   OPTIONAL_FILTER-generating pattern) must take the ROWID path — assert via
   the DML counters (extend `MSSQL_COUNTERS` with `direct_dml` /
   `rowid_dml` statement counters; SQL-invisible paths are how both-paths
   suites go vacuous — spec 063's lesson).
2. Direct-path correctness: filtered UPDATE/DELETE round-trip vs expected
   row sets, including: multi-column SET with expressions, CASE in SET,
   mapped functions in WHERE, empty match (0 rows), full-table (no WHERE).
3. Count fidelity: DuckDB-reported count == server-affected rows, both
   paths.
4. Transactions: direct statement inside BEGIN/COMMIT/ROLLBACK on the pinned
   connection; rollback undoes it; mixed with reads in the same transaction.
5. **#140 shapes:** PK-less table + pushable WHERE → works (both UPDATE and
   DELETE); PK-less + unpushable → the D4 error text; PK-less DELETE without
   WHERE → works (full-table delete is trivially fully-pushed).
6. `mssql_direct_dml = false` → rowid path for everything (counter-assert).
7. Existing DML suite unchanged — the bail-out path is byte-identical.

## 5. Acceptance criteria

1. `UPDATE t SET x = 1 WHERE <pushable>` on a 1M-row match: no scan
   round-trip (verify: counters show direct; wall/CPU measured before/after
   on the wide fixture — expect the update cost to collapse to the server's
   own statement time).
2. #140's reproduction passes; the unpushable-without-PK error names the
   offending expression.
3. Full suite green; `both_paths_agree` untouched; no change to any
   RETURNING behavior.
4. The token loop exists once, not four times.
