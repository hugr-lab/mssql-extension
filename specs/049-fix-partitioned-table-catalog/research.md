# Research: Fix Partitioned Table Catalog

**Phase 0 Output** | **Date**: 2026-05-19

## Findings

No NEEDS CLARIFICATION items existed in the spec. The investigation that produced this spec already established the bug location, the SQL Server semantics, and the candidate fix shapes.

### Decision 1: Use a pre-aggregated derived table

**Decision**: Replace `LEFT JOIN sys.partitions p ON o.object_id = p.object_id AND p.index_id IN (0, 1)` with:

```sql
LEFT JOIN (
    SELECT object_id, SUM(rows) AS rows
    FROM sys.partitions
    WHERE index_id IN (0, 1)
    GROUP BY object_id
) p ON p.object_id = o.object_id
```

**Rationale**:
- One row per `object_id` regardless of partition count → no more column-row multiplication in the outer `INNER JOIN sys.columns`.
- `SUM(rows)` gives the correct table-level cardinality estimate (the very thing the outer projection wants in `approx_rows`).
- Pure SQL, no schema dependency, no permission change vs. the existing query.
- Optimizer can stream-aggregate or hash-aggregate this trivially; cost is comparable to the existing per-partition scan.

**Alternatives considered**:

| Option | Why rejected |
| ------ | ------------ |
| **`OUTER APPLY (SELECT SUM(rows) ...)`** | Works, but introduces a correlated subquery shape. On older optimizer builds (SQL Server 2016/2017 RTM, before the legacy CE fixes) the planner sometimes generates a nested loop with per-row aggregate instead of a single aggregate. Derived-table form is more uniformly optimized across versions. |
| **`SELECT DISTINCT` outer wrapper** | Wrong semantics: the columns row already varies by partition count via `p.rows`, so DISTINCT on the projected columns doesn't dedupe. Would require selecting an artificial `MAX(p.rows)` per group, which is not the right answer (we want SUM, not MAX). |
| **Drop `sys.partitions` join from the column queries; fetch row counts separately** | Two round trips per single-table load; doubles the metadata query count for an already-hot path. Goes against spec 033's incremental-cache design which collapsed everything into single round trips. |
| **Switch to `sys.dm_db_partition_stats`** | Same per-partition granularity — switching DMV doesn't avoid aggregation. Adds a permission requirement (`VIEW DATABASE STATE`) that some hardened read-only ETL accounts don't have. No win. |
| **Switch to `sys.dm_db_partition_stats` aggregated** | Same aggregation needed as `sys.partitions`. Adds permission requirement for no benefit. |
| **Use `OBJECTPROPERTYEX(object_id, 'Cardinality')`** | Not documented as a public OBJECTPROPERTYEX value; behaviour varies across versions. Not portable. |
| **Use `sys.dm_db_stats_properties`** | Requires statistics to be present. Newly-created tables with no stats return NULL. Less reliable than `sys.partitions.rows` (which is maintained per insert/update/delete). |

### Decision 2: Apply the fix to all three SQL templates in one change

**Decision**: Modify `TABLE_DISCOVERY_SQL_TEMPLATE`, `SINGLE_TABLE_METADATA_SQL_TEMPLATE`, and `BULK_METADATA_SCHEMA_SQL_TEMPLATE` in the same commit.

**Rationale**:
- The three queries are tightly coupled: they're the three load paths of the same metadata cache. Fixing one but not the others means a partitioned table works when accessed one way (`SELECT * FROM <table>`) but crashes when accessed another (`mssql_preload_catalog(...)` or `SHOW ALL TABLES` populating the bulk cache).
- The fix shape is identical for all three templates — one mechanical replacement.
- Splitting across multiple PRs would leave a confusing intermediate state where some catalog operations work and others don't on the same table.

**Alternatives considered**:
- **MVP-only fix (SINGLE_TABLE only)**: Would resolve issue #85 but leave `mssql_preload_catalog` and `SHOW ALL TABLES` broken for partitioned tables. Not shipped.

### Decision 3: Test fixture lives in its own init SQL file

**Decision**: Create `docker/init/init-partitioned-tables.sql` rather than appending to `docker/init/init.sql`.

**Rationale**:
- `init.sql` is 770 lines. Adding partition function + partition scheme + filegroup-dependent objects to the bottom risks ordering issues (partition function must exist before partition scheme; partition scheme must exist before the indexed table).
- Partitioning depends on `PRIMARY` filegroup being writable. Test isolation: if this fixture ever fails on a CI flavour that has a restricted filegroup setup, the rest of `init.sql` is unaffected.
- Mirrors `init-transaction-tests.sql` precedent (already a separate file for the same reason).

**Alternatives considered**:
- **Append to `init.sql`**: Couples the partition fixture to the main init; ordering risk; harder to skip on CI flavours where partitioning isn't testable.
- **Create the partitioned table at test setup time via `mssql_exec`**: SQLLogicTest's per-test setup runs inside the test runner; can't reliably hold the partition function across test runs. Container-init time is the right boundary.

### Decision 4: Optional C++ unit test guards the templates

**Decision**: Add `test/cpp/test_metadata_cache_queries.cpp` asserting that each template literal contains the substrings `"GROUP BY"` and `"SUM(rows)"`.

**Rationale**:
- The bug was a subtle one: the query "worked" for non-partitioned tables, so the unit-test coverage for these templates only caught the single-partition case. A future maintainer rewriting the query to avoid the derived-table form might inadvertently drop the aggregation.
- A 5-line unit test that checks for the aggregation marker is cheap and makes the regression caught at unit-test time (fast) rather than integration-test time (requires docker-up).
- The test is intentionally weak (substring check) — it doesn't validate the *correctness* of the aggregation (integration test does that), just the *presence* of the construct.

**Alternatives considered**:
- **Parse the SQL and check the AST**: Heavyweight; would require a SQL parser as a test-only dep. Not worth it for a guard test.
- **Skip the unit test entirely**: Acceptable — the integration test catches real regressions. Listed as P3 / "optional" in plan.md.

### Decision 5: No new vcpkg dependency

**Decision**: The fix uses only standard SQL constructs and the existing test infrastructure (SQLLogicTest, Catch2). No new vcpkg packages, no new third-party dependencies.

**Rationale**: This is a bug fix; introducing dependencies for a fix would be inappropriate scope creep. Spec 044 just consolidated dependencies — adding new ones here would partially undo that work.

## Background Reading

- [Microsoft Docs: sys.partitions (Transact-SQL)](https://learn.microsoft.com/en-us/sql/relational-databases/system-catalog-views/sys-partitions-transact-sql) — confirms the per-partition row granularity and that `rows` is BIGINT.
- [Microsoft Docs: Partitioned Tables and Indexes](https://learn.microsoft.com/en-us/sql/relational-databases/partitions/partitioned-tables-and-indexes) — confirms the `RANGE RIGHT FOR VALUES (...)` creates N+1 partitions for N boundary values.
- [Microsoft Docs: index_id semantics](https://learn.microsoft.com/en-us/sql/relational-databases/system-catalog-views/sys-indexes-transact-sql) — confirms index_id=0 is heap, index_id=1 is clustered index, index_id≥2 is non-clustered.
- Issue #85 (https://github.com/hugr-lab/mssql-extension/issues/85) — original bug report with reproducer.
- Spec 033 (specs/033-fix-catalog-scan/) — context on the catalog/metadata caching architecture being patched.
