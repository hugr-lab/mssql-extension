# Quickstart: Verify DML Rewrite locally (spec 050)

How a developer reproduces the spec-050 verification path on their
own machine. Five checkpoints: (1) build, (2) unit + SQL tests,
(3) integration tests, (4) throughput benchmark, (5) capability +
transaction-wrap diagnostics.

## Prerequisites

- Docker Desktop or Docker Engine (for the SQL Server test
  container).
- `make`, `cmake` (≥ 3.21), a C++ compiler matching your platform's
  CI matrix.
- vcpkg bootstrapped (`make vcpkg-setup`).
- Ninja optional but recommended: prefix `GEN=ninja` to make
  commands (per project memory).
- Spec 044 and spec 046 already merged on `main`. If not, build
  against the spec-050 PR head — direct path still works without
  046; the throughput benchmark (Checkpoint 4) will fail SC-004
  but every other gate still passes.

## Checkpoint 1 — Build

```bash
GEN=ninja make
```

Expected: green build on the current platform. The new sources
under `src/dml/{delete,update,merge}/` and
`src/dml/mssql_dml_returning_parser.{hpp,cpp}` compile. The
removed legacy files (`mssql_rowid_extractor.cpp`,
`mssql_physical_{update,delete}.cpp`, etc.) are gone from the
source list.

If you see a linker error referencing
`MSSQLReturningParser` (old name), an out-of-tree consumer is
referencing the pre-rename symbol — update it to
`MSSQLDmlReturningParser`. No in-tree consumer should still use
the old name after spec 050.

## Checkpoint 2 — Unit + SQL tests (no SQL Server required)

```bash
# C++ unit test: rowid-strategy resolution decision matrix
GEN=ninja make test-resolve-rowid-strategy

# Existing extension test suite — runs without SQL Server (mocked
# connection):
GEN=ninja make test
```

Expected: `test_resolve_rowid_strategy` covers all 9 (strategy ×
capability) combinations from research.md §R2 and asserts the
exception message for each error case. Full SQL suite stays green
modulo the explicitly-rewritten MERGE tests (replaced by
`test/sql/dml/merge_disabled.test`).

## Checkpoint 3 — Integration tests (SQL Server required)

```bash
# Bring up the test SQL Server:
make docker-up

# Wait for readiness (~10s):
make docker-wait

# Run the full integration suite:
GEN=ninja make integration-test
```

The DML-related test files exercised by this checkpoint:

- `test/sql/dml/dml_capabilities.test` — US1, FR-001..FR-005,
  SC-001. Populates the 7-shape fixture and asserts capabilities.
- `test/sql/dml/direct_update_delete.test` — US2, FR-010..FR-013,
  SC-002. Asserts single-round-trip behavior via a TDS trace
  capture helper.
- `test/sql/dml/staging_update_join.test` — US3, FR-020..FR-028,
  SC-003. Joins MSSQL target to DuckDB-local table; asserts row
  counts and that `#upd_*` table is dropped after success.
- `test/sql/dml/dml_transactions.test` — US4, FR-030..FR-035,
  SC-005, SC-006. Three sub-cases: autocommit-failure,
  user-transaction-failure, DTC error.
- `test/sql/dml/merge_disabled.test` — US5, FR-040, SC-007.
  Asserts the exception message.
- `test/sql/dml/insert_returning.test` (existing) — FR-041,
  FR-042, SC-008. Verifies the parser rename is transparent.
- `test/sql/dml/update_delete_returning.test` (new under US5) —
  FR-043, SC-008.

When a test fails:

- Capability mismatch: look at the populated `TableDmlCapabilities`
  in the connection log. Most likely the engine-edition probe or
  the per-table `sys.indexes` query returned an unexpected value.
- Direct-path test fails because staging fired: dump the plan
  tree in the failure log. A new `LogicalOperator` type appeared
  between `LogicalUpdate`/`LogicalDelete` and `LogicalGet` and the
  whitelist needs an update.
