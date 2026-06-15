# Quickstart — Reproducing & Verifying the Fix Locally

**Spec**: [spec.md](./spec.md) · **Plan**: [plan.md](./plan.md)

## Prerequisites

- macOS or Linux dev box with the standard mssql-extension build environment (clang/gcc, cmake, ninja, vcpkg).
- A running SQL Server reachable via the env var `MSSQL_TESTDB_DSN` (the existing `make docker-up` works).
- `GEN=ninja` exported (or prepended to every `make` invocation) — required per repo memory.

## 1. Reproduce the bug (before applying the fix)

The repro test was checked in during the issue #126 investigation:

```bash
cd /Users/vgribanov/projects/hugr-lab/mssql-extension
git checkout main                 # pre-fix baseline
GEN=ninja make debug
./build/debug/test/unittest "test/cpp/test_concurrent_reads.cpp"
```

Expected on main:
- Scenarios 1–3 pass.
- **Scenario 4 fails** under UBSan with `runtime error: member call on address … which does not point to an object of type 'MSSQLTableEntry'` (or similar `invalid vptr`) at `EnsurePKLoaded`.

To make the failure deterministic, run under UBSan:

```bash
ASAN_OPTIONS=detect_stack_use_after_return=1 \
UBSAN_OPTIONS=print_stacktrace=1 \
./build/debug/test/unittest "test/cpp/test_concurrent_reads.cpp"
```

## 2. Apply the fix (after `/speckit-tasks` + `/speckit-implement` complete)

```bash
git checkout 052-thread-safe-catalog-entries
GEN=ninja make debug
```

## 3. Verify US1 — concurrent first-load (scenario 4)

```bash
for i in {1..10}; do
  ./build/debug/test/unittest "test/cpp/test_concurrent_reads.cpp" \
    --no-colors 2>&1 | tail -3
done
```

Expected: 10/10 runs pass clean. No `invalid vptr`. No TSan races (when built with `-fsanitize=thread`).

## 4. Verify US2 — concurrent invalidation (scenario 5, new)

Scenario 5 is added by this spec: 4 reader threads in a tight loop of `SELECT * FROM mssql.dbo.t` while a 5th thread calls `mssql_refresh_cache('mssql')` every 50ms for 30 seconds.

```bash
./build/debug/test/unittest "scenario 5" --no-colors
# OR via the new target:
GEN=ninja make test-concurrent-sanitizer
```

Expected: 30s clean run, throughput within ±20% of the no-invalidator baseline.

## 5. Verify US3 — sibling caches (scenario 6, new)

Scenario 6 adds a `duckdb_schemas()` / `duckdb_tables()` loop on top of scenario 5 to exercise `MSSQLCatalog::schema_entries_` and `MSSQLMetadataCache` raw-pointer-handout sites.

```bash
./build/debug/test/unittest "scenario 6" --no-colors
```

Expected: 30s clean.

## 6. Verify no regressions

```bash
# Existing fast-test gate (no SQL Server needed)
GEN=ninja make test

# Integration tests (needs MSSQL_TESTDB_DSN — Docker container is enough)
make docker-up
GEN=ninja make integration-test
make docker-down

# Spec 047 acceptance suites (catalog lifecycle, must still pass)
GEN=ninja make test-spec047-us1
GEN=ninja make test-spec047-us3
GEN=ninja make test-spec047-us-sec
GEN=ninja make test-token-cache-isolation
```

Expected: all green. No new failures.

## 7. Verify perf budget (SC-006)

A microbench is added during implementation:

```bash
# 4 threads × 10000 binds against a hot single-table cache
GEN=ninja make bench-catalog-concurrent

# Compare against v0.2.0 baseline checked into specs/052-thread-safe-catalog-entries/bench_results.md
```

Expected: ±20% vs baseline.

Integration-test runtime:

```bash
time make integration-test 2>&1 | tail -3
# v0.2.0 baseline: ~XX seconds (recorded in bench_results.md during implementation)
```

Expected: ±10% vs baseline.

## 8. Verify graveyard semantics (developer-only)

A C++ unit test `test/cpp/test_catalog_graveyard.cpp` exercises:

1. Insert N entries, hold raw pointers in a vector.
2. Call `Invalidate()`.
3. Verify entries_ is empty, graveyard size is N.
4. Dereference all raw pointers — no UAF.
5. Drop the raw-pointer-holding vector.
6. Destruct catalog.
7. Verify all N entries are freed (use a custom allocator counter or a destructor-counter on the entry).

```bash
./build/debug/test/unittest "graveyard"
```

Expected: pass.

## 9. dbt smoke (manual, not in CI)

The canonical motivating workload — not committed per Clarification Q4, but reproducible:

```bash
pip install dbt-duckdb dbt-core
mkdir /tmp/dbt-smoke && cd /tmp/dbt-smoke
# minimal dbt_project.yml + profiles.yml with mssql catalog attached
dbt run --threads 4
dbt test --threads 4
```

Expected: no segfault, run completes. The issue #126 reproducer in the issue itself is the canonical script — link from the PR description.

## Troubleshooting

- **UBSan trips on `member call on address … null pointer`**: the entry was destroyed before your raw pointer use — fix not applied or anchor missing in your new bind data type. Audit per [contracts/ownership.md § I5](./contracts/ownership.md#i5--bind-data-must-anchor-the-entry).
- **`std::bad_weak_ptr` from `shared_from_this()`**: entry was constructed without going through a `shared_ptr` factory. Audit per I1 — use `make_shared<MSSQLTableEntry>(...)`, not `new MSSQLTableEntry(...)` or stack construction.
- **TSan reports a race on `entries_`**: mutex protocol violated. Check I6 (acquisition order).
- **Graveyard grows in a long-running process** ⇒ expected; documented as a known limitation. If problematic in your workload, file a follow-up issue requesting `mssql_gc_catalog()` API.
