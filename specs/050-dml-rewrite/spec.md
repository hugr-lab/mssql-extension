# Feature Specification: DML Rewrite (UPDATE/DELETE + MERGE Guard)

**Feature Branch**: `050-dml-rewrite`

**Created**: 2026-05-22

**Status**: Draft

**Input**: User description: "Take the design extracted in `feature-spec/refactoring-dml-050.md`. Full rewrite of the UPDATE/DELETE pipeline. Catalog enrichment with rowid-strategy detection (PK or `%%physloc%%`). Direct DML pushdown for simple plans. Session temp staging tables for the general case (uploaded via the pipelined BCP path from spec 046, with DDL/literal types from spec 044). MERGE temporarily disabled with a clear NotImplementedException pointing users at the INSERT/UPDATE workaround pattern."

## Overview

As of v0.1.18 the UPDATE / DELETE / MERGE pipeline in
`hugr-lab/mssql-extension` has four documented correctness and
throughput problems (catalogued in `feature-spec/refactoring-v0.2.md`
§2):

1. UPDATE/DELETE route through a rowid-fetch + per-batch `UPDATE …
   FROM (VALUES …)` path that requires a primary key on every target
   table. Tables without a PK cannot be modified at all.
2. The `VALUES`-based rowid join hits SQL Server's 1000-row `VALUES`
   limit, so every operation > 1000 rows fans out into multiple
   round-trips with re-escaped string literals.
3. There is no BCP fast path for staging the rowid set; UPDATE values
   are re-escaped per row, killing throughput on remote SQL Server.
4. MERGE is routed through the same broken rowid path: `WHEN NOT
   MATCHED` rows have no rowid (silent data loss / incorrect results),
   `WHEN NOT MATCHED BY SOURCE` is unsupported, and `RETURNING
   merge_action` is dropped.

Spec 050 rewrites the pipeline to a postgres-extension-style
staging-table architecture with a direct-pushdown fast path for
structurally simple plans, adds catalog-level capability detection
(primary key + `%%physloc%%` per table; engine-edition per connection),
and **disables MERGE** with a clear `NotImplementedException` that
documents the migration pattern. Proper MERGE support is deferred to a
follow-up spec — the current behavior is worse than no behavior because
it silently corrupts results.

This is the third refactoring spec in the v0.2.0 series and the
direct continuation of spec 044's codec layer (used here for WHERE-
clause literals and `CREATE TABLE` DDL) and spec 046's pipelined BCP
(used here for staging-table upload).

Explicitly **not in scope** (despite the source doc historically
proposing them, and despite user pressure to bundle them):

- **MERGE INTO server-side execution.** Replaced with an
  `NotImplementedException`-throwing guard that lists the
  INSERT/UPDATE migration pattern. Server-side MERGE returns in a
  later spec after the staging path is stable.
- **`ON CONFLICT` / upsert clauses.** Already absent in v0.1.x; depend
  on real MERGE.
- **Parallel scan with rowid emission.** Single-threaded rowid scan is
  the same trade-off `postgres-extension` makes; parallelization is a
  separate optimization spec.
- **Chunked-commit large DML.** Trades atomicity for tempdb pressure;
  atomicity is the v0.2.0 commitment.
- **Expanded direct-path eligibility check.** v0.2.0 ships with a
  conservative whitelist (`LogicalGet`, `LogicalFilter`,
  `LogicalProjection`); expansion is data-driven by real query
  shapes from production.
- **`%%physloc%%` concurrency hardening** (auto `SERIALIZABLE`,
  `TABLOCKX` hint). Documented caveat only; users self-manage
  isolation for now.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Catalog enrichment with rowid-strategy detection (Priority: P1) 🎯 MVP

As a DuckDB user attaching an MSSQL catalog, I want the extension to
detect each table's DML capabilities (primary key + `%%physloc%%`
support) at catalog load, so that subsequent UPDATE / DELETE planning
can pick the correct rowid strategy without surprises and so that
unsupported targets fail at plan time with a clear message — not
mid-execution with a confusing T-SQL error.