- Staging test fails on rowid mismatch under physloc: a concurrent
  writer modified the target between scan and DML. Re-run; if
  consistently flaky, the test fixture needs a `TABLOCKX` hint or
  to switch to PK-based strategy.

## Checkpoint 4 — Throughput benchmark (`test/bench/bench_dml_throughput.sh`)

Records SC-004's ≥ 5× speedup yardstick.

```bash
# From the spec-050 PR worktree:
bash test/bench/bench_dml_throughput.sh > /tmp/spec050_dml.txt

# From a v0.1.18 worktree (same SQL Server!):
git worktree add ../mssql-0118 v0.1.18
cd ../mssql-0118 && GEN=ninja make && cd -
bash test/bench/bench_dml_throughput.sh --binary ../mssql-0118/build/mssql-extension > /tmp/v0118_dml.txt

# Compute ratio:
echo "v0.1.18:  $(grep elapsed /tmp/v0118_dml.txt)"
echo "spec-050: $(grep elapsed /tmp/spec050_dml.txt)"
```

The benchmark issues:

1. `CREATE TABLE t (id INT PRIMARY KEY, v INT, payload NVARCHAR(40))` on the test SQL Server.
2. Insert 1M rows via CTAS (existing path; not under measurement).
3. `UPDATE mssql.dbo.t SET v = v + 1 WHERE id BETWEEN 1 AND 1000000` — timed.
4. Tear down.

Expected: spec-050 wall-clock ≤ 0.20× v0.1.18's (≥ 5× speedup).
Record both numbers in `bench_results.md`.

## Checkpoint 5 — Capability + transaction-wrap diagnostics

For debugging real-user issues, the spec adds two diagnostic
surfaces:

```sql
-- Show resolved capabilities for a table:
SELECT * FROM mssql_table_capabilities('mssql.dbo.t');
-- Returns: has_primary_key, supports_physloc, physloc_unavailable_reason,
--          pk_column_names, engine_edition.

-- Log every DML transaction boundary (BEGIN / SAVE / COMMIT / ROLLBACK):
SET mssql_dml_log_transactions = true;
-- Run your DML. Boundaries appear in the extension's debug log
-- with savepoint names and @@TRANCOUNT values.
```

If a user reports "UPDATE silently fails inside my transaction",
turn on `mssql_dml_log_transactions = true` and inspect the
sequence:

- Expected (autocommit case): `BEGIN TRAN` → `…` → `COMMIT TRAN`.
- Expected (user-txn case): `SAVE TRANSACTION mssql_dml_<uuid>`
  → `…` → savepoint discard (implicit at user's COMMIT).
- Anomaly: `BEGIN TRAN` issued inside a user transaction →
  bug; the operator's `@@TRANCOUNT` probe misread.
- Anomaly: `ROLLBACK TRANSACTION` (no savepoint name) issued
  inside a user transaction → bug; the operator nuked the outer
  transaction.

## Failure-mode reference

When something breaks, the first questions to ask:

1. **Was the right path selected?** Set
   `mssql_dml_log_transactions = true` and look for the
   `MSSQLStaging…` or `MSSQLDirect…` operator name in the log.
   If the wrong path fired, dump the plan tree at
   `PlanUpdate`/`PlanDelete` time and inspect the whitelist
   decision.
2. **What's the resolved rowid strategy?** Same log, line
   `ResolveRowIdStrategy → PrimaryKey | PhysLoc`. If the answer
   surprises you, query `mssql_table_capabilities('…')` to see
   what the catalog believes.
3. **Was the transaction wrap correct?** Same log, lines
   `@@TRANCOUNT = N` and `XACT_STATE() = M`. The branch taken
   should match research.md §R5's matrix.
4. **Where did the staging table go?** Server-side query:
   `SELECT name FROM tempdb.sys.tables WHERE name LIKE '#upd_%'`.
   On success, none should remain. On failure, none should
   remain either (rollback wipes them). If one persists, the
   operator's cleanup path has a bug.
