# Spec 066 — Materialize own-catalog scans before a sink (#239)

**Status:** Draft (step 1 of the v0.3.0 order — promoted ahead of the DML
rework, 2026-08-06)
**Reference:** duckdb-postgres `MaterializePostgresScans`
(`src/storage/postgres_insert.cpp:428-446`) + our recon
(`specs/065-dml-pushdown-recon/research.md` §3.3)
**Closes:** #239; also fixes the latent same-catalog
`INSERT ... SELECT`-in-transaction collision found in the recon

---

## 1. The problem, precisely

Inside an explicit transaction every statement runs on the ONE pinned
connection, and that connection cannot stream a result set and take other
traffic at once (spec 057). So any plan where a sink WRITES to the catalog
while its source READS the same catalog collides:

| shape | today |
|---|---|
| `INSERT INTO ms.t SELECT ... FROM ms.u` in a transaction | **latent collision** — INSERT has no defer guard (recon §2); the sink executes mid-scan on the connection the scan is streaming on |
| `COPY (SELECT ... FROM ms.u) TO 'ms.dbo.t'` in a transaction | documented limitation (README): user must restructure |
| `UPDATE/DELETE ms.t ... ` in a transaction | works, via the defer machinery: every row buffered as Values until Finalize — unbounded memory, code duplicated per executor |
| Any of the above in AUTOCOMMIT | fine — the scan streams on its own pooled connection |

duckdb-postgres solves this in the planner: before the sink starts, scans of
its own catalog are drained into a `ColumnDataCollection`, so the connection
is free by the time the sink needs it. It pays that price UNCONDITIONALLY;
we can gate it (see D2) so autocommit keeps streaming.

## 2. Decisions

### D1 — the mechanism (mirrors the reference, adapted)

- `MSSQLCatalogScanBindData` gains `requires_materialization` (+ the
  serialize/deserialize fields — the common-subplan rule).
- When set, `TableScanInitGlobal` drains the entire result into a
  `ColumnDataCollection` before returning; execution serves chunks from the
  collection and the scan's connection is released at drain end (reset per
  the normal release path). `MaxThreads` returns 1 while materializing.
- A plan-walk helper `MaterializeOwnScans(PhysicalOperator &root, const
  string &catalog_name)` recurses `op.children`, and for every
  `PhysicalTableScan` whose function is ours AND whose bind data's
  `context_name` equals the sink's catalog, sets the flag. (Improvement over
  the reference, which flags every postgres scan regardless of catalog.)

### D2 — the gate: only where a conflict is real (two of them exist)

A scan is flagged when EITHER holds:

1. **Connection conflict**: the sink's `ClientContext` is inside an explicit
   transaction (`!IsAutoCommit() && IsInTransaction` — the DML executors'
   test) AND the scan's catalog is the sink's catalog. One pinned
   connection cannot stream and take the sink's traffic at once.
2. **Self-reference (Halloween)**: the scan reads the SAME TABLE the sink
   writes — in ANY commit mode. In autocommit the scan streams on session A
   while the sink mutates the table from session B; under READ COMMITTED
   the streaming SELECT can observe the sink's own writes (re-read moved
   rows, or feed an INSERT its own output). This is the hazard that makes
   duckdb-postgres materialize unconditionally; we name it and gate on it
   instead. It also covers today's autocommit UPDATE/DELETE mid-scan batch
   flushing against the scanned table.

Everything else in autocommit is untouched: source scans keep streaming on
pool connections, zero new buffering. This answers the cost question issue
#239 itself raises — postgres pays always, we pay only where one of the two
conflicts is provable at plan time.

### D3 — call sites

- `MSSQLCatalog::PlanInsert` — fixes the latent collision.
- `MSSQLCatalog::PlanCreateTableAs` — CTAS reads transactionally on the
  pinned connection while loading via pool; drained-first is also the shape
  MERGE will need.
- `MSSQLCatalog::PlanUpdate` / `PlanDelete` (4-arg, existing) — makes the
  child's rowid stream drain before Sink ever runs, which is what lets the
  DEFER MACHINERY RETIRE: with the scan guaranteed complete, the executors
  can flush batches during Sink in transactions exactly as they do in
  autocommit. Retiring the buffers themselves is left to spec 065/staging
  (this spec only guarantees the property), EXCEPT the guard: `defer_execution_`
  can flip to `false` once materialization is in — covered by a test here,
  removed there.