**Why this priority**: Every other story depends on this. Without
capability detection, neither the direct nor the staging path can
choose a rowid strategy, and we cannot fail fast on Synapse Dedicated /
Fabric Warehouse tables that lack PK + physloc.

**Independent Test**: Attach a SQL Server, query
`mssql_table_capabilities('schema.table')` (new diagnostic table
function — or equivalent — wired only for testing) and assert the
correct `has_primary_key`, `supports_physloc`, and
`physloc_unavailable_reason` for every table type in the fixture
(heap, clustered B-tree, CCI, NCCI, memory-optimized, external,
columnstore on Synapse).

**Acceptance Scenarios**:

1. **Given** a SQL Server 2022 instance with a heap table `T_HEAP`
   without primary key, **When** the extension loads the table's
   metadata, **Then** `has_primary_key = false`, `supports_physloc =
   true`, `physloc_unavailable_reason = ""`.
2. **Given** the same instance with a clustered-columnstore table
   `T_CCI`, **When** metadata loads, **Then** `supports_physloc =
   false`, `physloc_unavailable_reason = "table has clustered
   columnstore index"`.
3. **Given** a connection to Azure Synapse Dedicated (EngineEdition
   6), **When** any table is loaded, **Then** `supports_physloc =
   false`, `physloc_unavailable_reason = "engine is Azure Synapse
   Dedicated (no physloc support)"`.
4. **Given** the engine-edition probe issued once per connection,
   **When** the same connection is reused, **Then** the probe is not
   re-issued (cached on connection state).

---

### User Story 2 - Direct UPDATE/DELETE pushdown for simple plans (Priority: P1)

As an analyst issuing a structurally simple DML statement (no joins,
no subqueries, only the target table and pushable filters), I want a
single round-trip pushdown instead of the legacy rowid-fetch +
staged update, so that simple targeted edits run at native SQL Server
latency.

**Why this priority**: Direct path is the optimization that makes
simple operations feel native. It also exercises the
filter-pushability boundary against the spec 044 codec, which
otherwise has no DML-side coverage.

**Independent Test**: `DELETE FROM mssql.dbo.t WHERE id = 5` against
an instrumented connection shows exactly one outbound SQL_BATCH and
zero preceding rowid SELECTs. Same for
`UPDATE mssql.dbo.t SET name = 'x' WHERE id = 5`. Both update
`@@ROWCOUNT` correctly and stream `RETURNING` rows via the OUTPUT
clause when requested.

**Acceptance Scenarios**:

1. **Given** plan tree `LogicalDelete → LogicalFilter →
   LogicalGet(target)` where the filter is fully pushable, **When**
   the operator runs, **Then** the extension emits exactly one
   `DELETE FROM target WHERE <pushdown>` statement.
2. **Given** the same shape with a non-pushable filter expression
   (e.g. a UDF call), **When** planning, **Then** the direct path
   declines and the staging path is selected.
3. **Given** `UPDATE target SET col1 = 'x', col2 = 5 WHERE id = 7`,
   **When** the operator runs, **Then** the extension emits exactly
   one `UPDATE target SET … WHERE id = 7` and no rowid SELECT.
4. **Given** either operator with `RETURNING *`, **When** it runs,
   **Then** the emitted statement carries `OUTPUT
   DELETED.*`/`OUTPUT INSERTED.*` and the returned rows reach DuckDB
   intact.
5. **Given** session setting `mssql_direct_dml = false`, **When**
   the same simple plan is executed, **Then** the extension forces
   the staging path (debug aid).

---

### User Story 3 - Staging-path UPDATE/DELETE with both rowid strategies (Priority: P1)

As an ETL author running UPDATE / DELETE statements that join the
target with DuckDB-side or other-catalog data, I want the extension
to (a) materialize rowids from the target scan, (b) BCP-upload them
into a session temp table, and (c) execute a single `UPDATE … FROM …
JOIN #upd_xxx` (or `DELETE … FROM … JOIN #upd_xxx`) against the
server — choosing PK or `%%physloc%%` automatically based on what
the target supports.

**Why this priority**: This is the path the majority of real-world
DML hits. It also unlocks UPDATE/DELETE on PK-less tables (heaps,
analytical tables) that v0.1.x cannot handle.

