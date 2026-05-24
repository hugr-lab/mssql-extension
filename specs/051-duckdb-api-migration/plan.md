# Implementation Plan: DuckDB API Migration

**Branch**: `task/051-duckdb-api-migration-impl` (code PR)
| **Date**: 2026-05-24 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from
`/specs/051-duckdb-api-migration/spec.md`

## Summary

The four migrations in spec.md split cleanly into two compatibility classes:

| Migration | Strategy | Lives in |
|---|---|---|
| M1 `IsInterrupted()` | Unconditional rename | call sites |
| M2 `FlatVector` header | `__has_include` guard | `mssql_compat.hpp` (new) |
| M3 `SetNullHandling()` | Unconditional rename | call sites |
| M4 `BindScalarFunctionInput` | `#if` macro adapter | `mssql_compat.hpp` (new) |

Two unconditional renames, two guarded shims. The shims live behind two
macros that read naturally at call sites; we do not introduce a virtual
abstraction layer.

## New file: `src/include/mssql_compat.hpp`

CLAUDE.md already advertises this header in two places (Technology line +
"Key Architecture Concepts" bullet). The file does not currently exist —
this spec ends that lie.

Sketch:

```cpp
// src/include/mssql_compat.hpp
#pragma once

// ----- M2: FlatVector header relocation -----
#if __has_include(<duckdb/common/vector/flat_vector.hpp>)
#include <duckdb/common/vector/flat_vector.hpp>
#define MSSQL_DUCKDB_HAS_NEW_BIND_INPUT 1
#else
#include <duckdb/common/types/vector.hpp>
// FlatVector ships in vector.hpp on pre-997c2427 DuckDB.
#endif

// ----- M4: bind_scalar_function_t signature -----
#ifdef MSSQL_DUCKDB_HAS_NEW_BIND_INPUT
#define MSSQL_BIND_SCALAR_SIG(name) \
    static duckdb::unique_ptr<duckdb::FunctionData> name( \
        duckdb::BindScalarFunctionInput &input)
#define MSSQL_BIND_SCALAR_PROLOGUE                                  \
    auto &context = input.GetClientContext();                       \
    auto &arguments = input.GetArguments();                         \
    (void)context;                                                  \
    (void)arguments;
#else
#define MSSQL_BIND_SCALAR_SIG(name)                                 \
    static duckdb::unique_ptr<duckdb::FunctionData> name(           \
        duckdb::ClientContext &context,                             \
        duckdb::ScalarFunction & /*bound_function*/,                \
        duckdb::vector<duckdb::unique_ptr<duckdb::Expression>>      \
            &arguments)
#define MSSQL_BIND_SCALAR_PROLOGUE  /* no-op */
#endif
```

Notes:

- The `(void)x` no-ops in the new prologue keep the macro safe at use
  sites that read `arguments` but conditionally branch over `context`.
- `MSSQL_DUCKDB_HAS_NEW_BIND_INPUT` is gated on the presence of
  `flat_vector.hpp` because M2 and M4 landed together in DuckDB main. If
  they later diverge, this header grows a second sentinel.

## Call-site rewrites

### M1 (7 sites): `interrupted` → `IsInterrupted()`

```diff
- if (context.interrupted) {
+ if (context.IsInterrupted()) {
```

For the three `context.client.interrupted` sites in
`src/copy/copy_function.cpp`, the outer `context` is an
`ExecutionContext` — `context.client` is the `ClientContext` — so the
rewrite is `context.client.IsInterrupted()`.

### M2 (~25 sites): no source change, only includes

Each `src/codec/*_codec.cpp` and `src/table_scan/table_scan.cpp` adds:

```diff
+ #include "mssql_compat.hpp"
```

(Replacing or accompanying the existing `vector.hpp` include — TBD per
file to avoid the unused-include lint.)

### M3 (3 sites): `null_handling = X` → `SetNullHandling(X)`

```diff
- func.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
+ func.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
```

### M4 (3 callbacks): signature macro

```diff
- static unique_ptr<FunctionData> MSSQLExecBind(
-     ClientContext &context, ScalarFunction &bound_function,
-     vector<unique_ptr<Expression>> &arguments) {
+ MSSQL_BIND_SCALAR_SIG(MSSQLExecBind) {
+     MSSQL_BIND_SCALAR_PROLOGUE
      // body unchanged — uses `context` and `arguments`
  }
```

The body of each callback is unchanged because all three only read
`context` and `arguments`.

## CLAUDE.md updates

Two reconciliations in `CLAUDE.md`:

1. The "**DuckDB**: main branch, supports both stable (1.4.x) and nightly
   APIs via `src/include/mssql_compat.hpp`" line stays — it becomes true.
2. The "Key Architecture Concepts" bullet "**DuckDB API compat**: Auto-detected
   at CMake time. `MSSQL_GETDATA_METHOD` macro handles GetData vs
   GetDataInternal." is replaced/extended with a paragraph noting the four
   spec-051 shims (M1–M4) and that detection is via `__has_include`, not
   CMake.

## Forward-compat audit (recorded)

| New symbol | Exists on f480e781? | Exists on 997c2427? |
|---|---|---|
| `ClientContext::IsInterrupted()` | yes (`client_context.hpp:100`) | yes (`client_context.hpp:106`) |
| `<duckdb/common/vector/flat_vector.hpp>` | **no** | yes (`flat_vector.hpp:76`) |
| `ScalarFunction::SetNullHandling()` | yes (inherited, `function.hpp:199`) | yes (direct, `scalar_function.hpp:233`) |
| `BindScalarFunctionInput` | **no** | yes (`scalar_function.hpp:514`) |

Verification commands (rerun from the worktree):

```bash
git -C duckdb checkout f480e781694b03105c9281d2564f08adb9a8d4a8 -- src/include/
grep -nE "IsInterrupted|SetNullHandling|BindScalarFunctionInput|FlatVector" \
  duckdb/src/include/duckdb/main/client_context.hpp \
  duckdb/src/include/duckdb/function/scalar_function.hpp \
  duckdb/src/include/duckdb/function/function.hpp \
  duckdb/src/include/duckdb/common/types/vector.hpp
git -C duckdb cat-file -e \
  f480e781:src/include/duckdb/common/vector/flat_vector.hpp || \
  echo "MISSING on f480e781 — needs __has_include guard"

git -C duckdb checkout 997c2427680e7c0981e312743d27a6c61785ac9e -- src/include/
# Same greps; verify each symbol exists.
```

## Verification (PR #B)

```bash
# Original pin still builds
git -C duckdb checkout f480e781694b03105c9281d2564f08adb9a8d4a8
make clean && make release          # exits 0

# Target SHA builds
git -C duckdb checkout 997c2427680e7c0981e312743d27a6c61785ac9e
make clean && make release          # exits 0

# Tests pass
make test                           # unit tests green
# Optional, if SQL Server container available:
make docker-up && make integration-test && make docker-down
```

## Risks (per spec.md §Risks)

- DuckDB may rename more APIs in subsequent SHAs not yet audited; this spec
  only covers the four currently breaking. Future shims land beside M2/M4
  in `mssql_compat.hpp`.
- The macro-based approach for M4 trades a small loss of IDE go-to-symbol
  fidelity (the function signature is hidden inside a macro) for keeping
  the body identical on both SHAs. Alternative — write the callback twice
  with `#if` — is rejected as more invasive.

## Out of Scope (per spec.md)

Submodule gitlink bump, DuckDB patches, other API differences, behavior
changes, DuckLake changes.
