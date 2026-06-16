# Implementation Plan: Lazy GSSAPI/Kerberos Linking on Linux

**Branch**: `task/161-271a9d` | **Date**: 2026-06-15 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `specs/053-lazy-gssapi-linking/spec.md`

## Summary

The Linux community binary fails `LOAD mssql` on images without the MIT Kerberos
runtime because spec 042 links `libgssapi_krb5` + `libkrb5` at link time, placing
them in the binary's `DT_NEEDED` (the dynamic loader demands them before any code
runs). This plan removes those link-time dependencies on Linux and resolves the
GSSAPI/krb5 functions at runtime via `dlopen`/`dlsym`, triggered only when
integrated authentication is actually selected. The GSSAPI/krb5 **headers** remain
a build-time dependency (for types, structs, constants — none of which emit link
symbols, since the OIDs are already inline DER bytes). macOS keeps `-framework GSS`
(system framework, always present) and fills the same function-pointer table by
taking the addresses of the linked symbols, so call-site code is identical on both
platforms. Windows (SSPI) is untouched.

## Technical Context

**Language/Version**: C++ (C++11-compatible ABI for ODR with DuckDB on Linux; no C++17-only features in changed files)

**Primary Dependencies**: System GSSAPI/MIT Kerberos runtime (`libgssapi_krb5.so.2`, `libkrb5.so.3`) — **build-time headers only after this change**; `<dlfcn.h>` (`dlopen`/`dlsym`/`dlclose`/`dlerror`) from libc/libdl; macOS GSS system framework (unchanged)

**Storage**: N/A

**Testing**: Existing `test/kerberos/` docker-compose stack (KDC + SQL Server + test-client); C++ unit tests in `test/cpp/`; manual clean-image load test reproducing #161

**Target Platform**: Linux x86_64/ARM64 (primary fix target), macOS ARM64 (unchanged behavior), Windows x64 (unaffected — SSPI)

**Project Type**: DuckDB C++ extension (single project; native TDS client)

**Performance Goals**: No measurable change to non-Kerberos paths (zero new work at extension load or on SQL/Azure-AD connections). One-time `dlopen` (~ms) on first Kerberos connection only.

**Constraints**: Header `tds/auth/iauthenticator.hpp` must remain DuckDB-free (TDS layer reusable). Changed files must stay C++11-ABI-compatible (no structured bindings, no inline `constexpr static` ODR hazards). Thread-safe lazy init (worker threads may open the first Kerberos connection concurrently).

**Scale/Scope**: ~21 GSSAPI/krb5 functions to route through the shim (8 `gss_*`, 13 `krb5_*` + 4 test-only `krb5_*`); 1 new shim translation unit + header; edits to `krb5_authenticator.cpp`, `krb5_test_function.cpp`, `CMakeLists.txt`. No new user-facing surface.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Assessment |
|-----------|------------|
| **I. Native and Open** | ✅ Strengthens it — removes a forced runtime dependency on an external library; nothing is redistributed; the system Kerberos library is dlopened only if the user has it and asks for Kerberos. |
| **II. Streaming First** | ✅ N/A — no result-path change. |
| **III. Correctness over Convenience** | ✅ Aligns — when Kerberos is requested but unavailable, the system fails **explicitly** with a clear, actionable message (which package to install) rather than a cryptic loader error or silent fallback. |
| **IV. Explicit State Machines** | ✅ The lazy-load is an explicit, single, testable step in the connection-auth lifecycle (first `InitialBytes()`); load success/failure is observable at the right point. No hidden state. |
| **V. DuckDB-Native UX** | ✅ N/A. |
| **VI. Incremental Delivery** | ✅ N/A (defect fix); independently testable via clean-image load + existing Kerberos stack. |

**Result: PASS.** No violations; Complexity Tracking not required.

## Project Structure

### Documentation (this feature)

```text
specs/053-lazy-gssapi-linking/
├── plan.md              # This file
├── spec.md              # Feature specification
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/
│   └── gssapi_runtime.md  # Loader interface + error contract
└── checklists/
    └── requirements.md  # Spec quality checklist (from /speckit-specify)
```

### Source Code (repository root)