**Independent Test**: A test that joins an MSSQL target table to a
DuckDB-local table inside an UPDATE statement (a) succeeds, (b) shows
one BCP stream + one UPDATE…FROM…JOIN statement on the wire, (c)
returns correct rowcounts, (d) drops the staging table on success.

**Acceptance Scenarios**:

1. **Given** a target with primary key and a non-simple plan (any
   join, subquery, aggregation, set op, multiple `LogicalGet`),
   **When** the operator runs, **Then** a session temp table
   `#upd_<uuid>` is created with the PK columns (+ updated columns
   for UPDATE), populated via BCP, joined back to the target in one
   statement, and dropped.
2. **Given** a heap target without primary key on an engine that
   supports `%%physloc%%`, **When** the operator runs with
   `mssql_rowid_strategy = 'auto'`, **Then** the staging table's
   rowid column is `__physloc VARBINARY(8)`, the scan SELECT list
   carries `%%physloc%% AS __physloc`, and the JOIN predicate is
   `target.%%physloc%% = s.__physloc`.
3. **Given** `mssql_rowid_strategy = 'pk'` against a table without
   primary key, **When** the operator plans, **Then** the planner
   throws `BinderException` naming the missing PK.
4. **Given** `mssql_rowid_strategy = 'physloc'` against a CCI
   table, **When** the operator plans, **Then** the planner throws
   `BinderException` naming the reason (clustered columnstore
   index).
5. **Given** the scan operator with `emit_rowid = true`, **When**
   it runs, **Then** the scan is single-threaded
   (`max_threads = 1`) and emits the rowid column alongside the
   normal output columns.
6. **Given** either staging operator with `RETURNING`, **When** it
   runs, **Then** `OUTPUT INSERTED.*` / `OUTPUT DELETED.*` is
   appended to the JOIN statement and the rows reach DuckDB.

---

### User Story 4 - Transaction wrap (autocommit, savepoint, DTC error) (Priority: P1)

As a user who mixes ad-hoc DML with explicit `BEGIN
TRANSACTION` blocks, I want each staging-path DML operation to be
atomic: either it commits in full (in autocommit mode) or it can be
rolled back via savepoint without disturbing my outer transaction.

**Why this priority**: Atomicity is the contract that makes the
staging path safe. Without it, a failed UPDATE leaves partial
changes (staging table or target rows) behind.

**Independent Test**: Three tests: (a) autocommit failure injects
mid-UPDATE → target table unchanged, no `#upd_*` left behind;
(b) inside-user-transaction failure → savepoint rolls back our DML
only, outer transaction still active and committable; (c) inside
a distributed (DTC) transaction → operator throws
`NotImplementedException` at plan time.

**Acceptance Scenarios**:

1. **Given** `@@TRANCOUNT = 0` at Sink-phase start, **When** the
   staging operator runs, **Then** it issues `BEGIN TRANSACTION` +
   `SET XACT_ABORT ON`, executes the staging workflow, and issues
   `COMMIT` on success or `ROLLBACK` on failure.
2. **Given** `@@TRANCOUNT > 0` (user is in an explicit
   transaction), **When** the staging operator runs, **Then** it
   issues `SAVE TRANSACTION mssql_dml_<uuid>` (UUID truncated to
   the 32-char savepoint name limit), executes the workflow, and on
   failure issues `ROLLBACK TRANSACTION mssql_dml_<uuid>` — the
   outer transaction remains active.
3. **Given** `XACT_STATE() = 2` (active distributed transaction),
   **When** the staging operator plans, **Then** it throws
   `NotImplementedException` with a clear message.
4. **Given** two staging operators in the same user transaction,
   **When** both run, **Then** their savepoint names differ
   (UUID-suffixed) and they do not collide.
5. **Given** an error inside the staging workflow (e.g. BCP upload
   fails), **When** the operator unwinds, **Then** no `#upd_<uuid>`
   table remains visible after the connection returns to the pool
   (autocommit case: rollback wipes it; user-txn case: savepoint
   rollback wipes it; fatal connection drop: server-side session
   teardown reclaims tempdb).

---

