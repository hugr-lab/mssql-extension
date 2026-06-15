---

description: "Task list for spec 051 DuckDB API Migration"
---

# Tasks: DuckDB API Migration (f480e781 → 997c2427)

**Input**: Design documents from `/specs/051-duckdb-api-migration/`

**Prerequisites**: spec.md, plan.md (both present)

**Tests**: No new tests. Spec is a behavior-preserving API-shape adjustment;
existing SQLLogicTests and C++ unit tests must pass unmodified.

## Phase 0 — Compat header + CLAUDE.md

### T001 — Create `src/include/mssql_compat.hpp`

Per `plan.md` sketch. Two sections:

- M2: `__has_include` guard selecting `flat_vector.hpp` (new) or
  `vector.hpp` (legacy). Defines sentinel `MSSQL_DUCKDB_HAS_NEW_BIND_INPUT`
  when on new tree.
- M4: `MSSQL_BIND_SCALAR_SIG(name)` + `MSSQL_BIND_SCALAR_PROLOGUE` macros,
  gated on the same sentinel.

No call-site changes in this commit — header lands first so subsequent
commits can include it.

**Commit message**: `chore(051): add src/include/mssql_compat.hpp for DuckDB API shims`

### T002 — Update `CLAUDE.md` to make compat-header claim true

Two edits:

- "**DuckDB**: main branch, supports both stable (1.4.x) and nightly APIs via
  `src/include/mssql_compat.hpp`" — stays as-is; it becomes accurate after T001.
- "Key Architecture Concepts" → **DuckDB API compat** bullet — replace
  with a paragraph that lists the four M1–M4 shims and notes detection
  via `__has_include` on `flat_vector.hpp`. Drop the stale
  `MSSQL_GETDATA_METHOD` line if no such macro actually exists in tree
  (verify with `grep -rn MSSQL_GETDATA_METHOD src/`).

**Commit message**: `docs(051): reconcile CLAUDE.md compat-header description`

## Phase 1 — Unconditional renames (work on both SHAs)

### T003 [P] — M1: `interrupted` → `IsInterrupted()` (7 sites)

Files:

- `src/table_scan/table_scan_execute.cpp:50`
- `src/table_scan/table_scan.cpp:674`
- `src/mssql_functions.cpp:210`
- `src/copy/copy_function.cpp:462,486,508` — note nested
  `context.client.interrupted` → `context.client.IsInterrupted()`
- `src/copy/copy_function.cpp:617`

Verify with: `grep -rnE "\.interrupted\b|->interrupted\b" src/` returns empty.

**Commit message**: `refactor(051): migrate context.interrupted to IsInterrupted() (M1)`

### T004 [P] — M3: `null_handling = X` → `SetNullHandling(X)` (3 sites)

Files:

- `src/mssql_functions.cpp:402`
- `src/catalog/mssql_preload_catalog.cpp:162`
- `src/catalog/mssql_refresh_function.cpp:118`

Verify with: `grep -rnE "null_handling\s*=" src/` returns empty.

**Commit message**: `refactor(051): use SetNullHandling() setter (M3)`

## Phase 2 — Guarded shims via mssql_compat.hpp

### T005 — M2: add `mssql_compat.hpp` include to FlatVector users

Files (each gets `#include "mssql_compat.hpp"` near the existing duckdb
includes; remove direct `vector.hpp` include if it becomes unused):

- `src/codec/binary_codec.cpp`
- `src/codec/boolean_codec.cpp`
- `src/codec/datetime_codec.cpp`
- `src/codec/decimal_codec.cpp`
- `src/codec/float_codec.cpp`
- `src/codec/integer_codec.cpp`
- `src/codec/money_codec.cpp`
- `src/codec/string_codec.cpp`
- `src/codec/uuid_codec.cpp`
- `src/table_scan/table_scan.cpp`

Verify with: `make clean && make release` succeeds against both SHAs (T007).

**Commit message**: `refactor(051): route FlatVector through mssql_compat.hpp (M2)`

### T006 — M4: migrate three bind callbacks via macros

Each callback rewritten as:

```cpp
MSSQL_BIND_SCALAR_SIG(MyBind) {
    MSSQL_BIND_SCALAR_PROLOGUE
    // body unchanged
}
```

Files:

- `src/mssql_functions.cpp:287` — `MSSQLExecBind`
- `src/catalog/mssql_preload_catalog.cpp:22` — `MSSQLPreloadCatalogBind`
- `src/catalog/mssql_refresh_function.cpp:20` — `MSSQLRefreshCacheBind`

Each file gets `#include "mssql_compat.hpp"`.

Verify each callback body uses only `context` and `arguments` — confirmed
during spec audit (none use `bound_function`).

**Commit message**: `refactor(051): bind_scalar_function_t single-arg signature (M4)`

## Phase 3 — Verification

### T007 — Build against both SHAs

```bash
git -C duckdb checkout f480e781694b03105c9281d2564f08adb9a8d4a8
make clean && make release    # exit 0
git -C duckdb checkout 997c2427680e7c0981e312743d27a6c61785ac9e
make clean && make release    # exit 0
```

Capture stderr from both runs; both must end with the extension shared
library produced. After verification, restore the working tree:
`git -C duckdb checkout f480e781694b03105c9281d2564f08adb9a8d4a8` (the
pin we ship with).

### T008 — Run test suites

```bash
make test                     # C++ unit tests
make docker-up
make integration-test         # SQLLogicTest against SQL Server container
make docker-down
```

All must pass unmodified (no SQLLogicTest is edited or added by this spec).

### T009 — Write `pr_description.md`

References each M1–M4 by name, the SHA range tested (f480e781 ↔
997c2427), the two compile logs from T007, and oluies/ducklake#1 as the
downstream consumer.

## Phase 4 — Ship

### T010 — Open PR #B

`gh pr create --base main --head task/051-duckdb-api-migration-impl`
with title:

```
refactor(051): DuckDB API migration — IsInterrupted, FlatVector header,
               SetNullHandling, BindScalarFunctionInput
```

Body from `pr_description.md`.

## Dependencies

- T001 blocks T005, T006 (compat header must exist before includes work).
- T002 has no code dependency (docs).
- T003 / T004 are independent of each other and of T001 (work on both SHAs
  already).
- T005, T006 require T001.
- T007 requires T001–T006 complete.
- T008 requires T007 (no point running tests if build fails).
- T009, T010 require T008.

Parallel markers: `[P]` on T003, T004 — independent files, can land in
either order.
