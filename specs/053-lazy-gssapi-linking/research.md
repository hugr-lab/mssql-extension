# Research: Lazy GSSAPI/Kerberos Linking on Linux

Phase 0 research for spec 053. All decisions feed the plan and `/speckit-tasks`.

## R1 — Why does the extension fail to load today?

**Decision**: Root cause is link-time `DT_NEEDED`, not a packaging or code bug.

**Finding**: `CMakeLists.txt` (lines ~247–261 static, ~289–299 loadable) calls
`target_link_libraries(... ${MSSQL_GSSAPI_LIBRARIES} ${MSSQL_KRB5_BASE_LIBRARIES})`
on non-Apple POSIX. This records `libgssapi_krb5.so.2` and `libkrb5.so.3` as
`DT_NEEDED` entries. The ELF dynamic loader resolves all `DT_NEEDED` libraries
**at `dlopen` time of the extension** (i.e. during `LOAD mssql`), before any
extension code executes — hence the failure even for users who never touch
Kerberos. Confirmed against issue #161 (`linux_arm64`, v1.5.3 community build).

**Rationale**: The community build defaults `ENABLE_KRB5=ON`, so every Linux
community binary inherits the dependency.

## R2 — Can the dependency be made lazy without losing Kerberos?

**Decision**: Yes — `dlopen`/`dlsym` the runtime on first Kerberos use; keep the
**headers** at build time.

**Finding**: Only the `gss_*`/`krb5_*` **function** references emit relocations
that pull a SONAME into `DT_NEEDED`. The GSSAPI types (`gss_ctx_id_t`,
`gss_buffer_desc`, `OM_uint32`, `krb5_context`, …) and constants (flags, status
codes) are header declarations with no link symbols. The OIDs are already
**inline DER bytes** (the existing macOS workaround), so no extern OID data symbol
is referenced either. Therefore: remove `target_link_libraries`, keep
`target_include_directories`, and call every function through a `dlsym`-resolved
pointer → no `DT_NEEDED`, full functionality when the lib is present.

**Alternatives considered**:
- *Linker `-z lazy` / `--as-needed`*: rejected — they affect symbol binding, not
  `DT_NEEDED`; the loader still requires the library present at load.
- *Disable `ENABLE_KRB5` in the community build*: rejected — silently regresses a
  shipped feature; a user who installs the Kerberos package still couldn't use it.
- *Split Kerberos into a separate optional extension*: rejected — large refactor,
  changes the user's `LOAD` story, out of proportion to the fix.

## R3 — Which SONAMEs to dlopen, and fallbacks

**Decision**: Try versioned name first, then the unversioned dev symlink.

- GSSAPI: `libgssapi_krb5.so.2` → `libgssapi_krb5.so`
- krb5 (raw/keytab modes only): `libkrb5.so.3` → `libkrb5.so`

**Rationale**: `.so.2`/`.so.3` are the runtime SONAMEs shipped by the runtime
packages (`libgssapi-krb5-2`, `libkrb5-3`); the unversioned `.so` exists only with
`-dev` packages but is a harmless fallback. `gss_*` symbols come from the gssapi
handle; `krb5_*` symbols are resolved from a separately-dlopened `libkrb5` handle
(transitively-loaded krb5 is not guaranteed visible via the gssapi handle without
`RTLD_GLOBAL`, which we avoid).

## R4 — dlopen flags and thread safety

**Decision**: `RTLD_NOW | RTLD_LOCAL`; first-use guarded by `std::call_once`.

**Rationale**: `RTLD_LOCAL` avoids polluting the global symbol namespace (defensive
vs other extensions). `RTLD_NOW` surfaces a broken/ABI-mismatched library
immediately as a clear error rather than crashing mid-handshake (covers the
"library present but unloadable" edge case). `std::call_once` makes concurrent
first-connection attempts on worker threads load exactly once (FR-009). Handles are
process-lifetime; no `dlclose` needed (and avoids unload-ordering hazards).

## R5 — macOS path

**Decision**: Keep `-framework GSS`; populate the function-pointer table with direct
`&gss_*` addresses (no dlopen on macOS).

**Rationale**: GSS is a macOS **system framework**, always present at
`/System/Library/Frameworks` — there is no missing-library failure mode to fix.
Direct addresses keep one uniform call-site path while avoiding a fragile
framework-path dlopen. `krb5_*` (MIT extensions) are already rejected at
construction on macOS, so the `Krb5Fns` table is never requested there.

## R6 — Error message contract

**Decision**: On load failure, throw a `std::runtime_error` subclass whose message
names the missing object and the install package.

Example: `MSSQL Kerberos auth: could not load 'libgssapi_krb5.so.2'
(dlopen: <dlerror>). Install the system Kerberos runtime — Debian/Ubuntu:
libgssapi-krb5-2; RHEL/Fedora: krb5-libs.`

**Rationale**: Satisfies FR-005/SC-004 in a single message. The `mssql_kerberos_auth_test*`
functions catch this and return it as their result string (FR-006), staying
registered and crash-free when the runtime is absent.

## R7 — ABI / C++ standard constraint

**Decision**: Keep changed files C++11-ABI-compatible.

**Rationale**: Per CLAUDE.md, forcing C++17 on the extension while DuckDB is C++11
causes ODR link failures on GCC/Linux. The shim uses only `std::call_once`,
`std::once_flag`, function pointers, and `std::runtime_error` — all C++11. No
structured bindings, no inline `constexpr static` members.