### User Story 5 - MERGE early-error guard + RETURNING generalization (Priority: P2)

As a user, when I issue `MERGE INTO mssql.dbo.target …`, I want a
clear `NotImplementedException` at plan time listing the migration
patterns, rather than the silent data-corruption behavior of
v0.1.x. As a developer maintaining the extension, I want the
`MSSQLReturningParser` lifted out of the INSERT-only path so that
direct and staging UPDATE / DELETE can share it.

**Why this priority**: User-visible safety (no more silent
corruption) but lower urgency than the read/write paths above. The
RETURNING refactor lands together because every other story needs
it.

**Independent Test**: (a) `MERGE INTO mssql.dbo.target USING …`
throws `NotImplementedException` with the documented message and
non-zero exit code; (b) INSERT-RETURNING continues to pass its
existing test suite via the renamed parser; (c) UPDATE and DELETE
RETURNING tests added in US2/US3 pass.

**Acceptance Scenarios**:

1. **Given** any `MERGE INTO mssql.<schema>.<table>` statement,
   **When** DuckDB plans it, **Then** `MSSQLCatalog::PlanMergeInto`
   throws `NotImplementedException` whose message names the
   INSERT-WHERE-NOT-EXISTS and UPDATE-FROM-SOURCE migration
   patterns.
2. **Given** the existing INSERT RETURNING test suite, **When** the
   parser is renamed to `MSSQLDmlReturningParser` and the INSERT
   path is routed through a shim, **Then** every previously-green
   INSERT RETURNING test stays green.
3. **Given** UPDATE / DELETE RETURNING via either direct or staging
   path, **When** the operator runs, **Then** the parser returns
   the rows in DuckDB-expected column order with correct types.

---

### Edge Cases

- **`%%physloc%%` under concurrent writers**: documented caveat —
  page splits, row movement, or index rebuild between scan and DML
  invalidate physloc values. We do not auto-wrap in SERIALIZABLE in
  v0.2.0. Users on PK-less tables are advised to either add a PK or
  isolate the operation in their own SERIALIZABLE transaction.
- **TempDB pressure**: a 100M-row UPDATE with 5 updated INT columns
  + BIGINT PK ≈ 2.8 GB of staging tempdb. Documented as a sizing
  recommendation. v0.2.0 does not chunk to bound this (would lose
  atomicity).
- **Pass-through `BEGIN TRANSACTION`** issued via a scalar
  `mssql_exec(...)` call that bypasses DuckDB's transaction manager:
  detected by always probing `@@TRANCOUNT` at Sink-phase start
  rather than trusting cached connection-state flags.
- **Savepoint name length cap**: SQL Server allows max 32 chars;
  truncate the UUID hex to fit `mssql_dml_<22 hex chars>`.
- **`OUTPUT` with AFTER triggers on target**: `INSERTED.*` reflects
  post-trigger row state, per SQL Server semantics. Documented in
  the RETURNING edge cases.
- **`OUTPUT` of MAX-types (NVARCHAR(MAX), VARBINARY(MAX))** streams
  back as PLP; existing parser already handles PLP — no new work.
- **Connection drop mid-DML**: temp table reclaim is server-side
  session teardown. No client-side cleanup needed (best-effort).
- **Direct-path SET expression referring to another table**:
  ineligible for direct path; falls back to staging.

## Requirements *(mandatory)*

Functional requirements are grouped by user story. Each FR cites a
concrete artifact or behavior so it is testable.

### Catalog enrichment (US1)

- **FR-001**: On first use of a connection, the extension MUST query
  `SELECT SERVERPROPERTY('EngineEdition')` once and cache the integer
  result on the connection state. Subsequent users of the same
  connection MUST NOT re-issue the probe.
- **FR-002**: For each MSSQL table loaded into the catalog, the
  extension MUST compute a `TableDmlCapabilities` record containing
  `has_primary_key`, `pk_columns` (already present via existing
  catalog code), `supports_physloc`, and `physloc_unavailable_reason`.
