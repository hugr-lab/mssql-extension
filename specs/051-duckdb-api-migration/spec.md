# Feature Specification: DuckDB API Migration (f480e781 → 997c2427)

**Feature Branch**: `task/051-duckdb-api-migration-spec` (docs) → `task/051-duckdb-api-migration-impl` (code)

**Created**: 2026-05-24

**Status**: Draft (docs PR)

**Input**: External request from downstream consumer **oluies/ducklake PR #1**.
DuckLake needs the MSSQL extension to build against newer DuckDB SHAs than the
gitlink currently pins. Four DuckDB public APIs used by this extension were
renamed/relocated/reshaped between SHA `f480e781694b03105c9281d2564f08adb9a8d4a8`
(2026-02-13, currently pinned) and SHA
`997c2427680e7c0981e312743d27a6c61785ac9e` (2026-05-13, "Clustered Aggregation
(#22230)"). The extension compiles cleanly against the former and fails on the
latter. DuckLake's MSSQL build job is blocked.

## Overview

Adapt the extension's source so a single tree compiles against **both**
SHAs without changing behavior. This is a pure API-shape adjustment — no new
features, no new dependencies, no submodule bump, no refactoring beyond what
the four migrations directly require.

Goal: after this spec ships and a new extension version tags, DuckLake bumps
its `specs/001-mssql-metadata-backend/mssql-extension.version` and PR #1
unblocks.

## Scope

**In scope**: the four DuckDB API-shape adjustments enumerated in §Migrations.
A new compatibility header `src/include/mssql_compat.hpp` (already advertised
by `CLAUDE.md` but missing from the tree) houses the two `#if`-guarded
adjustments. `CLAUDE.md` is updated to make its compat-header description
honest.

**Out of scope**:

- Bumping `duckdb/` submodule gitlink (separate, larger decision).
- Adding patches against DuckDB itself.
- Any DuckDB API differences other than the four below.
- Behavior change of any kind. Existing SQLLogicTests must pass unmodified.
- Touching DuckLake — that repo bumps the version separately once we tag.

## Migrations

### M1. `ClientContext::interrupted` → `IsInterrupted()`

**DuckDB compiler error** (verbatim) when building against 997c2427:

```
error: 'class duckdb::ClientContext' has no member named 'interrupted';
       did you mean 'Interrupt'?
```

**Header evidence**:

- `duckdb/main/client_context.hpp` on f480e781 — declares
  `DUCKDB_API bool IsInterrupted() const;` (already public).
- Same header on 997c2427 — declares the same `IsInterrupted()`.

**Conclusion**: unconditional rename works on both SHAs. No compat guard.

**Call sites** (7 total):

- `src/table_scan/table_scan_execute.cpp:50` — `if (context.interrupted)`
- `src/table_scan/table_scan.cpp:674` — `if (context.interrupted)`
- `src/mssql_functions.cpp:210` — `if (context.interrupted)`
- `src/copy/copy_function.cpp:462,486,508` — `if (context.client.interrupted)`
- `src/copy/copy_function.cpp:617` — `if (context.interrupted)`

### M2. `FlatVector` header moved → `duckdb/common/vector/flat_vector.hpp`

**DuckDB compiler error** (verbatim) when building against 997c2427:

```
error: 'FlatVector' has not been declared
```

**Header evidence**:

- f480e781: `struct FlatVector` declared at
  `duckdb/common/types/vector.hpp:444`.
- 997c2427: `struct FlatVector` declared at
  `duckdb/common/vector/flat_vector.hpp:76`. The legacy
  `duckdb/common/types/vector.hpp` no longer transitively exposes it.
- `git cat-file -e f480e781:src/include/duckdb/common/vector/flat_vector.hpp`
  → "path does not exist". The new header is target-SHA-only.

**Conclusion**: `__has_include` guard required. The compat header includes
the new path when present, falls back to the legacy path otherwise.

**Call sites** (~25 total across):

- `src/table_scan/table_scan.cpp` (2)
- `src/codec/{binary,boolean,datetime,decimal,float,integer,money,string,uuid}_codec.cpp`

### M3. `ScalarFunction::null_handling` field write → `SetNullHandling()`

**DuckDB compiler error** (verbatim) when building against 997c2427:

```
error: 'class duckdb::ScalarFunction' has no member named 'null_handling';
       did you mean 'GetNullHandling'?
```

**Header evidence**:

- f480e781: `BaseScalarFunction` (the parent) declares both the public field
  `null_handling` AND the inherited setter
  `void SetNullHandling(FunctionNullHandling)` at
  `duckdb/function/function.hpp:199–201`.
- 997c2427: `ScalarFunction` declares `SetNullHandling()` as a direct member
  at `duckdb/function/scalar_function.hpp:233`. The field moved behind
  `properties.null_handling`.

**Conclusion**: unconditional `SetNullHandling()` works on both SHAs. No compat
guard.

**Call sites** (3 total, all identical):

- `src/mssql_functions.cpp:402`
- `src/catalog/mssql_preload_catalog.cpp:162`
- `src/catalog/mssql_refresh_function.cpp:118`

All three are:
`func.null_handling = FunctionNullHandling::SPECIAL_HANDLING;`

### M4. `bind_scalar_function_t` signature: 3-arg → single `BindScalarFunctionInput&`

**DuckDB compiler error** (verbatim) when building against 997c2427:

```
error: invalid conversion from
  'unique_ptr<FunctionData> (*)(ClientContext&, ScalarFunction&,
                                vector<unique_ptr<Expression>>&)'
to
  'bind_scalar_function_t' {aka 'unique_ptr<FunctionData> (*)(BindScalarFunctionInput&)'}
```

**Header evidence**:

- f480e781: `bind_scalar_function_t` =
  `unique_ptr<FunctionData> (*)(ClientContext &, ScalarFunction &,
  vector<unique_ptr<Expression>> &)`
  at `duckdb/function/scalar_function.hpp:100`. **No `BindScalarFunctionInput`
  class exists.**
- 997c2427: `bind_scalar_function_t` =
  `unique_ptr<FunctionData> (*)(BindScalarFunctionInput &)`
  at `duckdb/function/scalar_function.hpp:138`. `BindScalarFunctionInput`
  class is at line 514 with **private** data members and public accessors
  `GetClientContext()`, `GetBoundFunction()`, `GetArguments()`,
  `GetBinder()`, `HasBinder()`.

**Conclusion**: `#if` guard required. The compat header defines two macros:
`MSSQL_BIND_SCALAR_SIG(name)` for the function signature and
`MSSQL_BIND_SCALAR_PROLOGUE` for the per-version unpacking of `context` and
`arguments`. Each bind callback then reads naturally on both SHAs.

**Call sites** (3 callbacks):

- `MSSQLExecBind` — `src/mssql_functions.cpp:287`
- `MSSQLPreloadCatalogBind` — `src/catalog/mssql_preload_catalog.cpp:22`
- `MSSQLRefreshCacheBind` — `src/catalog/mssql_refresh_function.cpp:20`

All three callbacks use only `context` and `arguments` — none read
`bound_function`. The `ScalarFunction` vs `BoundScalarFunction` parameter
type divergence on 997c2427 therefore does not affect us.

## Acceptance Criteria

1. **Builds clean against `997c2427`**:
   ```bash
   git -C duckdb checkout 997c2427680e7c0981e312743d27a6c61785ac9e
   make clean && make release    # exit 0
   ```
2. **Builds clean against the existing pin `f480e781`** (no regression):
   ```bash
   git -C duckdb checkout f480e781694b03105c9281d2564f08adb9a8d4a8
   make clean && make release    # exit 0
   ```
3. **All existing tests pass unmodified**: `make test` green;
   `make integration-test` green if a SQL Server container is available.
4. **No behavior change**: no SQLLogicTest is modified or added by this spec.
5. **`CLAUDE.md` reconciled**: the "DuckDB" technology line and the "DuckDB
   API compat" architecture bullet accurately describe the (now-created)
   `src/include/mssql_compat.hpp`.

## Risks

- Future DuckDB SHAs may rename more APIs; this spec only covers the four
  that currently break. The compat header is designed so additional shape
  shims can land beside the existing two.
- `__has_include` is C++17, which DuckDB extensions support (DuckDB ships
  C++11-default but `__has_include` is a preprocessor feature available in
  GCC/Clang/MSVC regardless of `-std=`).
- `BoundScalarFunction` (the new bound-form type on 997c2427) is not
  exposed via our bind callbacks because none of them read `bound_function`.
  If a future bind callback needs to mutate `bound_function`, the abstraction
  must extend.

## Out of Scope (explicit)

- `bind_scalar_function_extended_t` migration — that already-extended
  callback shape pre-dated `BindScalarFunctionInput` and is not used by this
  extension.
- DuckDB version macro auto-detection — `__has_include` on the new
  `flat_vector.hpp` is the single sentinel for both M2 and M4 (they
  landed in DuckDB together; see git log between f480e781 and 997c2427).
  If they later split, the compat header gains a second sentinel.

## Downstream

- **oluies/ducklake PR #1** is the proximate consumer. After this spec's
  impl PR merges and we tag (e.g. v0.2.1), DuckLake bumps
  `specs/001-mssql-metadata-backend/mssql-extension.version`.