- **COPY**: `CopyFunction::plan` (`copy_to_plan_t`, bind-time whole-plan
  hook — `duckdb/src/planner/binder/statement/bind_copy.cpp:90-92`) is the
  only reach COPY gives us. Phase decision: implement it here if the hook's
  cost (owning the COPY plan shape) proves small; otherwise document COPY as
  the one remaining restructure case and finish it in a follow-up. The
  investigation task T0 below decides.

### D4 — memory honesty

Materialization buffers the full source result client-side — the SAME
memory the UPDATE/DELETE defer path already spends today, now in a
`ColumnDataCollection` (columnar, spillable by DuckDB's own buffer manager)
instead of `vector<vector<Value>>`. Document the equivalence; no new limit
setting in v1 (the collection spills; the Value buffers never did).

### D5 — the materialization VEHICLE: client collection vs server #stage

Where the drained rows LIVE depends on what is legal, and for large results
the server is the better buffer (maintainer question, 2026-08-06: "if there
are millions of rows — fill a temp table in batches and mutate once?" —
yes, and it falls out of the connection rules):

The vehicle splits by OPERATION CLASS (maintainer boundary, 2026-08-06):
what a mutation stages is AUXILIARY data (rowids + SET values) and the
mutation itself only exists as a statement — staging is inherent; what a
load ships IS the payload, and `INSERT BULK` lands it in the target
directly — routing it through a stage would push the full volume through
the server TWICE. Client-side spill is cheaper than a server-side double
write.

**Load class — INSERT (spec 062) / COPY / CTAS: always direct `INSERT BULK`
into the target.** When the gate demands materialization (transaction +
same catalog, or self-reference), the source drains into a client
`ColumnDataCollection` (spillable via DuckDB's buffer manager), then the
bulk load runs single-pass into the target — on the pinned connection in a
transaction (INSERT/COPY; CTAS stays on pool connections as today), on pool
connections otherwise.

**Mutation class — UPDATE / DELETE (and the MERGE source later): staged.**

| case | vehicle | client memory | final mutation |
|---|---|---|---|
| autocommit | stream the rowid scan (conn A) → BCP into `##stage_<uuid>` (conn B) | O(chunk) | `UPDATE/DELETE ... JOIN ##stage`, per the application modes below |
| explicit transaction | stream scan (pinned A) → BCP into `##stage_<uuid>` (pool conn B) — the no-second-connection rule bound TARGET rows; a global temp is scratch, its rows never roll back, and it IS visible to the pinned connection. UUID answers namespace collisions; B held until the mutation completes; reset-on-release is the error-path cleanup. Fallback to client CDC when the pool cannot grant B, below the small-result threshold, or via opt-out (a `##` table is briefly server-visible to any session) | O(chunk) | ONE (or batched) statement on pinned A, inside the transaction, then DROP + release B |
| small results (either mode) | client CDC / today's VALUES join — below a threshold the stage round-trip is not worth it (the spec-062 D1 decision-function shape) | small | VALUES join as today |

**Application modes** (maintainer refinement, 2026-08-06): the stage can be
applied two ways, and BOTH exist:

- **Incremental (autocommit default)** — the user's pattern: `CREATE #stage`
  once, then loop { BCP ~N rows → `UPDATE/DELETE ... JOIN #stage` →
  `TRUNCATE TABLE #stage` }. This is today's VALUES-join batching with a
  ~200× larger, index-joinable batch and bulk delivery instead of literal
  SQL — and it PRESERVES today's semantics exactly, warts included:
  incremental application on failure, and the READ COMMITTED ambiguity of
  mutating `t` while its scan still streams (identical to today's mid-scan
  flushing — inherited, not introduced). Bounded tempdb, short statements
  (no lock escalation, small log transactions), progress on failure.
  Batch size is its own knob (staging batches are statement granularity;
  `mssql_copy_flush_rows` remains the wire batch INSIDE each BCP stream) —
  default in the ~100k range, decided by measurement in the staging spec.
- **Pipelined incremental (the autocommit shape of the above; maintainer
  refinement)** — the roles separate onto their own connections: the SCAN
  streams on A, a DATA connection B fills `##stage` tables, a DML
  connection C runs the JOIN batches — ping-pong two stages so load(N+1)
  overlaps update(N). Wall time tends to max(scan, load, mutate) instead of
  their sum. Costs 2–3 pool connections: degrade gracefully to the serial
  single-connection loop when the pool cannot grant them. In a transaction
  the DML leg is pinned to A, so the overlap is scan∥load with DML batches
  after the drain — smaller but still real.
- **Single-statement (transaction mode; autocommit opt-in)** — stage
  everything, mutate once. In an explicit transaction this is the natural
  shape (atomicity comes from the transaction; the pinned connection does
  minimal work); in autocommit it is the opt-in for users who want
  one-statement point-in-time semantics over incremental progress.

Notes:
- "Batches" come free at the wire level either way: a `#stage` fill is a
  continuous `INSERT BULK` stream whose server-visible batch boundaries are
  `mssql_copy_flush_rows` (102 400).
- The single-writer-on-a-GIVEN-connection seam (research.md §1.3) is the
  prerequisite for every stage cell above — one more consumer for it.
- With `##stage`, the DEFER MACHINERY RETIRES EVERYWHERE — the transaction
  case streams too. Client CDC survives only as the fallback (pool
  exhaustion / small results / opt-out), which flips D4's memory story:
  bounded is the norm, spillable-full-result the exception.
- The spec-063-era objection to `##` ("collides across concurrent merges
  and is dropped by reset") was about `##` as a load TARGET; for scratch
  staging the UUID name answers the first and the reset-drop is exactly the
  wanted failure-mode cleanup.
- The final `UPDATE ... JOIN #stage` over millions of rows is one server
  statement and one log transaction; if transaction-log pressure ever
  demands chunked mutation, `TOP (N)`-loop batching of the JOIN is the
  server-side lever — recorded as an open question, not v1.
- v1 SCOPE of this spec stays the mechanism + CDC vehicle; the `#stage`
  vehicle lands with the staging/062 specs that build the seam — but the
  gate and the vehicle table are decided HERE so those specs implement
  against a settled design.

## 3. Tasks

- **T0** — probe `CopyFunction::plan`: minimal prototype that reproduces the
  default COPY plan + the walk; measure blast radius (what else the default
  plan builder does that we would now own). Decides D3's COPY phase.
- T1 — bind-data flag + drain in InitGlobal + serialize; unit-level test
  with a large result (spill path exercised).
- T2 — the walk + gate; wire into PlanInsert/PlanCTAS/PlanUpdate/PlanDelete.
- T3 — tests:
  1. transaction `INSERT INTO ms.t SELECT FROM ms.u` — works (today's
     latent collision becomes the headline test);
  2. transaction `COPY (SELECT FROM ms.u) TO ms.t` — works if T0 lands COPY,
     else keeps the documented error;
  3. transaction UPDATE with `defer_execution_ = false` — works and
     round-trips (proves the retirement property for 065);
  4. autocommit, DIFFERENT tables: counters/plan assert NO materialization
     happened (gate bite test #1);
  4a. autocommit, SELF-reference (`INSERT INTO t SELECT FROM t WHERE ...`):
     materialization DOES happen, and the insert is not fed its own output
     (row count exactly the pre-statement match count — the Halloween
     test);
  5. rollback: materialized-source INSERT rolls back atomically;
  6. same-alias-different-catalog: two ATTACHes of one server — scan of
     alias A feeding a sink into alias B must NOT materialize (context_name
     inequality — the gate's second bite test).
- T4 — README/docs: the "reading and writing the same catalog in one
  transaction" limitation text updated (removed or narrowed to COPY per T0).

## 4. Acceptance

1. The three transaction shapes (INSERT-SELECT, UPDATE, CTAS source) run
   green with sources in the same catalog; #239 closes.
2. Autocommit plans show zero behavior/perf change (interleaved A/B on the
   read bench — within noise).
3. Full suite green; serialize/deserialize covered (common-subplan test
   pattern from #211).