- **FR-003**: `supports_physloc` MUST be `false` whenever any of:
  (a) engine edition is 6 (Synapse Dedicated) or 11 (Fabric Warehouse
  / Synapse Serverless / Fabric SQL Endpoint), (b) the table is
  memory-optimized (`sys.tables.is_memory_optimized = 1`), (c) the
  table is external (`is_external = 1`), or (d) the table has a
  clustered columnstore or NCCI base index (`sys.indexes.type IN
  (5, 6)`).
- **FR-004**: `physloc_unavailable_reason` MUST be a human-readable
  string suitable for an error message; values are enumerated in
  `data-model.md` §E2.
- **FR-005**: `MSSQLTableEntry` MUST expose `GetDmlCapabilities()`
  returning the cached `TableDmlCapabilities`.

### Direct DML (US2)

- **FR-010**: `MSSQLCatalog::PlanDelete` and `PlanUpdate` MUST
  detect a simple plan tree (`LogicalUpdate`/`LogicalDelete` →
  optional `LogicalProjection` → optional `LogicalFilter` →
  `LogicalGet(target)`, with all filters fully pushable per the spec
  044 codec) and select `MSSQLDirectDelete` / `MSSQLDirectUpdate`.
- **FR-011**: `MSSQLDirectDelete` MUST emit exactly one
  `DELETE FROM <target> WHERE <pushdown> [OUTPUT DELETED.…]`
  statement per operator execution. No preceding rowid SELECT.
- **FR-012**: `MSSQLDirectUpdate` MUST emit exactly one
  `UPDATE <target> SET <col> = <literal>, … WHERE <pushdown>
  [OUTPUT INSERTED.…]` statement per operator execution. SET
  expressions that reference other tables, subqueries, or
  non-pushable values MUST disqualify the plan from the direct
  path.
- **FR-013**: Setting `mssql_direct_dml = false` MUST force the
  staging path even for plans that qualify for the direct path.

### Staging DML (US3)

- **FR-020**: `MSSQLCatalog::PlanDelete` / `PlanUpdate` MUST select
  `MSSQLStagingDelete` / `MSSQLStagingUpdate` for any plan that
  fails the direct-path check.
- **FR-021**: The staging operator MUST resolve the rowid strategy
  via `ResolveRowIdStrategy(context, table)` per the algorithm in
  `data-model.md` §E1, honoring the
  `mssql_rowid_strategy` session setting (`auto` /
  `pk` / `physloc`).
- **FR-022**: For PK strategy, the staging table schema MUST
  contain the PK columns (DDL types via spec 044's
  `AppendDdlColumnType`); for physloc strategy, it MUST contain
  one `__physloc VARBINARY(8)` column.
- **FR-023**: For staging UPDATE, the staging table MUST additionally
  contain the updated columns with their target-type DDL.
- **FR-024**: Staging rowid (+ update values) MUST upload via the
  spec 046 pipelined BCP path; no row-by-row literal escape.
- **FR-025**: The staging operator MUST emit exactly one
  `UPDATE target SET … FROM target JOIN #upd_<uuid> s ON …
  [OUTPUT INSERTED.…]` or `DELETE target FROM target JOIN
  #upd_<uuid> s ON … [OUTPUT DELETED.…]` statement per operator
  execution.
- **FR-026**: After the JOIN statement, the operator MUST issue
  `DROP TABLE #upd_<uuid>` on success.
- **FR-027**: `MSSQLCatalog::MaterializeMssqlScans(plan, strategy)`
  MUST walk the plan tree depth-first, locate every MSSQL scan
  feeding the DML, and set `emit_rowid = true`,
  `rowid_strategy = strategy`, `requires_materialization = true`,
  `max_threads = 1`.
- **FR-028**: The scan operator with `emit_rowid = true` MUST add
  the PK columns (PK strategy) or `%%physloc%% AS __physloc`
  (physloc strategy) to its SELECT list, and project the rowid
  value into the chunk output alongside regular columns.

### Transaction wrap (US4)

- **FR-030**: At Sink-phase start of any staging operator, the
  extension MUST issue `SELECT @@TRANCOUNT` to determine whether an
  outer transaction is active. Cached `HasPinnedConnection()` state
  MUST NOT be used as the authoritative signal.
