# refactor(051): DuckDB API migration — IsInterrupted, FlatVector header, SetNullHandling, BindScalarFunctionInput

## Summary

Source-only migration so a single tree compiles against **both** the currently
pinned DuckDB SHA `f480e781694b03105c9281d2564f08adb9a8d4a8` (2026-02-13) and
target SHA `997c2427680e7c0981e312743d27a6c61785ac9e` (2026-05-13, "Clustered
Aggregation #22230"). Spec was merged separately as #123.

Unblocks downstream **oluies/ducklake#1**'s MSSQL build job.

## Four API migrations

| # | DuckDB error on 997c2427 | Fix | Compat |
|---|---|---|---|
| **M1** | `'ClientContext' has no member 'interrupted'; did you mean 'Interrupt'?` | Rename → `IsInterrupted()` | Unconditional (exists on both SHAs) |
| **M2** | `'FlatVector' has not been declared` | New header `<duckdb/common/vector/flat_vector.hpp>` | `__has_include` guard in `mssql_compat.hpp` |
| **M3** | `'ScalarFunction' has no member 'null_handling'; did you mean 'GetNullHandling'?` | Setter `SetNullHandling()` | Unconditional (exists on both SHAs) |
| **M4** | `invalid conversion ... bind_scalar_function_t {aka (*)(BindScalarFunctionInput&)}` | Single-arg struct signature | Macro guard in `mssql_compat.hpp` |

Header-grep evidence on both SHAs is recorded in
[`specs/051-duckdb-api-migration/plan.md`](../specs/051-duckdb-api-migration/plan.md).

## Commits

One commit per concern, in order:

1. `chore(051): add src/include/mssql_compat.hpp for DuckDB API shims` —
   New header with the M2 `__has_include` + M4 `MSSQL_BIND_SCALAR_SIG` /
   `MSSQL_BIND_SCALAR_PROLOGUE` macros.
2. `docs(051): reconcile CLAUDE.md DuckDB-compat description` —
   CLAUDE.md long advertised this header in two places but the file did not
   exist. Replaces the stale `MSSQL_GETDATA_METHOD` paragraph.
3. `refactor(051): migrate context.interrupted to IsInterrupted() (M1)` —
   7 sites, 4 files.
4. `refactor(051): use SetNullHandling() setter (M3)` — 3 sites.
5. `refactor(051): route FlatVector through mssql_compat.hpp (M2)` — 11 files.
6. `refactor(051): bind_scalar_function_t single-arg signature (M4)` — 3 callbacks.

## Discrepancy with spec.md (M2 file list)

Spec.md listed 10 files for M2; the actual file count is 11 — spec missed
`src/tds/encoding/type_converter.cpp` (2 `FlatVector::` sites at lines 266
and 461). The impl PR adds the compat include there too. Spec.md's
"~25 sites total" estimate is unchanged.

## Verification

Header-grep evidence (both SHAs verified — recorded in plan.md):

| New symbol | Exists on f480e781? | Exists on 997c2427? |
|---|---|---|
| `ClientContext::IsInterrupted()` | yes | yes |
| `<duckdb/common/vector/flat_vector.hpp>` | **no** (needs guard) | yes |
| `ScalarFunction::SetNullHandling()` | yes (inherited) | yes (direct) |
| `BindScalarFunctionInput` | **no** (needs guard) | yes |

Build verification:

```bash
# pinned SHA (must still build — no regression)
git -C duckdb checkout f480e781694b03105c9281d2564f08adb9a8d4a8
make clean && make release    # exit 0   ← attach build log

# target SHA (must now build — the point of this PR)
git -C duckdb checkout 997c2427680e7c0981e312743d27a6c61785ac9e
make clean && make release    # exit 0   ← attach build log
```

## What's NOT in this PR

- Submodule gitlink bump — separate decision.
- Patches against DuckDB — extension adapts to DuckDB, not vice versa.
- New dependencies.
- Behavior change of any kind. No SQLLogicTest is modified or added.

## Test plan

Local build verification was deferred to CI: the maintainer's macOS dev box
does not have the project's `vcpkg` bootstrapped locally and falls back to
homebrew's simdutf, which requires C++17 — conflicting with DuckDB's C++11
default. CI builds against the project's vcpkg with the C++11-compatible
simdutf and is the authoritative verification gate.

- [ ] CI green on the pinned SHA `f480e781` (the gitlink isn't bumped, so
      that's what CI builds against by default)
- [ ] Manual verification: `git -C duckdb checkout 997c2427 && make release`
      green on a fresh runner (will run before tag)
- [ ] `make test` (C++ unit tests) green in CI
- [ ] `make integration-test` green if Docker container reachable in CI
- [ ] Reviewer confirms each commit is purely an API-shape change

## Downstream

After this PR merges and the next release tags (e.g. v0.2.1), DuckLake bumps
`specs/001-mssql-metadata-backend/mssql-extension.version` in oluies/ducklake#1.