```text
src/
├── include/tds/auth/
│   ├── iauthenticator.hpp          # UNCHANGED (must stay DuckDB-free)
│   ├── krb5_authenticator.hpp      # EDIT: keep GSSAPI headers (types); no functional symbol use
│   └── gssapi_runtime.hpp          # NEW: function-pointer tables + Get accessors + load error type
└── tds/auth/
    ├── gssapi_runtime.cpp          # NEW: dlopen/dlsym (Linux) or direct-address (macOS) table population
    ├── krb5_authenticator.cpp      # EDIT: route all gss_*/krb5_* calls through the runtime table
    └── krb5_test_function.cpp      # EDIT: route krb5_*/gss_* test calls through the runtime table

CMakeLists.txt                      # EDIT: Linux — keep include dirs, DROP target_link_libraries for
                                    #       gssapi/krb5; add gssapi_runtime.cpp to sources; link dl.
                                    #       macOS — unchanged (keep -framework GSS).
```

**Structure Decision**: Single-project DuckDB extension. The shim is a new, self-contained pair in the existing `src/tds/auth/` layer (where all integrated-auth code already lives, behind `MSSQL_ENABLE_KRB5`). No new top-level directories.

## Implementation Approach (detail for /speckit-tasks)

### 1. Runtime loader (`gssapi_runtime.{hpp,cpp}`)
- Two POD structs of function pointers: `GssApiFns` (8 `gss_*`) and `Krb5Fns` (13 + 4 test `krb5_*`). Signatures taken verbatim from the GSSAPI/krb5 headers (still included).
- `const GssApiFns &GetGssApi();` and `const Krb5Fns &GetKrb5();` — each loads on first call via `std::call_once` (thread-safe, FR-009) and caches.
- **Linux**: `dlopen` with `RTLD_NOW | RTLD_LOCAL`. Try SONAMEs in order: `libgssapi_krb5.so.2` → `libgssapi_krb5.so`; krb5: `libkrb5.so.3` → `libkrb5.so`. `dlsym` each needed symbol; if the handle or any symbol is missing, throw `Krb5RuntimeUnavailable` carrying the missing library/symbol + `dlerror()`.
- **macOS** (`__APPLE__`): populate `GssApiFns` directly with `&gss_*` (still linked via framework). `Krb5Fns` is never requested on macOS (keytab/raw already rejected at construction).
- Error type: a `std::runtime_error` subclass whose message names the missing `.so` and recommends the package (`libgssapi-krb5-2` on Debian/Ubuntu, `krb5-libs` on RHEL/Fedora) — satisfies FR-005/SC-004.

### 2. Authenticator rewiring (`krb5_authenticator.cpp`)
- At the top of `AcquireCredentials()` / `DoSecContextStep()` (the GSSAPI entry points), grab `const auto &gss = GetGssApi();` and replace `gss_xxx(` call sites with `gss.xxx(`. Same for krb5 raw-mode via `GetKrb5()`.
- The first `GetGssApi()` is reached on the first `InitialBytes()` — the single lazy-load trigger. The `Krb5Authenticator` constructor still does **no** GSSAPI calls (unchanged), so construction never loads the library.
- `ThrowGssError` uses `gss.display_status` from the table.

### 3. Test functions (`krb5_test_function.cpp`)
- Route its `krb5_*`/`gss_*` calls through the tables. Wrap the body so a `Krb5RuntimeUnavailable` becomes the function's returned string (FR-006) instead of propagating — keeps the function registered and crash-free when the runtime is absent.

### 4. CMake (`CMakeLists.txt`, both static + loadable targets)
- Add `src/tds/auth/gssapi_runtime.cpp` to `EXTENSION_SOURCES` under `if(MSSQL_KRB5_AVAILABLE)` (next to `krb5_authenticator.cpp`, line ~217).
- Non-Apple branch (lines ~251–259 and ~293–297): **keep** `target_include_directories` (headers needed at build); **remove** the `target_link_libraries(... ${MSSQL_GSSAPI_LIBRARIES} ${MSSQL_KRB5_BASE_LIBRARIES})`. Add `${CMAKE_DL_LIBS}` to the link line (for `dlopen` on older glibc).
- Apple branch: unchanged (`target_link_libraries(... ${MSSQL_GSS_FRAMEWORK})`).
- Leave `pkg_check_modules` discovery as-is (still needed for include dirs); update the nearby comment to note link-time deps are intentionally dropped on Linux.

### Verification gates
- `ldd build/release/extension/mssql/mssql.duckdb_extension | grep -i krb5` → empty (SC-002).
- Clean-image (#161 repro) load test → succeeds (SC-001/SC-005).
- `test/kerberos/` stack → all pass (SC-003).
- Unit test asserting the unavailable-runtime error message contains the lib name + package (SC-004).

## Complexity Tracking

> No constitution violations — section intentionally empty.