- **FR-031**: When `@@TRANCOUNT = 0`, the staging operator MUST
  wrap the workflow in `BEGIN TRANSACTION` + `SET XACT_ABORT ON`
  + `COMMIT` (success) or `ROLLBACK` (failure).
- **FR-032**: When `@@TRANCOUNT > 0`, the staging operator MUST
  wrap the workflow in `SAVE TRANSACTION mssql_dml_<uuid>` (UUID
  truncated to fit the 32-char savepoint name cap) and, on
  failure, `ROLLBACK TRANSACTION mssql_dml_<uuid>`. It MUST NOT
  issue `BEGIN`/`COMMIT` around the user's outer transaction.
- **FR-033**: The extension MUST probe `XACT_STATE()`; when the
  return is `2` (active distributed transaction), the operator MUST
  throw `NotImplementedException` with a message naming DTC as the
  unsupported case.
- **FR-034**: Two staging operators in the same user transaction
  MUST use distinct savepoint names (UUID per operator instance).
- **FR-035**: On success, the operator MUST explicitly
  `DROP TABLE #upd_<uuid>` before its commit/savepoint discard, to
  bound tempdb residence in pooled long-lived connections.

### MERGE guard + RETURNING (US5)

- **FR-040**: `MSSQLCatalog::PlanMergeInto` MUST throw
  `NotImplementedException` whose message names the
  INSERT-WHERE-NOT-EXISTS and UPDATE-FROM-SOURCE migration patterns
  and links to a future spec ID.
- **FR-041**: The existing `MSSQLReturningParser` (in
  `src/dml/insert/`) MUST be renamed to `MSSQLDmlReturningParser`
  and lifted to `src/dml/mssql_dml_returning_parser.{hpp,cpp}`. The
  new constructor takes generic column metadata
  (`vector<TdsColumnMetadata>` + returning column id vector), not
  `MSSQLInsertTarget`.
- **FR-042**: The existing INSERT path MUST continue to use the
  renamed parser via a backward-compatible shim that constructs
  the new parser from `MSSQLInsertTarget`. No INSERT external
  behavior change.
- **FR-043**: Direct and staging UPDATE / DELETE MUST consume the
  parser to surface OUTPUT rows back to DuckDB.

### Settings surface

- **FR-050**: The extension MUST register session settings
  `mssql_rowid_strategy` (string: `auto` | `pk` | `physloc`;
  default `auto`), `mssql_direct_dml` (boolean; default `true`),
  and `mssql_dml_log_transactions` (boolean; default `false`).
- **FR-051**: Setting validation MUST happen at the consumption
  site (`ResolveRowIdStrategy`, direct/staging dispatch in
  `PlanUpdate`/`PlanDelete`), not as global registration
  side-effects.

### Backward compatibility

- **FR-060**: Existing tables that previously worked under PK-based
  rowid DML MUST continue to work with no user-visible behavior
  change other than improved throughput from BCP staging.
- **FR-061**: Existing INSERT and INSERT-RETURNING tests MUST
  remain green after the parser rename.
- **FR-062**: Existing MERGE tests that exercised the broken
  v0.1.x behavior MUST be replaced by a single test that asserts
  the new `NotImplementedException` with its documented message.

### Documentation

- **FR-070**: `README.md` (and `docs/` where applicable) MUST
  document the new `mssql_rowid_strategy` and `mssql_direct_dml`
  settings, the `%%physloc%%` concurrency caveat, the
  MERGE-temporarily-disabled notice with migration patterns, and
  the tempdb sizing recommendation for large staging operations.

## Success Criteria *(mandatory)*

Measurable yardsticks for spec-050 acceptance.

- **SC-001** (Catalog detection): For every fixture table in
  `test/sql/dml/dml_capabilities.test`, the assertion that
  `has_primary_key`, `supports_physloc`, and
  `physloc_unavailable_reason` match the expected values returns
  green. The fixture covers ≥ 7 cases: heap, clustered B-tree,
  NCCI base, CCI base, memory-optimized, external, and a Synapse
  Dedicated table (via mocked engine edition if a live Synapse
  instance is unavailable in CI).
- **SC-002** (Direct path): A TDS trace captured during
  `DELETE FROM mssql.dbo.t WHERE id = 5` against the Docker SQL
  Server contains exactly one outbound SQL_BATCH whose text matches
  `^DELETE FROM .* WHERE id = 5\s*(?:OUTPUT .*)?\s*$`. No preceding
  SELECT. Same shape for `UPDATE`.
- **SC-003** (Staging path correctness): The test
  `test/sql/dml/staging_update_join.test` joins an MSSQL target to
  a DuckDB-local table inside an UPDATE and asserts the resulting
  target row counts via `SELECT COUNT(*) FROM mssql.dbo.target`
  match expected values. PK and physloc variants both pass.
- **SC-004** (Throughput): For a 1M-row UPDATE on a remote Docker
  SQL Server, spec-050 wall-clock is ≤ 0.20× the v0.1.18 baseline
  (≥ 5× speedup). Recorded in `bench_results.md`.
- **SC-005** (Transaction wrap): Three tests in
  `test/sql/dml/dml_transactions.test` pass: autocommit-failure,
  inside-user-txn-failure, and DTC error. Server-side
  `sys.dm_tran_active_transactions` shows no orphaned transactions
  after each test.
- **SC-006** (Savepoint isolation): A test that runs two staging
  UPDATEs inside the same user transaction with distinct UUIDs
  asserts via SQL Server logs that two distinct savepoint names
  were issued.
- **SC-007** (MERGE guard): `test/sql/dml/merge_disabled.test`
  asserts that any `MERGE INTO mssql.…` statement throws the
  documented `NotImplementedException` whose message contains the
  substrings `INSERT INTO` and `UPDATE … FROM`.
- **SC-008** (RETURNING parity): The existing INSERT RETURNING
  test suite stays 100% green; new UPDATE / DELETE RETURNING tests
  (one per direct/staging × update/delete = 4 variants) pass.
- **SC-009** (Code-size budget): Net LOC change ≤ +800 measured by
  `tokei` on the spec-050 PR head vs `main`. Source doc estimated
  +700 net; +800 is the contractual ceiling.
- **SC-010** (Backward compat): Every test under `test/sql/dml/`
  that was green on v0.1.18 and exercises non-MERGE DML stays
  green or is explicitly rewritten with a recorded rationale.

## Assumptions

- Spec 044 (Codec Layer) is merged and provides `FormatSqlLiteral`
  and `AppendDdlColumnType`. Spec 050 does not duplicate this
  work.
- Spec 046 (BCP throughput) is merged and provides the pipelined
  single-connection BCP path. If 046 lands later, the staging
  upload still works but at the v0.1.x throughput floor; SC-004
  is gated on 046.
- The Docker SQL Server image used for integration tests supports
  `%%physloc%%` (true for `mcr.microsoft.com/mssql/server:2022-latest`).
  Synapse-edition behavior is tested by injecting an engine-edition
  override into the connection state for the relevant fixtures.
- `XACT_STATE()` is available on all supported engine editions. (It
  is — back to SQL Server 2005.)
- DuckDB's plan tree shapes between `LogicalUpdate`/`LogicalDelete`
  and the leaf `LogicalGet` are stable for the duration of v0.2.0;
  changes in DuckDB's planner that introduce new operator types in
  this region will require a whitelist update.

## Dependencies

- **Spec 044 (Codec Layer)** — direct-path WHERE literals use
  `FormatSqlLiteral`; staging-table DDL uses `AppendDdlColumnType`.
- **Spec 046 (BCP throughput)** — staging upload uses the
  pipelined writer for single-connection bulk insert.
- **Independent of Spec 042 (Integrated Authentication)** — no
  file overlap.
- **Independent of Spec 043 (Foundation fixes)** — consumed
  transitively via spec 044's simdutf usage.

## Out of Scope (explicit deferrals)

- MERGE server-side execution (replaced by an error guard).
- `ON CONFLICT` / upsert clauses.
- Parallel scan with rowid emission.
- Chunked-commit large DML.
- Direct-path eligibility expansion beyond `LogicalProjection /
  Filter / Get`.
- `%%physloc%%` auto-isolation hardening (`SERIALIZABLE`,
  `TABLOCKX`).
